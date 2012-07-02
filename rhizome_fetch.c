/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <time.h>
#include "serval.h"
#include "rhizome.h"

extern int sigPipeFlag;
extern int sigIoFlag;


typedef struct rhizome_file_fetch_record {
  struct sched_ent alarm;
  rhizome_manifest *manifest;
  char fileid[RHIZOME_FILEHASH_STRLEN + 1];
  FILE *file;
  
  char request[1024];
  int request_len;
  int request_ofs;
  
  long long file_len;
  long long file_ofs;

  int state;
  
#define RHIZOME_FETCH_CONNECTING 1
#define RHIZOME_FETCH_SENDINGHTTPREQUEST 2
#define RHIZOME_FETCH_RXHTTPHEADERS 3
#define RHIZOME_FETCH_RXFILE 4
  
} rhizome_file_fetch_record;

struct callback_stats fetch_stats;

/* List of queued transfers */
#define MAX_QUEUED_FILES 4
int rhizome_file_fetch_queue_count=0;
rhizome_file_fetch_record file_fetch_queue[MAX_QUEUED_FILES];
/* 
   Queue a manifest for importing.

   There are three main cases that can occur here:

   1. The manifest has no associated file (filesize=0);
   2. The associated file is already in our database; or
   3. The associated file is not already in our database, and so we need
   to fetch it before we can import it.

   Cases (1) and (2) are more or less identical, and all we need to do is to
   import the manifest into the database.

   Case (3) requires that we fetch the associated file.

   This is where life gets interesting.
   
   First, we need to make sure that we can free up enough space in the database
   for the file.

   Second, we need to work out how we are going to get the file. 
   If we are on an IPv4 wifi network, then HTTP is probably the way to go.
   If we are not on an IPv4 wifi network, then HTTP is not an option, and we need
   to use a Rhizome/Overlay protocol to fetch it.  It might even be HTTP over MDP
   (Serval Mesh Datagram Protocol) or MTCP (Serval Mesh Transmission Control Protocol
   -- yet to be specified).

   For efficiency, the MDP transfer protocol should allow multiple listeners to
   receive the data. In contrast, it would be nice to have the data auth-crypted, if
   only to deal with packet errors (but also naughty people who might want to mess
   with the transfer.

   For HTTP over IPv4, the biggest problem is that we don't know the IPv4 address of
   the sender, or in fact that the link is over IPv4 and thus that HTTP over IPv4 is
   an option.  We probably need to be passed this information.  This has since been
   incorporated.
*/

/* As defined below uses 64KB */
#define RHIZOME_VERSION_CACHE_NYBLS 2 /* 256=2^8=2nybls */
#define RHIZOME_VERSION_CACHE_SHIFT 1
#define RHIZOME_VERSION_CACHE_SIZE 128
#define RHIZOME_VERSION_CACHE_ASSOCIATIVITY 16
typedef struct rhizome_manifest_version_cache_slot {
  unsigned char idprefix[24];
  long long version;
} rhizome_manifest_version_cache_slot;
rhizome_manifest_version_cache_slot rhizome_manifest_version_cache
[RHIZOME_VERSION_CACHE_SIZE][RHIZOME_VERSION_CACHE_ASSOCIATIVITY];

int rhizome_manifest_version_cache_store(rhizome_manifest *m)
{
  int bin=0;
  int slot;
  int i;

  char *id=rhizome_manifest_get(m,"id",NULL,0);
  if (!id) return 1; // dodgy manifest, so don't suggest that we want to RX it.

  /* Work out bin number in cache */
  for(i=0;i<RHIZOME_VERSION_CACHE_NYBLS;i++)
    {
      int nybl=hexvalue(id[i]);
      bin=(bin<<4)|nybl;
    }
  bin=bin>>RHIZOME_VERSION_CACHE_SHIFT;

  slot=random()%RHIZOME_VERSION_CACHE_ASSOCIATIVITY;
  rhizome_manifest_version_cache_slot *entry
    =&rhizome_manifest_version_cache[bin][slot];
  unsigned long long manifest_version = rhizome_manifest_get_ll(m,"version");

  entry->version=manifest_version;
  for(i=0;i<24;i++)
    {
      int byte=(hexvalue(id[(i*2)])<<4)|hexvalue(id[(i*2)+1]);
      entry->idprefix[i]=byte;
    }

  return 0;
}

int rhizome_manifest_version_cache_lookup(rhizome_manifest *m)
{
  int bin=0;
  int slot;
  int i;

  char id[RHIZOME_MANIFEST_ID_STRLEN + 1];
  if (!rhizome_manifest_get(m, "id", id, sizeof id))
    // dodgy manifest, we don't want to receive it
    return WHY("Ignoring bad manifest (no ID field)");
  str_toupper_inplace(id);
  m->version = rhizome_manifest_get_ll(m, "version");
  
  // skip the cache for now
  long long dbVersion = -1;
  if (sqlite_exec_int64(&dbVersion, "SELECT version FROM MANIFESTS WHERE id='%s';", id) == -1)
    return WHY("Select failure");
  if (dbVersion >= m->version) {
    if (0) WHYF("We already have %s (%lld vs %lld)", id, dbVersion, m->version);
    return -1;
  }
  return 0;

  /* Work out bin number in cache */
  for(i=0;i<RHIZOME_VERSION_CACHE_NYBLS;i++)
    {
      int nybl=hexvalue(id[i]);
      bin=(bin<<4)|nybl;
    }
  bin=bin>>RHIZOME_VERSION_CACHE_SHIFT;
  
  for(slot=0;slot<RHIZOME_VERSION_CACHE_ASSOCIATIVITY;slot++)
    {
      rhizome_manifest_version_cache_slot *entry
	=&rhizome_manifest_version_cache[bin][slot];
      for(i=0;i<24;i++)
	{
	  int byte=
	    (hexvalue(id[(i*2)])<<4)
	    |hexvalue(id[(i*2)+1]);
	  if (byte!=entry->idprefix[i]) break;
	}
      if (i==24) {
	/* Entries match -- so check version */
	long long rev = rhizome_manifest_get_ll(m,"version");
	if (1) DEBUGF("cached version %lld vs manifest version %lld", entry->version,rev);
	if (rev > entry->version) {
	  /* If we only have an old version, try refreshing the cache
	     by querying the database */
	  if (sqlite_exec_int64(&entry->version, "select version from manifests where id='%s'", id) != 1)
	    return WHY("failed to select stored manifest version");
	  DEBUGF("Refreshed stored version from database: entry->version=%lld", entry->version);
	}
	if (rev < entry->version) {
	  /* the presented manifest is older than we have.
	     This allows the caller to know that they can tell whoever gave them the
	     manifest it's time to get with the times.  May or not ever be
	     implemented, but it would be nice. XXX */
	  WHYF("cached version is NEWER than presented version (%lld is newer than %lld)",
	      entry->version,rev);
	  return -2;
	} else if (rev<=entry->version) {
	  /* the presented manifest is already stored. */	   
	  if (1) DEBUG("cached version is NEWER/SAME as presented version");
	  return -1;
	} else {
	  /* the presented manifest is newer than we have */
	  DEBUG("cached version is older than presented version");
	  return 0;
	}
      }
    }

  DEBUG("Not in manifest cache");

  /* Not in cache, so all is well, well, maybe.
     What we do know is that it is unlikely to be in the database, so it probably
     doesn't hurt to try to receive it.  

     Of course, we can just ask the database if it is there already, and populate
     the cache in the process if we find it.  The tradeoff is that the whole point
     of the cache is to AVOID database lookups, not incurr them whenever the cache
     has a negative result.  But if we don't ask the database, then we can waste
     more effort fetching the file associated with the manifest, and will ultimately
     incurr a database lookup (and more), so while it seems a little false economy
     we need to do the lookup now.

     What this all suggests is that we need fairly high associativity so that misses
     are rare events. But high associativity then introduces a linear search cost,
     although that is unlikely to be nearly as much cost as even thinking about a
     database query.

     It also says that on a busy network that things will eventually go pear-shaped
     and require regular database queries, and that memory allowing, we should use
     a fairly large cache here.
 */
  long long manifest_version = rhizome_manifest_get_ll(m, "version");
  long long count;
  switch (sqlite_exec_int64(&count, "select count(*) from manifests where id='%s' and version>=%lld", id, manifest_version)) {
    case -1:
      return WHY("database error reading stored manifest version");
    case 1:
      if (count) {
	/* Okay, we have a stored version which is newer, so update the cache
	  using a random replacement strategy. */
	long long stored_version;
	if (sqlite_exec_int64(&stored_version, "select version from manifests where id='%s'", id) < 1)
	  return WHY("database error reading stored manifest version"); // database is broken, we can't confirm that it is here
	DEBUGF("stored version=%lld, manifest_version=%lld (not fetching; remembering in cache)",
	    stored_version,manifest_version);
	slot=random()%RHIZOME_VERSION_CACHE_ASSOCIATIVITY;
	rhizome_manifest_version_cache_slot *entry
	  =&rhizome_manifest_version_cache[bin][slot];
	entry->version=stored_version;
	for(i=0;i<24;i++)
	  {
	    int byte=(hexvalue(id[(i*2)])<<4)|hexvalue(id[(i*2)+1]);
	    entry->idprefix[i]=byte;
	  }
	/* Finally, say that it isn't worth RXing this manifest */
	return stored_version > manifest_version ? -2 : -1;
      }
      break;
    default:
      return WHY("bad select result");
  }
  /* At best we hold an older version of this manifest, and at worst we
     don't hold any copy. */
  return 0;
}

typedef struct ignored_manifest {
  unsigned char bid[crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES];
  struct sockaddr_in peer;
  long long timeout;
} ignored_manifest;

#define IGNORED_BIN_SIZE 8
#define IGNORED_BIN_COUNT 64
#define IGNORED_BIN_BITS 6
typedef struct ignored_manifest_bin {
  int bins_used;
  ignored_manifest m[IGNORED_BIN_SIZE];
} ignored_manifest_bin;

typedef struct ignored_manifest_cache {
  ignored_manifest_bin bins[IGNORED_BIN_COUNT];
} ignored_manifest_cache;

/* used uninitialised, since the probability of
   a collision is exceedingly remote */
ignored_manifest_cache ignored;

int rhizome_ignore_manifest_check(rhizome_manifest *m,
				  struct sockaddr_in *peerip)
{
  int bin = m->cryptoSignPublic[0]>>(8-IGNORED_BIN_BITS);
  int slot;
  for(slot = 0; slot != IGNORED_BIN_SIZE; ++slot)
    {
      if (!memcmp(ignored.bins[bin].m[slot].bid,
		  m->cryptoSignPublic,
		  crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES))
	{
	  if (ignored.bins[bin].m[slot].timeout>overlay_gettime_ms())
	    return 1;
	  else 
	    return 0;
	}
    }
  return 0;
}

int rhizome_queue_ignore_manifest(rhizome_manifest *m,
				  struct sockaddr_in *peerip,int timeout)
{
  /* The supplied manifest from a given IP has errors, so remember 
     that it isn't worth considering */
  int bin = m->cryptoSignPublic[0]>>(8-IGNORED_BIN_BITS);
  int slot;
  for(slot = 0; slot != IGNORED_BIN_SIZE; ++slot)
    {
      if (!memcmp(ignored.bins[bin].m[slot].bid,
		  m->cryptoSignPublic,
		  crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES))
	break;
    }
  if (slot>=IGNORED_BIN_SIZE) slot=random()%IGNORED_BIN_SIZE;
  bcopy(&m->cryptoSignPublic[0],
	&ignored.bins[bin].m[slot].bid[0],
	crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES);
  /* ignore for a while */
  ignored.bins[bin].m[slot].timeout=overlay_gettime_ms()+timeout;
  bcopy(peerip,
	&ignored.bins[bin].m[slot].peer,
	sizeof(struct sockaddr_in));
  return 0;

}

typedef struct rhizome_candidates {
  rhizome_manifest *manifest;
  struct sockaddr_in peer;
  long long size;
  /* XXX Need group memberships/priority level here */
  int priority;
} rhizome_candidates;

rhizome_candidates candidates[MAX_CANDIDATES];
int candidate_count=0;

/* sort indicated candidate from starting position down
   (or up) */
int rhizome_position_candidate(int position)
{
  while(position<candidate_count&&position>=0) {
    rhizome_candidates *c1=&candidates[position];
    rhizome_candidates *c2=&candidates[position+1];
    if (c1->priority>c2->priority
	||(c1->priority==c2->priority
	   &&c1->size>c2->size))
      {
	rhizome_candidates c=*c1;
	*c1=*c2;
	*c2=c;
	position++;
      } 
    else {
      /* doesn't need moving down, but does it need moving up? */
      if (!position) return 0;
      rhizome_candidates *c0=&candidates[position-1];
      if (c1->priority<c0->priority
	  ||(c1->priority==c0->priority
	     &&c1->size<c0->size))
	{
	  rhizome_candidates c=*c1;
	  *c1=*c2;
	  *c2=c;
	  position--;
	} 
      else return 0;   
    }
  }
  return 0;
}

/* Verifies manifests as late as possible to avoid wasting time. */
int rhizome_suggest_queue_manifest_import(rhizome_manifest *m,
				  struct sockaddr_in *peerip)
{
  IN();
  /* must free manifest when done with it */
  char *id=rhizome_manifest_get(m,"id",NULL,0);
  long long filesize=rhizome_manifest_get_ll(m,"filesize");
  int priority=100; /* normal priority */
  int i;

  if (1) DEBUGF("Rhizome considering %s (size=%lld, priority=%d)",
		id,filesize,priority);
  m->version=rhizome_manifest_get_ll(m,"version");

  if (rhizome_manifest_version_cache_lookup(m)) {
    /* We already have this version or newer */
    if (1||debug&DEBUG_RHIZOMESYNC) {
      DEBUGF("manifest id=%s, version=%lld",
	   rhizome_manifest_get(m,"id",NULL,0),
	   rhizome_manifest_get_ll(m,"version"));
      DEBUG("We already have that manifest or newer.");
    }
    rhizome_manifest_free(m);
    RETURN(-1);
  } else {
    if (1||debug&DEBUG_RHIZOMESYNC) {
      long long stored_version;
      if (sqlite_exec_int64(&stored_version, "select version from manifests where id='%s'",id) > 0)
	DEBUGF("manifest id=%s, version=%lld is new to us (we only have version %lld).",
	      id,
	      m->version,
	      stored_version);
    }
  }

  /* work out where to put it in the list */
  for(i=0;i<candidate_count;i++)
    {
      /* If this manifest is already in the list, stop.
         (also replace older manifest versions with newer ones,
          which can upset the ordering.) */
      if (candidates[i].manifest==NULL) continue;
      if (!strcasecmp(id,rhizome_manifest_get(candidates[i].manifest,"id",NULL,0)))
	  {
	    /* duplicate.
	       XXX - Check versions! We should replace older with newer,
	       and then update position in queue based on size */
	  long long list_version=rhizome_manifest_get_ll(candidates[i].manifest, "version");
	  long long this_version=rhizome_manifest_get_ll(m,"version");
	  if (list_version>=this_version) {
	    /* this version is older than the one in the list,
	       so don't list this one */
	    rhizome_manifest_free(m);
	    RETURN(0); 
	  } else {
	    /* replace listed version with this newer version */
	    if (rhizome_manifest_verify(m)) {
	      WHY("Error verifying manifest when considering queuing for import");
	      /* Don't waste time looking at this manifest again for a while */
	      rhizome_queue_ignore_manifest(m,peerip,60000);
	      rhizome_manifest_free(m);
	      RETURN(-1);
	    }

	    rhizome_manifest_free(candidates[i].manifest);
	    candidates[i].manifest=m;
	    /* update position in list */
	    rhizome_position_candidate(i);
	    RETURN(0);
	  }
	}

      /* if we have a higher priority file than the one at this
	 point in the list, stop, and we will shuffle the rest of
	 the list down. */
      if (candidates[i].priority>priority
	  ||(candidates[i].priority==priority
	     &&candidates[i].size>filesize))
	break;
    }
  if (i>=MAX_CANDIDATES) {
    /* our list is already full of higher-priority items */
    rhizome_manifest_free(m);
    RETURN(-1);
  }

  if (rhizome_manifest_verify(m)) {
    WHY("Error verifying manifest when considering queuing for import");
    /* Don't waste time looking at this manifest again for a while */
    rhizome_queue_ignore_manifest(m,peerip,60000);
    rhizome_manifest_free(m);
    RETURN(-1);
  }

  if (candidate_count==MAX_CANDIDATES) {
    /* release manifest structure for whoever we are bumping from the list */
    rhizome_manifest_free(candidates[MAX_CANDIDATES-1].manifest);
    candidates[MAX_CANDIDATES-1].manifest=NULL;
  } else candidate_count++;
  /* shuffle down */
  int bytes=(candidate_count-(i+1))*sizeof(rhizome_candidates);
  if (0) DEBUGF("Moving slot %d to slot %d (%d bytes = %d slots)",
	      i,i+1,bytes,bytes/sizeof(rhizome_candidates));
  bcopy(&candidates[i],
	&candidates[i+1],
	bytes);
  /* put new candidate in */
  candidates[i].manifest=m;
  candidates[i].size=filesize;
  candidates[i].priority=priority;
  candidates[i].peer=*peerip;

  int j;
  if (0) {
    DEBUG("Rhizome priorities fetch list now:");
    for(j=0;j<candidate_count;j++)
      DEBUGF("%02d:%s:size=%lld, priority=%d",
	   j,
	   rhizome_manifest_get(candidates[j].manifest,"id",NULL,0),
	   candidates[j].size,candidates[j].priority);
  }

  RETURN(0);
}

void rhizome_enqueue_suggestions(struct sched_ent *alarm)
{
  int i;
  for(i=0;i<candidate_count;i++)
    {
      if (rhizome_file_fetch_queue_count>=MAX_QUEUED_FILES) 
	break;
      int manifest_kept = 0;
      rhizome_queue_manifest_import(candidates[i].manifest,&candidates[i].peer, &manifest_kept);
      if (!manifest_kept) {
	rhizome_manifest_free(candidates[i].manifest);
	candidates[i].manifest = NULL;
      }
    }
  if (i) {
    /* now shuffle up */
    int bytes=(candidate_count-i)*sizeof(rhizome_candidates);
    if (0) DEBUGF("Moving slot %d to slot 0 (%d bytes = %d slots)",
		i,bytes,bytes/sizeof(rhizome_candidates));
    bcopy(&candidates[i],&candidates[0],bytes);
    candidate_count-=i;
  }
  alarm->alarm = overlay_gettime_ms() + 3000;
  schedule(alarm);
  return;
}

int rhizome_queue_manifest_import(rhizome_manifest *m, struct sockaddr_in *peerip, int *manifest_kept)
{
  *manifest_kept = 0;
  int i;

  /* Do the quick rejection tests first, before the more expensive once,
     like querying the database for manifests. 

     We probably need a cache of recently rejected manifestid:versionid
     pairs so that we can avoid database lookups in most cases.  Probably
     the first 64bits of manifestid is sufficient to make it resistant to
     collission attacks, but using 128bits or the full 256 bits would be safer.
     Let's make the cache use 256 bit (32byte) entries for power of two
     efficiency, and so use the last 64bits for version id, thus using 192 bits
     for collission avoidance --- probably sufficient for many years yet (from
     time of writing in 2012).  We get a little more than 192 bits by using
     the cache slot number to implicitly store the first bits.
  */

  if (rhizome_manifest_version_cache_lookup(m)) {
    /* We already have this version or newer */
    if (1||debug&DEBUG_RHIZOMESYNC) {
      DEBUGF("manifest id=%s, version=%lld",
	   rhizome_manifest_get(m,"id",NULL,0),
	   rhizome_manifest_get_ll(m,"version"));
      DEBUG("We already have that manifest or newer.");
    }
    return -1;
  } else {
    if (1||debug&DEBUG_RHIZOMESYNC) {
      DEBUGF("manifest id=%s, version=%lld is new to us.",
	   rhizome_manifest_get(m,"id",NULL,0),
	   rhizome_manifest_get_ll(m,"version"));
    }
  }

  /* Don't queue if queue slots already full */
  if (rhizome_file_fetch_queue_count>=MAX_QUEUED_FILES) {
    if (1||debug&DEBUG_RHIZOME) DEBUG("Already busy fetching files");
    return -1;
  }
  /* Don't queue if already queued */
  char *id=rhizome_manifest_get(m,"id",NULL,0);
  if (!rhizome_str_is_manifest_id(id))
    return WHYF("Invalid manifest ID: %s", id);
  for(i=0;i<rhizome_file_fetch_queue_count;i++) {
    rhizome_file_fetch_record 
      *q=&file_fetch_queue[i];
    if (!strcasecmp(id,rhizome_manifest_get(q->manifest,"id",NULL,0))) {
	if (1||debug&DEBUG_RHIZOMESYNC)
	  DEBUGF("Already have %s in the queue.",id);
	return -1;
      }
  }

  if (!rhizome_manifest_get(m, "filehash", m->fileHexHash, sizeof m->fileHexHash))
    return WHY("Manifest missing filehash");
  if (!rhizome_str_is_file_hash(m->fileHexHash))
    return WHYF("Invalid file hash: %s", m->fileHexHash);
  str_toupper_inplace(m->fileHexHash);
  m->fileHashedP = 1;

  long long filesize = rhizome_manifest_get_ll(m, "filesize");

  if (1||debug&DEBUG_RHIZOMESYNC) 
    DEBUGF("Getting ready to fetch file %s for manifest %s", m->fileHexHash, id);

  if (filesize > 0 && m->fileHexHash[0])
    {
      long long gotfile = 0;
      if (sqlite_exec_int64(&gotfile, "SELECT COUNT(*) FROM FILES WHERE ID='%s' and datavalid=1;", m->fileHexHash) != 1)
	return WHY("select failed");
      if (gotfile!=1) {
	/* We need to get the file */
	/* Discard request if the same manifest is already queued for reception.   
	 */
	int i,j;
	for(i=0;i<rhizome_file_fetch_queue_count;i++)
	  {
	    for(j=0;j<crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES;j++)
	      if (m->cryptoSignPublic[j]
		  !=file_fetch_queue[i].manifest->cryptoSignPublic[j]) break;
	    if (j==crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES)
	      {
		/* We are already fetching this manifest */
		if (1||debug&DEBUG_RHIZOME) DEBUGF("Already fetching manifest");
		return -1;
	      }
	    for(j=0;j<=RHIZOME_FILEHASH_STRLEN;j++)
	      if (m->fileHexHash[j]!=file_fetch_queue[i].fileid[j]) break;
	    if (j==RHIZOME_FILEHASH_STRLEN + 1)
	      {
		/* We are already fetching this file */
		if (1||debug&DEBUG_RHIZOME) DEBUGF("Already fetching file %s", m->fileHexHash);
		return -1;
	      }
	  }

	if (peerip)
	  {
	    /* Transfer via HTTP over IPv4 */
	    int sock = socket(AF_INET,SOCK_STREAM,0);
	    SET_NONBLOCKING(sock);
	    struct sockaddr_in peeraddr;
	    bcopy(peerip,&peeraddr,sizeof(peeraddr));
	    peeraddr.sin_port=htons(RHIZOME_HTTP_PORT);
	    DEBUG("Initiating HTTP connection for transfer");
	    int r=connect(sock,(struct sockaddr*)&peeraddr,sizeof(peeraddr));
	    if ((errno!=EINPROGRESS)&&(r!=0)) {	      
	      WHY("Failed to open socket to peer's rhizome web server");
	      WHY_perror("connect");
	      close (sock);
	      return -1;
	    }
	    
	    rhizome_file_fetch_record 
	      *q=&file_fetch_queue[rhizome_file_fetch_queue_count];
	    q->manifest = m;
	    *manifest_kept = 1;
	    q->alarm.poll.fd=sock;
	    strncpy(q->fileid, m->fileHexHash, RHIZOME_FILEHASH_STRLEN + 1);
	    snprintf(q->request,1024,"GET /rhizome/file/%s HTTP/1.0\r\n\r\n",
		     q->fileid);
	    q->request_len=strlen(q->request);
	    q->request_ofs=0;
	    q->state=RHIZOME_FETCH_CONNECTING;
	    q->file_len=-1;
	    q->file_ofs=0;
	    
	    /* XXX Don't forget to implement resume */
	    /* XXX We should stream file straight into the database */
	    const char *id = rhizome_manifest_get(q->manifest, "id", NULL, 0);
	    if (id == NULL) {
	      close(sock);
	      return WHY("Manifest missing ID");
	    }
	    if (create_rhizome_import_dir() == -1)
	      return -1;
	    char filename[1024];
	    if (!FORM_RHIZOME_IMPORT_PATH(filename, "file.%s", id)) {
	      close(sock);
	      return -1;
	    }
	    q->manifest->dataFileName = strdup(filename);
	    q->file=fopen(q->manifest->dataFileName,"w");
	    if (!q->file) {
	      WHY_perror("fopen");
	      if (1||debug&DEBUG_RHIZOME)
		DEBUGF("Could not open '%s' to write received file.", q->manifest->dataFileName);
	      close(sock);
	      return -1;
	    }
	    
	    /* Watch for activity on the socket */
	    q->alarm.function=rhizome_fetch_poll;
	    fetch_stats.name="rhizome_fetch_poll";
	    q->alarm.stats=&fetch_stats;
	    q->alarm.poll.events=POLLIN|POLLOUT;
	    watch(&q->alarm);
	    /* And schedule a timeout alarm */
	    q->alarm.alarm=overlay_gettime_ms() + RHIZOME_IDLE_TIMEOUT;
	    schedule(&q->alarm);

	    rhizome_file_fetch_queue_count++;
	    if (1||debug&DEBUG_RHIZOME) DEBUGF("Queued file for fetching into %s (%d in queue)",
					    q->manifest->dataFileName, rhizome_file_fetch_queue_count);
	    return 0;
	  }
	else
	  {
	    /* Transfer via overlay */
	    return WHY("Rhizome fetching via overlay not implemented");
	  }
      }
      else
	{
	  if (1||debug&DEBUG_RHIZOMESYNC) 
	    DEBUGF("We already have the file for this manifest; importing from manifest alone.");
	  if (create_rhizome_import_dir() == -1)
	    return -1;
	  char filename[1024];
	  if (!FORM_RHIZOME_IMPORT_PATH(filename, "manifest.%s", id))
	    return -1;
	  if (!rhizome_write_manifest_file(m, filename)) {
	    rhizome_bundle_import(m, NULL, id, m->ttl-1);
	  }
	}
    }

  return 0;
}

int rhizome_fetch_close(rhizome_file_fetch_record *q){
  /* Free ephemeral data */
  if (q->file) fclose(q->file);
  q->file=NULL;
  if (q->manifest) 
    rhizome_manifest_free(q->manifest);
  q->manifest=NULL;
  
  /* close socket and stop watching it */
  unwatch(&q->alarm);
  unschedule(&q->alarm);
  close(q->alarm.poll.fd);
  q->alarm.poll.fd=-1;
  
  /* Reduce count of open connections */	
  rhizome_file_fetch_queue_count--;
  
  if (debug&DEBUG_RHIZOME) 
    DEBUGF("Released rhizome fetch slot (%d used)",
	   rhizome_file_fetch_queue_count);
  return 0;
}

void rhizome_fetch_write(rhizome_file_fetch_record *q){
  int bytes;
  bytes=write(q->alarm.poll.fd,&q->request[q->request_ofs],
	      q->request_len-q->request_ofs);
  if (bytes>0) {
    
    // reset timeout
    unschedule(&q->alarm);
    q->alarm.alarm=overlay_gettime_ms() + RHIZOME_IDLE_TIMEOUT;
    schedule(&q->alarm);
    q->request_ofs+=bytes;
    
    if (q->request_ofs>=q->request_len) {
      /* Sent all of request.  Switch to listening for HTTP response headers.
       */
      q->request_len=0; q->request_ofs=0;
      q->state=RHIZOME_FETCH_RXHTTPHEADERS;
      q->alarm.poll.events=POLLIN;
      watch(&q->alarm);
    }else if(q->state==RHIZOME_FETCH_CONNECTING)
      q->state = RHIZOME_FETCH_SENDINGHTTPREQUEST;
  } else if (errno!=EAGAIN) {
    WHY("Got error while sending HTTP request.  Closing.");
    rhizome_fetch_close(q);
  }
}

void rhizome_fetch_poll(struct sched_ent *alarm)
{
  rhizome_file_fetch_record *q=(rhizome_file_fetch_record *)alarm;
  
  if (alarm->poll.revents==0){
    // timeout, close the socket
    rhizome_fetch_close(q);
    return;
  }
  
  switch(q->state) 
    {
    case RHIZOME_FETCH_CONNECTING:
    case RHIZOME_FETCH_SENDINGHTTPREQUEST:
      rhizome_fetch_write(q);
      break;
    case RHIZOME_FETCH_RXFILE:
      /* Keep reading until we have the promised amount of data */
      
      sigPipeFlag=0;
      
      errno=0;
      char buffer[8192];

      int bytes=read(q->alarm.poll.fd,buffer,8192);
      
      /* If we got some data, see if we have found the end of the HTTP request */
      if (bytes>0) {
	
	// reset timeout
	unschedule(&q->alarm);
	q->alarm.alarm=overlay_gettime_ms() + RHIZOME_IDLE_TIMEOUT;
	schedule(&q->alarm);

	if (bytes>(q->file_len-q->file_ofs))
	  bytes=q->file_len-q->file_ofs;
	if (fwrite(buffer,bytes,1,q->file)!=1)
	  {
	    if (debug&DEBUG_RHIZOME) DEBUGF("Failed writing %d bytes to file. @ offset %d",bytes,q->file_ofs);
	    rhizome_fetch_close(q);
	    return;
	  }
	q->file_ofs+=bytes;
	
      } else if (bytes==0) {
	WHY("Got zero bytes, assume socket dead.");
	rhizome_fetch_close(q);
	return;
      }
      if (q->file_ofs>=q->file_len)
	{
	  /* got all of file */
	  if (debug&DEBUG_RHIZOME) DEBUGF("Received all of file via rhizome -- now to import it");
	  {
	    fclose(q->file); q->file=NULL;
	    const char *id = rhizome_manifest_get(q->manifest, "id", NULL, 0);
	    if (id == NULL)
	      { WHY("Manifest missing ID"); return; }
	    if (create_rhizome_import_dir() == -1)
	      return;
	    char filename[1024];
	    if (!FORM_RHIZOME_IMPORT_PATH(filename,"manifest.%s", id))
	      return;
	    /* Do really write the manifest unchanged */
	    if (debug&DEBUG_RHIZOME) {
	      DEBUGF("manifest has %d signatories",q->manifest->sig_count);
	      DEBUGF("manifest id = %s, len=%d",
		      rhizome_manifest_get(q->manifest,"id",NULL,0),
		      q->manifest->manifest_bytes);
	      dump("manifest",&q->manifest->manifestdata[0],
		   q->manifest->manifest_all_bytes);
	    }
	    q->manifest->finalised=1;
	    q->manifest->manifest_bytes=q->manifest->manifest_all_bytes;
	    if (rhizome_write_manifest_file(q->manifest,filename) != -1) {
	      rhizome_bundle_import(q->manifest, NULL, id,
				    q->manifest->ttl - 1 /* TTL */);
	    }
	    rhizome_manifest_free(q->manifest);
	    q->manifest=NULL;
	  }
	  rhizome_fetch_close(q);
	  return;
	}
      break;
    case RHIZOME_FETCH_RXHTTPHEADERS:
      /* Keep reading until we have two CR/LFs in a row */
      sigPipeFlag=0;
      
      errno=0;
      bytes=read(q->alarm.poll.fd,&q->request[q->request_len],
		 1024-q->request_len-1);

      /* If we got some data, see if we have found the end of the HTTP request */
      if (bytes>0) {
	int lfcount=0;
	int i=q->request_len-160;
	
	// reset timeout
	unschedule(&q->alarm);
	q->alarm.alarm=overlay_gettime_ms() + RHIZOME_IDLE_TIMEOUT;
	schedule(&q->alarm);
	
	if (i<0) i=0;
	q->request_len+=bytes;
	if (q->request_len<1024)
	  q->request[q->request_len]=0;
	
	for(;i<(q->request_len+bytes);i++)
	  {
	    switch(q->request[i]) {
	    case '\n': lfcount++; break;
	    case '\r': /* ignore CR */ break;
	    case 0: /* ignore NUL (telnet inserts them) */ break;
	    default: lfcount=0; break;
	    }
	    if (lfcount==2) break;
	  }
	
	if (debug&DEBUG_RHIZOME)
	  dump("http reply headers",(unsigned char *)q->request,lfcount==2?i:q->request_len);
	
	if (lfcount==2) {
	  /* We have the response headers, so parse.
	     (we may also have some bytes of content, so we need to be a little
	     careful) */

	  /* Terminate string at end of headers */
	  q->request[i]=0;

	  /* Get HTTP result code */
	  char *s=strstr(q->request,"HTTP/1.0 ");
	  if (!s) { 
	    if (debug&DEBUG_RHIZOME) DEBUGF("HTTP response lacked HTTP/1.0 response code.");
	    rhizome_fetch_close(q);
	    return;
	  }
	  int http_response_code=strtoll(&s[9],NULL,10);
	  if (http_response_code!=200) {
	    if (debug&DEBUG_RHIZOME) DEBUGF("Rhizome web server returned %d != 200 OK",http_response_code);
	    rhizome_fetch_close(q);
	    return;
	  }
	  /* Get content length */
	  s=strstr(q->request,"Content-length: ");
	  if (!s) {
	    if (debug&DEBUG_RHIZOME) 
	      DEBUGF("Missing Content-Length: header.");
	    rhizome_fetch_close(q);
	    return;
	  }
	  q->file_len=strtoll(&s[16],NULL,10);
	  
	  if (q->file_len<0) {
	    if (debug&DEBUG_RHIZOME) 
	      DEBUGF("Illegal file size (%d).",q->file_len);
	    rhizome_fetch_close(q);
	    return;
	  }

	  /* Okay, we have both, and are all set.
	     File is already open, so just write out any initial bytes of the
	     file we read, and update state flag.
	  */
	  int fileRxBytes=q->request_len-(i+1);
	  if (fileRxBytes>0)
	    if (fwrite(&q->request[i+1],fileRxBytes,1,q->file)!=1)
	      {
		if (debug&DEBUG_RHIZOME) 
		  DEBUGF("Failed writing initial %d bytes to file.",
			  fileRxBytes);	       
		rhizome_fetch_close(q);
		return;
	      }
	  q->file_ofs=fileRxBytes;
	  DEBUGF("Transferred (%lld of %lld)",
		 q->file_ofs,q->file_len);
	  q->state=RHIZOME_FETCH_RXFILE;
	}
	
      } 
      
      if (sigPipeFlag||((bytes==0)&&(errno==0))) {
	/* broken pipe, so close connection */
	if (debug&DEBUG_RHIZOME) 
	  DEBUG("Closing rhizome fetch connection due to sigpipe");
	rhizome_fetch_close(q);
	return;
      }	 
      break;
    default:
      if (debug&DEBUG_RHIZOME) 
	DEBUG("Closing rhizome fetch connection due to illegal/unimplemented state.");
      rhizome_fetch_close(q);
      return;
    }
  return;
}
