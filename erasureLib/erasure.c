/*
Copyright (c) 2015, Los Alamos National Security, LLC
All rights reserved.

Copyright 2015.  Los Alamos National Security, LLC. This software was produced
under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National
Laboratory (LANL), which is operated by Los Alamos National Security, LLC for
the U.S. Department of Energy. The U.S. Government has rights to use, reproduce,
and distribute this software.  NEITHER THE GOVERNMENT NOR LOS ALAMOS NATIONAL
SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR ASSUMES ANY LIABILITY
FOR THE USE OF THIS SOFTWARE.  If software is modified to produce derivative
works, such modified software should be clearly marked, so as not to confuse it
with the version available from LANL.
 
Additionally, redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
3. Neither the name of Los Alamos National Security, LLC, Los Alamos National
Laboratory, LANL, the U.S. Government, nor the names of its contributors may be
used to endorse or promote products derived from this software without specific
prior written permission.

THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL SECURITY, LLC OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-----
NOTE:
-----
Although these files reside in a seperate repository, they fall under the MarFS copyright and license.

MarFS is released under the BSD license.

MarFS was reviewed and released by LANL under Los Alamos Computer Code identifier:
LA-CC-15-039.

These erasure utilites make use of the Intel Intelligent Storage
Acceleration Library (Intel ISA-L), which can be found at
https://github.com/01org/isa-l and is under its own license.

MarFS uses libaws4c for Amazon S3 object communication. The original version
is at https://aws.amazon.com/code/Amazon-S3/2601 and under the LGPL license.
LANL added functionality to the original work. The original work plus
LANL contributions is found at https://github.com/jti-lanl/aws4c.

GNU licenses can be found at http://www.gnu.org/licenses/.
*/



 

/* ---------------------------------------------------------------------------

This file provides the implementation of multiple operations intended for
use by the MarFS MultiComponent DAL.

These include:   ne_read(), ne_write(), ne_health(), and ne_rebuild().

Additionally, each output file gets an xattr added to it (yes all 12 files
in the case of a 10+2 the xattr looks something like this:

   10 2 64 0 196608 196608 3304199718723886772 1717171

These fields, in order, are:

    N         is nparts
    E         is numerasure
    offset    is the starting position of the stripe in terms of part number
    chunksize is chunksize
    nsz       is the size of the part
    ncompsz   is the size of the part but might get used if we ever compress the parts
    totsz     is the total real data in the N part files.

Since creating erasure requires full stripe writes, the last part of the
file may all be zeros in the parts.  Thus, totsz is the real size of the
data, not counting the trailing zeros.

All the parts and all the erasure stripes should be the same size.  To fill
in the trailing zeros, this program uses truncate - punching a hole in the
N part files for the zeros.

In the case where libne is built to include support for S3-authentication,
and to use the libne sockets extensions (RDMA, etc) instead of files, then
the caller (for example, the MarFS sockets DAL) may acquire
authentication-information at program-initialization-time which we could
not acquire at run-time.  For example, access to authentication-information
may require escalated privileges, whereas fuse and pftool de-escalate
priviledges after start-up.  To support such cases, we must allow a caller
to pass cached credentials through the ne_etc() functions, to the
underlying skt_etc() functions.

--------------------------------------------------------------------------- */


#include "erasure.h"
#include "udal.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#if (AXATTR_RES == 2)
#  include <attr/xattr.h>
#else
#  include <sys/xattr.h>
#endif

#include <assert.h>
#include <pthread.h>


static int set_block_xattr(ne_handle handle, int block);



// #defines, macros, external functions, etc, that we don't want exported
// for users of the library some are also used in libneTest.c
//
// #include "erasure_internals.h"

/* The following are defined here, so as to hide them from users of the library */
#ifdef HAVE_LIBISAL
extern uint32_t      crc32_ieee(uint32_t seed, uint8_t * buf, uint64_t len);
extern void          ec_encode_data(int len, int srcs, int dests, unsigned char *v,unsigned char **src, unsigned char **dest);
#else
extern uint32_t      crc32_ieee_base(uint32_t seed, uint8_t * buf, uint64_t len);
extern void          ec_encode_data_base(int len, int srcs, int dests, unsigned char *v,unsigned char **src, unsigned char **dest);
#endif

int xattr_check( ne_handle handle, char *path );
int manifest_check( SnprintfFunc fn, void* state, const uDAL* impl, SktAuth auth, char *path, ne_mode mode, ne_status status );
int part_check( SnprintfFunc fn, void* state, const uDAL* impl, SktAuth auth, char *path, ne_status status );
static int gf_gen_decode_matrix(unsigned char *encode_matrix,
                                unsigned char *decode_matrix,
                                unsigned char *invert_matrix,
                                unsigned int *decode_index,
                                unsigned char *src_err_list,
                                unsigned char *src_in_err,
                                int nerrs, int nsrcerrs, int k, int m);
//void dump(unsigned char *buf, int len);


#if 0

    // Old compile-time selection of sockets versus file semantics
    #ifdef SOCKETS
    #  define FD(FDESC)                           (FDESC).peer_fd
    #  define OPEN(FDESC, AUTH, ...)              do{ skt_open(&(FDESC), ## __VA_ARGS__); \
                                                      skt_fcntl(&(FDESC), SKT_F_SETAUTH, (AUTH)); } while(0)
    #  define HNDLOP(OP, FDESC, ...)              skt_##OP(&(FDESC), ## __VA_ARGS__)
    #  define pHNDLOP(OP, FDESC_PTR, ...)         skt_##OP((FDESC_PTR), ## __VA_ARGS__)
    #  define PATHOP(OP, AUTH, PATH, ...)         skt_##OP((AUTH), (PATH), ## __VA_ARGS__)
    #  define UMASK(FDESC, MASK)                  umask(MASK) /* TBD */
    #  define DEFAULT_AUTH_INIT(AUTH)             skt_auth_init(SKT_S3_USER, &(AUTH))
    #  define AUTH_INSTALL(FD, AUTH)              skt_auth_install((FD), (AUTH))
    
    #else
    #  define FD(FDESC)                           (FDESC)
    #  define OPEN(FDESC, AUTH, ...)              ((FDESC) = open(__VA_ARGS__))
    #  define HNDLOP(OP, FDESC, ...)              OP((FDESC), ## __VA_ARGS__)
    #  define pHNDLOP(OP, FDESC_PTR, ...)         OP(*(FDESC_PTR), ## __VA_ARGS__)
    #  define PATHOP(OP, AUTH, PATH, ...)         OP((PATH), ## __VA_ARGS__)
    #  define UMASK(FDESC, MASK)                  umask(MASK)
    #  define DEFAULT_AUTH_INIT(AUTH)             ((AUTH) = NULL)  /* just like a funcall returning zero */
    #  define AUTH_INSTALL(FD, AUTH)
    #endif

#else

// New run-time selection of sockets versus file semantics

#  define FD_INIT(GFD, HANDLE)                (HANDLE)->impl->fd_init(&(GFD))
#  define FD_ERR(GFD)                         (GFD).hndl->impl->fd_err(&(GFD))
#  define FD_NUM(GFD)                         (GFD).hndl->impl->fd_num(&(GFD))

// This is opening a file (not a handle), using the impl in the handle.
// Save the ne_handle into the GFD at open-time, so:
// (a) we don't have to pass handle->auth to impls that don't need it
// (b) write_all() can just take a GFD, and impls can still get auth if they need it
//
// TBD: It's awkward for some of the new functions to create a dummy
//      handle, just so they can OPEN() a GFD.  Better would be to just
//      have OPEN() take impl and auth, and store just those onto the gfd.
//      The only UDAL op that cares is udal_skt_open() which only needs the
//      auth.  [p]HNDLOP() could then just use the impl directly from the
//      gfd.  The handle is never really needed.  Callers of OPEN() that
//      are currently using a "fake" handle just to use OPEN() include
//      ne_size(), and ne_set_attr1().
//
#  define OPEN(GFD, HANDLE, ...)              do { (GFD).hndl = (HANDLE); \
                                                   (HANDLE)->impl->open(&(GFD), ## __VA_ARGS__); } while(0)

#  define pHNDLOP(OP, GFDp, ...)              (GFDp)->hndl->impl->OP((GFDp), ## __VA_ARGS__)
#  define HNDLOP(OP, GFD, ...)                (GFD).hndl->impl->OP(&(GFD), ## __VA_ARGS__)

#  define PATHOP(OP, IMPL, AUTH, PATH, ...)   (IMPL)->OP((AUTH), (PATH), ## __VA_ARGS__)
#  define UMASK(GFD, MASK)                    umask(MASK) /* TBD */
#  define DEFAULT_AUTH_INIT(AUTH)             skt_auth_init(SKT_S3_USER, &(AUTH))
#  define AUTH_INSTALL(FD, AUTH)              skt_auth_install((FD), (AUTH))

#  define INDEX_TRANS(INDEX,OFFSET,N,E)       ((INDEX+OFFSET)%(N+E))

#endif


/**
 * write(2) may return less than the requested write-size, without there being any errors.
 * Call write() repeatedly until the buffer has been completely written, or an error has occurred.
 *
 * @param GenericFD fd: the file/socket to write to
 * @param void* buffer: buffer to be written
 * @param size_t nbytes: size of the buffer
 *
 * @return ssize_t: the amount written.  negative for errors.
 */

static ssize_t write_all(GenericFD* fd, const void* buffer, size_t nbytes) {
  ssize_t     result = 0;
  size_t      remain = nbytes;
  const char* buf    = buffer;  /* assure ourselves pointer arithmetic is by onezies  */

  while (remain) {
    errno = 0;

    ssize_t count = pHNDLOP(write, fd, buf+result, remain);

    if (count < 0)
      return count;

    //    // this is EAGAIN, even after successful writes
    //    else if (errno)
    //      return -1;

    remain -= count;
    result += count;
  }

  return result;
}



// check for an incomplete write of an object
//int incomplete_write( ne_handle handle ) {
//   char fname[MAXNAME];
//   int i;
//   int err_cnt = 0;
//
//   for( i = 0; i < handle->nerr; i++ ) {
//      int block = handle->src_err_list[i];
//      handle->snprintf( fname, MAXNAME, handle->path,
//                        (handle->status->O + block) % ( (handle->status->N) ? (handle->status->N + handle->status->E) : MAXPARTS ),
//                        handle->state);
//      strcat( fname, WRITE_SFX );
//      
//      struct stat st;
//      // check for a partial data-file
//      if( stat( fname, &st ) == 0 ) {
//         return 1;
//      }
//      else {
//         //check for a partial meta-file
//         strcat( fname, META_SFX );
//         if( stat( fname, &st ) == 0 ) return 1;
//         err_cnt++;
//      }
//   }
//
//   return 0;
//}


void bq_destroy(BufferQueue *bq) {
  // XXX: Should technically check these for errors (ie. still locked)
  pthread_mutex_destroy(&bq->qlock);
  pthread_cond_destroy(&bq->full);
  pthread_cond_destroy(&bq->empty);
}

int bq_init(BufferQueue *bq, int block_number, void **buffers, ne_handle handle) {
  int i;
  for(i = 0; i < MAX_QDEPTH; i++) {
    bq->buffers[i] = buffers[i];
  }

  bq->block_number = block_number;
  bq->qdepth       = 0;
  bq->head         = 0;
  bq->tail         = 0;
  bq->flags        = 0;
  bq->csum         = 0;
  bq->buffer_size  = handle->status->bsz;
  bq->handle       = handle;
  bq->offset       = 0;

  FD_INIT(bq->file, handle);

  if(pthread_mutex_init(&bq->qlock, NULL)) {
    PRINTerr("failed to initialize mutex for qlock\n");
    return -1;
  }
  if(pthread_cond_init(&bq->full, NULL)) {
    PRINTerr("failed to initialize cv for full\n");
    // should also destroy the mutex
    pthread_mutex_destroy(&bq->qlock);
    return -1;
  }
  if(pthread_cond_init(&bq->empty, NULL)) {
    PRINTerr("failed to initialize cv for empty\n");
    pthread_mutex_destroy(&bq->qlock);
    pthread_cond_destroy(&bq->full);
    return -1;
  }

  return 0;
}

void bq_signal(BufferQueue*bq, BufferQueue_Flags sig) {
  pthread_mutex_lock(&bq->qlock);
  PRINTdbg("signalling 0x%x to block %d\n", (uint32_t)sig, bq->block_number);
  bq->flags |= sig;
  pthread_cond_signal(&bq->full);
  pthread_mutex_unlock(&bq->qlock);  
}

void bq_close(BufferQueue *bq) {
  bq_signal(bq, BQ_FINISHED);
}

void bq_abort(BufferQueue *bq) {
  bq_signal(bq, BQ_ABORT);
}


void bq_writer_finis(void* arg) {
  BufferQueue *bq = (BufferQueue *)arg;
  PRINTdbg("exiting thread for block %d, in %s\n", bq->block_number, bq->path);
}


void *bq_writer(void *arg) {
  BufferQueue *bq      = (BufferQueue *)arg;
  ne_handle    handle  = bq->handle;
  size_t       written = 0;
  int          error;

#ifdef INT_CRC
  const int write_size = bq->buffer_size + sizeof(u32);
#else
  const int write_size = bq->buffer_size;
#endif

  if (handle->stat_flags & SF_THREAD)
     fast_timer_start(&handle->stats[bq->block_number].thread);
  if (handle->stat_flags & SF_OPEN)
     fast_timer_start(&handle->stats[bq->block_number].open);
  
  // debugging, assure we see thread entry/exit, even via cancellation
  PRINTdbg("entering thread for block %d, in %s\n", bq->block_number, bq->path);
  pthread_cleanup_push(bq_writer_finis, bq);

  // open the file.
  mode_t mask = umask( 0000 );
  OPEN(bq->file, handle, bq->path, O_WRONLY|O_CREAT, 0666);
  umask( mask );

  if(pthread_mutex_lock(&bq->qlock) != 0) {
    if (handle->stat_flags & SF_THREAD)
       fast_timer_stop(&handle->stats[bq->block_number].thread);
    exit(-1); // XXX: is this the appropriate response??
  }
  if(FD_ERR(bq->file)) {
    bq->flags |= BQ_ERROR;
  }
  else {
    bq->flags |= BQ_OPEN;
  }
  pthread_cond_signal(&bq->empty);
  pthread_mutex_unlock(&bq->qlock);

  PRINTdbg("opened file %d\n", bq->block_number);
  if (handle->stat_flags & SF_OPEN)
     fast_timer_stop(&handle->stats[bq->block_number].open);
  if (handle->stat_flags & SF_RW)
     fast_timer_start(&handle->stats[bq->block_number].read);

  
  while(1) {

    // wait for FULL condition
    if((error = pthread_mutex_lock(&bq->qlock)) != 0) {
      PRINTerr("failed to lock queue lock: %s\n", strerror(error));
      // XXX: This is a FATAL error
      if (handle->stat_flags & SF_THREAD)
         fast_timer_stop(&handle->stats[bq->block_number].thread);
      return (void *)-1;
    }
    while(bq->qdepth == 0 && !((bq->flags & BQ_FINISHED) || (bq->flags & BQ_ABORT))) {
      PRINTdbg("bq_writer[%d]: waiting for signal from ne_write\n", bq->block_number);
      pthread_cond_wait(&bq->full, &bq->qlock);
    }

    if (handle->stat_flags & SF_RW) {
       fast_timer_stop(&handle->stats[bq->block_number].read);
       log_histo_add_interval(&handle->stats[bq->block_number].read_h,
                              &handle->stats[bq->block_number].read);
    }

    // check for flags that might tell us to quit
    if(bq->flags & BQ_ABORT) {
      PRINTerr("aborting buffer queue\n");
      if (handle->stat_flags & SF_CLOSE)
         fast_timer_start(&handle->stats[bq->block_number].close);

      if(HNDLOP(close, bq->file) == 0) {
         PATHOP(unlink, handle->impl, handle->auth, bq->path); // try to clean up after ourselves.
      }
      pthread_mutex_unlock(&bq->qlock);

      if (handle->stat_flags & SF_CLOSE)
         fast_timer_stop(&handle->stats[bq->block_number].close);
      return NULL;
    }

    if((bq->qdepth == 0) && (bq->flags & BQ_FINISHED)) {       // then we are done.
      // // TBD: ?
      // PRINTerr("closing buffer queue\n");
      // HNDLOP(close, bq->file);
      // pthread_mutex_unlock(&bq->qlock);

      PRINTdbg("BQ finished\n");
      break;
    }
    

    if(!(bq->flags & BQ_ERROR)) {

      if (handle->stat_flags & SF_RW)
         fast_timer_start(&handle->stats[bq->block_number].write);

      pthread_mutex_unlock(&bq->qlock);
      if(written >= SYNC_SIZE) {
        if ( HNDLOP(fsync, bq->file) ) {
          bq->flags |= BQ_ERROR;
        }
        written = 0;
      }

      PRINTdbg("Writing block %d\n", bq->block_number);
      u32 crc   = crc32_ieee(TEST_SEED, bq->buffers[bq->head], bq->buffer_size);
      error     = write_all(&bq->file, bq->buffers[bq->head], bq->buffer_size);
#ifdef INT_CRC
      if (error == bq->buffer_size)
         error += write_all(&bq->file, &crc, sizeof(u32)); // XXX: super small write... could degrade performance
#endif
      bq->csum += crc;
      pthread_mutex_lock(&bq->qlock);
    }
    else { // there were previous errors. skipping the write
      error = write_size;
    }

    if(error < write_size) {
      bq->flags |= BQ_ERROR;
    }
    else {
      written += error;
    }

    PRINTdbg("write done for block %d\n", bq->block_number);
    if (handle->stat_flags & SF_RW) {
       fast_timer_stop(&handle->stats[bq->block_number].write);
       log_histo_add_interval(&handle->stats[bq->block_number].write_h,
                              &handle->stats[bq->block_number].write);
    }

    // even if there was an error, say we wrote the block and move on.
    // the producer thread is responsible for checking the error flag
    // and killing us if needed.
    if (handle->stat_flags & SF_RW)
       fast_timer_start(&handle->stats[bq->block_number].read);

    bq->head = (bq->head + 1) % MAX_QDEPTH;
    bq->qdepth--;

    pthread_cond_signal(&bq->empty);
    pthread_mutex_unlock(&bq->qlock);
  }
  pthread_mutex_unlock(&bq->qlock);


  // close the file and terminate if any errors were encountered
  if (handle->stat_flags & SF_CLOSE)
     fast_timer_start(&handle->stats[bq->block_number].close);

  if ( HNDLOP(close, bq->file) || (bq->flags & BQ_ERROR) ) {
    bq->flags |= BQ_ERROR;      // ensure the error was noted
    PRINTerr("error closing block %d\n", bq->block_number);
    if (handle->stat_flags & SF_THREAD)
       fast_timer_stop(&handle->stats[bq->block_number].thread);
    return NULL; // don't bother trying to rename
  }

  handle->csum[bq->block_number] = bq->csum;
  if(set_block_xattr(bq->handle, bq->block_number) != 0) {
    bq->flags |= BQ_ERROR;
    // if we failed to set the xattr, don't bother with the rename.
    PRINTerr("error setting xattr for block %d\n", bq->block_number);
    if (handle->stat_flags & SF_THREAD)
       fast_timer_stop(&handle->stats[bq->block_number].thread);
    return NULL;
  }
  if (handle->stat_flags & SF_CLOSE)
     fast_timer_stop(&handle->stats[bq->block_number].close);


  // rename
  if (handle->stat_flags & SF_RENAME)
     fast_timer_start(&handle->stats[bq->block_number].rename);

  char block_file_path[MAXNAME];
  //  sprintf( block_file_path, handle->path,
  //           (bq->block_number+handle->status->O)%(handle->status->N+handle->status->E) );
  handle->snprintf( block_file_path, MAXNAME, handle->path,
                    (bq->block_number+handle->status->O)%(handle->status->N+handle->status->E), handle->state );

  if( PATHOP( rename, handle->impl, handle->auth, bq->path, block_file_path ) != 0 ) {
    PRINTerr("bq_writer: failed to rename written file %s\n", bq->path );
    bq->flags |= BQ_ERROR;
  }

#ifdef META_FILES
  // rename the META file too
  strncat( bq->path, META_SFX, strlen(META_SFX)+1 );
  strncat( block_file_path, META_SFX, strlen(META_SFX)+1 );
  if ( PATHOP( rename, handle->impl, handle->auth, bq->path, block_file_path ) != 0 ) {
    PRINTerr("bq_writer: failed to rename written meta file %s\n", bq->path );
    bq->flags |= BQ_ERROR;
  }
#endif

  if (handle->stat_flags & SF_RENAME)
     fast_timer_stop(&handle->stats[bq->block_number].rename);
  if (handle->stat_flags & SF_THREAD)
     fast_timer_stop(&handle->stats[bq->block_number].thread);

  pthread_cleanup_pop(1);
  return NULL;
}

/**
 * Initialize the buffer queues for the handle and start the threads.
 *
 * @return -1 on failure, 0 on success.
 */
static int initialize_queues(ne_handle handle) {
  int i;
  int num_blocks = handle->status->N + handle->status->E;

  /* allocate buffers */
  for(i = 0; i < MAX_QDEPTH; i++) {
    int error = posix_memalign(&handle->buffer_list[i], 64,
                               num_blocks * handle->status->bsz);
    if(error == -1) {
      int j;
      // clean up previously allocated buffers and fail.
      // we can't recover from this error.
      for(j = i-1; j >= 0; j--) {
         free(handle->buffer_list[j]);
      }
      PRINTerr("posix_memalign failed for queue %d\n", i);
      return -1;
    }
  }

  /* open files and initialize BufferQueues */
  for(i = 0; i < num_blocks; i++) {
    int error, file_descriptor;
    char path[MAXNAME];
    BufferQueue *bq = &handle->blocks[i];
    // generate the path
    // sprintf(bq->path, handle->path, (i + handle->status->O) % num_blocks);
    handle->snprintf(bq->path, MAXNAME, handle->path, (i + handle->status->O) % num_blocks, handle->state);

    strcat(bq->path, WRITE_SFX);

    // assign pointers into the memaligned buffers.
    void *buffers[MAX_QDEPTH];
    int j;
    for(j = 0; j < MAX_QDEPTH; j++) {
      buffers[j] = handle->buffer_list[j] + i * handle->status->bsz;
    }
    
    if(bq_init(bq, i, buffers, handle) < 0) {
      // TODO: handle error.
      PRINTerr("bq_init failed for block %d\n", i);
      return -1;
    }

    // start the threads
    error = pthread_create(&handle->threads[i], NULL, bq_writer, (void *)bq);
    if(error != 0) {
      PRINTerr("failed to start thread %d\n", i);
      return -1;
      // TODO: clean up!!
    }
  }

  /* create the buff_list in the handle. */
  for(i = 0; i < MAX_QDEPTH; i++) {
    int j;
    for(j = 0; j < num_blocks; j++) {
      handle->block_buffs[i][j] = handle->buffer_list[i] + j * handle->status->bsz;
    }
  }

  // check for errors on open...
  for(i = 0; i < num_blocks; i++) {
    PRINTdbg("Checking for error opening block %d\n", i);

    BufferQueue *bq = &handle->blocks[i];
    pthread_mutex_lock(&bq->qlock);

    // wait for the queue to be ready.
    while(!(bq->flags & BQ_OPEN) && !(bq->flags & BQ_ERROR))
      pthread_cond_wait(&bq->empty, &bq->qlock);

    if(bq->flags & BQ_ERROR) {
      PRINTerr("open failed for block %d\n", i);
      handle->src_in_err[i] = 1;
      handle->src_err_list[handle->nerr] = i;
      handle->nerr++;
    }
    pthread_mutex_unlock(&bq->qlock);
  }

  return 0;
}

int bq_enqueue(BufferQueue *bq, const void *buf, size_t size) {
  int ret = 0;

  if((ret = pthread_mutex_lock(&bq->qlock)) != 0) {
     PRINTerr("Failed to lock queue for write\n");
    errno = ret;
    return -1;
  }

  while(bq->qdepth == MAX_QDEPTH)
    pthread_cond_wait(&bq->empty, &bq->qlock);

  // NOTE: _Might_ be able to get away with not locking here, since
  // access is controled by the qdepth var, which will not allow a
  // read until we say there is stuff here to be read.
  // 
  // bq->buffers[bq->tail] is a pointer to the beginning of the
  // buffer. bq->buffers[bq->tail] + bq->offset should be a pointer to
  // the inside of the buffer.
  memcpy(bq->buffers[bq->tail]+bq->offset, buf, size);

  if(size+bq->offset < bq->buffer_size) {
    // then this is not a complete block.
    PRINTdbg("saved incomplete buffer for block %d\n", bq->block_number);
    bq->offset += size;
  }
  else {
    bq->offset = 0;
    bq->qdepth++;
    bq->tail = (bq->tail + 1) % MAX_QDEPTH;
    if(bq->flags & BQ_ERROR) {
      ret = 1;
    }
    PRINTdbg("queued complete buffer for block %d\n", bq->block_number);
    pthread_cond_signal(&bq->full);
  }
  pthread_mutex_unlock(&bq->qlock);

  return ret;
}


// unused.  These all devolve to memset(0), which is already done on all
// the BenchStats in a handle, when the handle is initialized
int init_bench_stats(BenchStats* stats) {

   fast_timer_reset(&stats->thread);
   fast_timer_reset(&stats->open);

   fast_timer_reset(&stats->read);
   log_histo_reset(&stats->read_h);

   fast_timer_reset(&stats->write);
   log_histo_reset(&stats->write_h);

   fast_timer_reset(&stats->close);
   fast_timer_reset(&stats->rename);

   fast_timer_reset(&stats->crc);
   log_histo_reset(&stats->crc_h);

   return 0;
}


// This might work, if you have an NFS Multi-Component implementation,
// and your block-directories are named something like /path/to/block%d/more/path/filename,
// and the name of the block 0 dir is /path/to/block0
//
// This is the default, for MC repos.  We ignore <state>
//
// There's an opportunity for MC repos to handle e.g. non-zero naming, etc, by extending the
// the default marfs configuration for MC repos, something like is done for MC_SOCKETS,
// and passing that in as <stat>, here.

int ne_default_snprintf(char* dest, size_t size, const char* format, u32 block, void* state) {
  return snprintf(dest, size, format, block);
}



/**
 * Opens a new handle for a specific erasure striping
 *
 * ne_open(path, mode, ...)  calls this with fn=ne_default_snprintf, and state=NULL
 *
 * @param SnprintfFunc : function takes block-number and <state> and produces per-block path from template.
 * @param state : optional state to be used by SnprintfFunc (e.g. configuration details)
 * @param cred : optional credentials (actually AWSContext*) to authenticate socket connections (e.g. RDMA)
 * @param char* path : sprintf format-template for individual files of in each stripe.
 * @param ne_mode mode : Mode in which the file is to be opened.  Either NE_RDONLY, NE_WRONLY, or NE_REBUILD.
 * @param int erasure_offset : Offset of the erasure stripe, defining the name of the first N file
 * @param int N : Data width of the striping
 * @param int E : Erasure width of the striping
 *
 * @return ne_handle : The new handle for the opened erasure striping
 */


ne_handle ne_open1_vl( SnprintfFunc fn, void* state,
                       uDALType itype, SktAuth auth, StatFlagsValue stat_flags,
                       char *path, ne_mode mode, va_list ap )
{
   char file[MAXNAME];       /* array name of files */
   int counter;
   int ret;
   int N = 0;
   int E = 0;
   int erasure_offset = 0;
#ifdef INT_CRC
   int crccount;
#endif
   int bsz = BLKSZ;

   counter = 3;
   if ( mode & NE_SETBSZ ) {
      counter++;
      mode -= NE_SETBSZ;
      PRINTdbg( "ne_open: NE_SETBSZ flag detected\n");
   }
   if ( mode & NE_NOINFO ) {
      counter -= 3;
      mode -= NE_NOINFO;
      PRINTdbg( "ne_open: NE_NOINFO flag detected\n");
   }

   if ( (mode == NE_WRONLY)  &&  counter < 2 ) {
      PRINTerr( "ne_open: recieved an invalid \"NE_NOINFO\" flag for \"NE_WRONLY\" operation\n");
      errno = EINVAL;
      return NULL;
   }

   // Parse variadic arguments
   if ( counter == 1 ) {
      bsz = va_arg( ap, int );
   }
   else if ( counter > 1 ){
      erasure_offset = va_arg( ap, int );
      N = va_arg( ap, int );
      E = va_arg( ap, int );
      if ( counter == 4 ){
         bsz = va_arg( ap, int );
      }
   }

#ifdef INT_CRC
   //shrink data size to fit crc within block
   bsz -= sizeof( u32 );
#endif

   if ( counter > 1 ) {
      if ( N < 1  ||  N > MAXN ) {
         PRINTerr( "ne_open: improper N arguement received - %d\n", N);
         errno = EINVAL;
         return NULL;
      }
      if ( E < 0  ||  E > MAXE ) {
         PRINTerr( "ne_open: improper E arguement received - %d\n", E);
         errno = EINVAL;
         return NULL;
      }
      if ( erasure_offset < 0  ||  erasure_offset >= N+E ) {
         PRINTerr( "ne_open: improper erasure_offset arguement received - %d\n", erasure_offset);
         errno = EINVAL;
         return NULL;
      }
   }
   if ( bsz < 0  ||  bsz > MAXBLKSZ ) {
      PRINTerr( "ne_open: improper bsz argument received - %d\n", bsz );
      errno = EINVAL;
      return NULL;
   }

   ne_handle handle = malloc( sizeof( struct handle ) );
   if( handle == NULL ) {
      PRINTerr( "ne_open: failed to allocate space for a new handle\n" );
      errno=ENOMEM;
      return NULL;
   }
   memset(handle, 0, sizeof(struct handle));

   /* initialize any non-zero handle members */
   handle->status = malloc( sizeof( struct ne_status_struct ) );
   if( handle == NULL ) {
      PRINTerr( "ne_open: failed to allocate space for a new handle status struct\n" );
      errno=ENOMEM;
      free( handle );
      return NULL;
   }
   handle->status->N = N;
   handle->status->E = E;
   handle->status->bsz = bsz;
   handle->status->O = erasure_offset;
   handle->mode = mode;
   handle->snprintf = fn;
   handle->state    = state;
   handle->auth     = auth;
   handle->impl     = get_impl(itype);

   /* insure a valid implementation */
   if (! handle->impl) {
      PRINTerr( "ne_open: couldn't find implementation for itype %d\n", itype );
      errno = EINVAL;
      free( handle->status );
      free( handle );
      return NULL;
   }

   // flags control collection of timing stats
   handle->stat_flags = stat_flags;
   if (handle->stat_flags) {
      fast_timer_inits();

      // // redundant with memset() on handle
      // init_bench_stats(&handle->agg_stats);
   }
   if (handle->stat_flags & SF_HANDLE)
      fast_timer_start(&handle->handle_timer); /* start overall timer for handle */

   char* nfile = malloc( strlen(path) + 1 );
   strncpy( nfile, path, strlen(path) + 1 );
   handle->path = nfile;

   if ( (mode == NE_REBUILD)  ||  (mode == NE_RDONLY  &&  counter < 2) ) {
      if( mode == NE_REBUILD ) {
         ret = manifest_check( fn, state, handle->impl, auth, path, NE_WRONLY, handle->status ); // identify any manifest errors and stripe properties
      }
      else {
         ret = manifest_check( fn, state, handle->impl, auth, path, mode, handle->status ); // identify total data size of stripe
      }
      if ( ret != 0 ) {
         PRINTerr( "ne_open: manifest check has failed\n" );
         free( handle->status );
         free( handle );
         errno = ENOENT;
         return NULL;
      }
      PRINTdbg("ne_open: Post manifest_check() -- N = %d, E = %d, O = %d, TotSz = %llu\n",
               handle->status->N, handle->status->E, handle->status->O, handle->status->totsz );

   }
   else if ( mode != NE_WRONLY ) { //reject improper mode arguments
      PRINTerr( "improper mode argument received - %d\n", mode );
      free( handle->status );
      free( handle );
      errno = EINVAL;
      return NULL;
   }

   N = handle->status->N;
   E = handle->status->E;
   bsz = handle->status->bsz;
   erasure_offset = handle->status->O;
   PRINTdbg( "ne_open: using stripe values (N=%d,E=%d,bsz=%d,offset=%d)\n", N,E,bsz,erasure_offset);

   // to speed up the rebuild process, verify all stripe parts ahead of time
   if( mode == NE_REBUILD ) {
      ret = part_check( fn, state, handle->impl, auth, path, handle->status ); //verify the existance and size of all data/erasure parts

      //translate data errors into the src_in_err/src_err_list handle structures
      for( counter = 0; counter < N + E; counter++ ) {
         handle->src_in_err[ counter ] = handle->status->data_status[ INDEX_TRANS(counter,erasure_offset,N,E) ];
         if( handle->src_in_err[ counter ] == 1 ) {
            handle->src_err_list[ handle->nerr ] = counter;
            handle->nerr++;
         }
      }

      if( ret != 0 ) {
         PRINTerr( "ne_open: data/erasure part check has failed\n" );
         free( handle->status );
         free( handle );
         errno=ENODATA;
         return NULL;
      }
   }


   if( handle->mode == NE_WRONLY ) { // first cut: mutlti-threading only for writes.
     if(initialize_queues(handle) < 0) {
       // all destruction/cleanup should be handled in initialize_queues()
       free(handle);
       errno = ENOMEM;
       return NULL;
     }
     if( UNSAFE(handle) ) {
       int i;
       for(i = 0; i < handle->status->N + handle->status->E; i++) {
         bq_abort(&handle->blocks[i]);
         // just detach and let the OS clean up. We don't care about the return any more.
         pthread_detach(handle->threads[i]);
       }
     }
   }

   else { // for non-writes, initialize the buffers in the old way.

   /* allocate a big buffer for all the N chunks plus a bit extra for reading in crcs */
#ifdef INT_CRC
     crccount = 1;
     if ( E > 0 )
        crccount = E;

     ret = posix_memalign( &(handle->buffer), 64, ((N+E)*bsz) + (sizeof(u32)*crccount) ); //add space for intermediate checksum
     PRINTdbg("ne_open: Allocated handle buffer of size %zd for bsz=%d, N=%d, E=%d\n",
              ((N+E)*bsz) + (sizeof(u32)*crccount), bsz, N, E);
#else
     ret = posix_memalign( &(handle->buffer), 64, ((N+E)*bsz) );
     PRINTdbg("ne_open: Allocated handle buffer of size %zd for bsz=%d, N=%d, E=%d\n",
              (N+E)*bsz, bsz, N, E);
#endif

     if ( ret != 0 ) {
       PRINTerr( "ne_open: failed to allocate handle buffer\n" );
       errno = ENOMEM;
       return NULL;
     }

     /* loop through and open up all the output files and initilize per part info and allocate buffers */
     counter = 0;
     PRINTdbg( "opening file descriptors...\n" );
     mode_t mask = umask(0000);
     while ( counter < N+E ) {

       if (handle->stat_flags & SF_OPEN)
           fast_timer_start(&handle->stats[counter].open);

       bzero( file, MAXNAME );
       u32 blk_i = INDEX_TRANS(counter,erasure_offset,N,E); // absolute index of block to be written, within pod
       handle->snprintf(file, MAXNAME, path, blk_i, handle->state);
       
#ifdef INT_CRC
       if ( counter > N ) {
         crccount = counter - N;
         handle->buffs[counter] = handle->buffer + ( counter*bsz ) + ( crccount * sizeof(u32) ); //make space for block and erasure crc
       }
       else {
         handle->buffs[counter] = handle->buffer + ( counter*bsz ); //make space for block
       }
#else
       handle->buffs[counter] = handle->buffer + ( counter*bsz ); //make space for block
#endif

       if (handle->stat_flags & SF_OPEN)
          fast_timer_start(&handle->stats[counter].open);

      if( (mode != NE_REBUILD)  ||  (handle->src_in_err[counter] == 0) ) {
         PRINTdbg( "   opening %s for read\n", file );
         OPEN(handle->FDArray[counter], handle, file, O_RDONLY );
      }

      if (handle->stat_flags & SF_OPEN)
         fast_timer_stop(&handle->stats[counter].open);

      if ( FD_ERR(handle->FDArray[counter])  &&  handle->src_in_err[counter] == 0 ) {
         PRINTerr( "   failed to open file %s!!!!\n", file );
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         handle->src_in_err[counter] = 1;
         if ( handle->nerr > E ) { //if errors are unrecoverable, terminate
           errno = ENODATA;
           return NULL;
         }
         if ( mode != NE_REBUILD ) { counter++; }
         
         continue;
       }
      
       counter++;
     }
     umask(mask);
   }
   
   /* allocate matrices */
   handle->encode_matrix = malloc(MAXPARTS * MAXPARTS);
   handle->decode_matrix = malloc(MAXPARTS * MAXPARTS);
   handle->invert_matrix = malloc(MAXPARTS * MAXPARTS);
   handle->g_tbls = malloc(MAXPARTS * MAXPARTS * 32);

   return handle;

}

// caller (e.g. MC-sockets DAL) specifies SprintfFunc, stat, and SktAuth
// New: caller also provides flags that control whether stats are collected
ne_handle ne_open1( SnprintfFunc fn, void* state,
                    uDALType itype, SktAuth auth, StatFlagsValue stat_flags,
                    char *path, ne_mode mode, ... ) {

   va_list vl;
   va_start(vl, mode);
   return ne_open1_vl(fn, state, itype, auth, stat_flags, path, mode, vl);
   va_end(vl);
}


// provide defaults for SprintfFunc, state, and SktAuth
// so naive callers can continue to work (in some cases).
ne_handle ne_open( char *path, ne_mode mode, ... ) {

   // this is safe for builds with/without sockets enabled
   // and with/without socket-authentication enabled
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return NULL;
   }

   va_list vl;
   va_start(vl, mode);
   return ne_open1_vl(ne_default_snprintf, NULL, UDAL_POSIX, auth, 0, path, mode, vl);
   va_end(vl);
}





/**
 * read(2) may return less than the requested read-size, without there being any errors.
 * Call read() repeatedly until the buffer has been completely filled, or an error (or EOF) has occurred.
 *
 * @param GenericFD fd: the file/socket to read from
 * @param void* buffer: buffer to be filled
 * @param size_t nbytes: size of the buffer
 *
 * @return ssize_t: the amount read.  negative for errors.
 */

ssize_t read_all(GenericFD* fd, void* buffer, size_t nbytes) {
  ssize_t     result = 0;
  size_t      remain = nbytes;
  char*       buf    = buffer;  /* assure ourselves pointer arithmetic is by onezies  */

  while (remain) {
    errno = 0;

    ssize_t count = pHNDLOP(read, fd, buf+result, remain);

    if (count < 0)
      return count;

    else if (count == 0)
      return result;            /* EOF */

    //    // COMMENTED OUT: see write_all()
    //    else if (errno)
    //      return -1;

    remain -= count;
    result += count;
  }

  return result;
}

/**
 * Reads nbytes of data at offset from the erasure striping referenced by the given handle
 * @param ne_handle handle : Handle referencing the desired erasure striping
 * @param void* buffer : Memory location in which to store the retrieved data
 * @param int nbytes : Integer number of bytes to be read
 * @param off_t offset : Offset within the data at which to begin the read
 * @return int : The number of bytes read or -1 on a failure
 */
ssize_t ne_read( ne_handle handle, void *buffer, size_t nbytes, off_t offset ) 
{
   int mtot = (handle->status->N)+(handle->status->E);
   int minNerr = handle->status->N+1;  // greater than N
   int maxNerr = -1;   // less than N
   int nsrcerr = 0;
   int counter;
   char firststripe;
   char firstchunk;
   char error_in_stripe;
   unsigned char *temp_buffs[ MAXPARTS ];
   int            temp_buffs_alloc = 0;
   int N = handle->status->N;
   int E = handle->status->E;
   unsigned int bsz = handle->status->bsz;
   int nerr = 0;
   unsigned long datasz[ MAXPARTS ] = {0};
   long ret_in;
   int tmp;
   unsigned int decode_index[ MAXPARTS ];
   u32 llcounter;
   u32 readsize;
   u32 startoffset;
   u32 startpart;
   u32 startstripe;
   u32 tmpoffset;
   u32 tmpchunk;
   u32 endchunk;
#ifdef INT_CRC
   u32 crc;
#endif
   ssize_t out_off;
   off_t seekamt;

   
   if (nbytes > UINT_MAX) {
     PRINTerr( "ne_read: not yet validated for write-sizes above %lu\n", UINT_MAX);
     errno = EFBIG;             /* sort of */
     return -1;
   }

   if ( handle->mode != NE_RDONLY ) {
      PRINTerr( "ne_read: handle is in improper mode for reading!\n" );
      errno = EPERM;
      return -1;
   }

   if ( (offset + nbytes) > handle->status->totsz ) {
      PRINTdbg("ne_read: read would extend beyond EOF, resizing read request...\n");
      nbytes = handle->status->totsz - offset;
      if ( nbytes <= 0 ) {
         PRINTerr( "ne_read: offset is beyond filesize\n" );
         // return -1;             /* pread() would just return 0 in this case */
         return 0;             /* EOF */
      }
   }

   llcounter = 0;
   tmpoffset = 0;

   //check stripe cache
   if ( (offset >= handle->buff_offset)
        &&  (offset < (handle->buff_offset + handle->buff_rem)) ) {
      seekamt = offset - handle->buff_offset;
      readsize = ( nbytes > (handle->buff_rem - seekamt) ) ? (handle->buff_rem - seekamt) : nbytes;
      PRINTdbg( "ne_read: filling request for first %lu bytes from cache with offset %zd in buffer...\n",
                (unsigned long) readsize, seekamt );
      memcpy( buffer, handle->buffer + seekamt, readsize );
      llcounter += readsize;
   }

   //if entire request was cached, nothing remains to be done
   if ( llcounter == nbytes )
      return llcounter;


   //determine min/max errors and allocate temporary buffers
   for ( counter = 0; counter < mtot; counter++ ) {
      if ( handle->src_in_err[counter] ) {
         nerr++;
         if ( counter < N ) { 
            nsrcerr++;
            if ( counter > maxNerr ) { maxNerr = counter; }
            if ( counter < minNerr ) { minNerr = counter; }
         }
      }
   }

   if ( handle->nerr != nerr ) {
      PRINTerr( "ne_read: iconsistent internal state : handle->nerr and handle->src_in_err\n" );
      errno = ENOTRECOVERABLE;
      return -1;
   }


   /******** Rebuild While Reading ********/
read:

   startstripe = (offset+llcounter) / (bsz*N);
   startpart = (offset + llcounter - (startstripe*bsz*N))/bsz;
   startoffset = offset+llcounter - (startstripe*bsz*N) - (startpart*bsz);

   PRINTdbg("ne_read: read with rebuild from startstripe %d startpart %d and startoffset %d for nbytes %d\n",
           startstripe, startpart, startoffset, nbytes);

   counter = 0;

   endchunk = ((offset+nbytes) - (startstripe*N*bsz) ) / bsz;
   int stop = endchunk;

   if ( endchunk > N ) {
      endchunk = N;
      stop = mtot - 1;
   }     

   /**** set seek positions for initial reading ****/
   //if not reading from corrupted chunks, we can just set these normally
   if (startpart > maxNerr  ||  endchunk < minNerr ) {

      for ( counter = 0; counter <= stop; counter++ ) {

#ifdef INT_CRC
         seekamt = startstripe * ( bsz+sizeof(u32) ); 
         if (counter < startpart) {
            seekamt += ( bsz+sizeof(u32) ); 
         }
#else
         seekamt = (startstripe*bsz);
         if (counter < startpart) {
            seekamt += bsz;
         }
         else if (counter == startpart) {
            seekamt += startoffset; 
         }
#endif

         if (handle->stat_flags & SF_RW)
            fast_timer_start(&handle->stats[counter].read);

         if( handle->src_in_err[counter] == 0 ) {
            if ( counter >= N ) {
#ifdef INT_CRC
               seekamt += ( bsz+sizeof(u32) );
#else
               seekamt += bsz;
#endif

               PRINTdbg("seeking erasure file e%d to %zd, as we will be reading from the next stripe\n",counter-N, seekamt);
            }
            else {
               PRINTdbg("seeking input file %d to %zd, as there is no error in this stripe\n",counter, seekamt);
            }


            tmp = HNDLOP(lseek, handle->FDArray[counter], seekamt, SEEK_SET);

            //if we hit an error here, seek positions are wrong and we must restart
            if ( tmp != seekamt ) {
               if ( counter > maxNerr )  maxNerr = counter;
               if ( counter < minNerr )  minNerr = counter;
               handle->src_in_err[counter] = 1;
               handle->src_err_list[handle->nerr] = counter;
               handle->nerr++;
               nsrcerr++;
               handle->e_ready = 0; //indicate that erasure structs require re-initialization

               if (handle->stat_flags & SF_RW) {
                  fast_timer_stop(&handle->stats[counter].read);
                  log_histo_add_interval(&handle->stats[counter].read_h,
                                         &handle->stats[counter].read);
               }

               goto read; //if another error is encountered, start over
            }
         }

         if (handle->stat_flags & SF_RW) {
            fast_timer_stop(&handle->stats[counter].read);
            log_histo_add_interval(&handle->stats[counter].read_h,
                                   &handle->stats[counter].read);
         }
      }
      //temporary addition to allow for the constant reading of erasure parts
      for ( counter = N; counter < mtot; counter++ ) {
         tmp = 0;
         if ( handle->src_in_err[ counter ] == 0 ) {
#ifdef INT_CRC
            tmp = lseek(handle->FDArray[counter],(startstripe*( bsz+sizeof(u32) )),SEEK_SET);
#else
            tmp = lseek(handle->FDArray[counter],(startstripe*bsz),SEEK_SET);
#endif
         }
         //note any errors, no need to restart though
         if ( tmp < 0 ) {
            handle->src_in_err[counter] = 1;
            handle->src_err_list[handle->nerr] = counter;
            handle->nerr++;
            nsrcerr++;
            handle->e_ready = 0; //indicate that erasure structs require re-initialization
         }
      }
      tmpchunk = startpart;
      tmpoffset = startoffset;
      error_in_stripe = 0;
   }


   else {  //if not, we will require the entire stripe for rebuild

      PRINTdbg("startpart = %d, endchunk = %d\n   This stripe contains corrupted blocks...\n", startpart, endchunk);
      while (counter < mtot) {

         if( handle->src_in_err[counter] == 0 ) {

            if (handle->stat_flags & SF_RW)
               fast_timer_start(&handle->stats[counter].read);

#ifdef INT_CRC
            tmp = HNDLOP(lseek, handle->FDArray[counter], (startstripe*( bsz+sizeof(u32) )), SEEK_SET);
#else
            tmp = HNDLOP(lseek, handle->FDArray[counter], (startstripe*bsz), SEEK_SET);
#endif

            //note any errors, no need to restart though
            if ( tmp < 0 ) {
               if ( counter > maxNerr )  maxNerr = counter;
               if ( counter < minNerr )  minNerr = counter;
               handle->src_in_err[counter] = 1;
               handle->src_err_list[handle->nerr] = counter;
               handle->nerr++;
               nsrcerr++;
               handle->e_ready = 0; //indicate that erasure structs require re-initialization
               counter++;

               if (handle->stat_flags & SF_RW) {
                  fast_timer_stop(&handle->stats[counter].read);
                  log_histo_add_interval(&handle->stats[counter].read_h,
                                         &handle->stats[counter].read);
               }
               continue;
            }
#ifdef INT_CRC
            PRINTdbg("seek input file %d to %lu, to read entire stripe\n",counter, (unsigned long)(startstripe*( bsz+sizeof(u32) )));
#else
            PRINTdbg("seek input file %d to %lu, to read entire stripe\n",counter, (unsigned long)(startstripe*bsz));
#endif

            if (handle->stat_flags & SF_RW) {
               fast_timer_stop(&handle->stats[counter].read);
               log_histo_add_interval(&handle->stats[counter].read_h,
                                      &handle->stats[counter].read);
            }
         }

         counter++;
      }

      tmpchunk = 0;
      tmpoffset = 0;
      error_in_stripe = 1;
      //handle->e_ready = 0; //test


      // temp_buffs[] will be needed for regeneration
      for ( counter = 0; counter < mtot; counter++ ) {
         tmp = posix_memalign((void **)&(temp_buffs[counter]),64,bsz);
         if ( tmp != 0 ) {
            PRINTerr( "ne_read: failed to allocate temporary data buffer\n" );
            errno = tmp;
            return -1;
         }
      }
      temp_buffs_alloc = mtot;
   }


   firstchunk = 1;
   firststripe = 1;
   out_off = llcounter;

   /**** output each data stipe, regenerating as necessary ****/
   while ( llcounter < nbytes ) {

      if( handle->nerr > handle->status->E ) {
         PRINTerr("ne_read: errors exceed erasure limits\n");
         errno=ENODATA;
         return llcounter;
      }

      handle->buff_offset = (offset+llcounter);
      handle->buff_rem = 0;

      for ( counter = 0; counter < N; counter++ ) {
         datasz[counter] = 0;
      }

      endchunk = ((long)(offset+nbytes) - (long)( (offset + llcounter) - ((offset+llcounter)%(N*bsz)) ) ) / bsz;

      PRINTdbg( "ne_read: endchunk unadjusted - %d\n", endchunk );
      if ( endchunk >= N ) {
         endchunk = N - 1;
      }

      PRINTdbg("ne_read: endchunk adjusted - %d\n", endchunk);
      if ( endchunk < minNerr ) {
         PRINTdbg( "ne_read: there is no error in this stripe\n");
         error_in_stripe = 0;
      }

      /**** read data into buffers ****/
      for( counter=tmpchunk; counter < N; counter++ ) {

         if ( llcounter == nbytes  &&  error_in_stripe == 0 ) {
            PRINTdbg( "ne_read: data reads complete\n");
            break;
         }

         if (handle->stat_flags & SF_RW)
            fast_timer_start(&handle->stats[counter].read);

         readsize = bsz-tmpoffset;

         if ( handle->src_in_err[counter] == 1 ) {  //this data chunk is invalid
            PRINTdbg("ne_read: ignoring data for faulty chunk %d\n",counter);
            if ( firstchunk == 0 ) {
               llcounter += readsize;

               if ( llcounter < nbytes ) {
                  datasz[counter] = readsize;
               }
               else {
                  datasz[counter] = nbytes - (llcounter - readsize);
                  llcounter=nbytes;
               }
            }
            else if ( counter == startpart ) {
               llcounter += (( readsize - (startoffset - tmpoffset) < (nbytes - llcounter) )
                             ? readsize - (startoffset - tmpoffset)
                             : (nbytes - llcounter));
               datasz[counter] = llcounter - out_off;
               firstchunk = 0;
            }
            // ensure that the stripe is flagged as having an error.
            error_in_stripe = 1;

            if (handle->stat_flags & SF_RW) {
               fast_timer_stop(&handle->stats[counter].read);
               log_histo_add_interval(&handle->stats[counter].read_h,
                                      &handle->stats[counter].read);
            }
         }

         else {    //this data chunk is valid, store it

            if ( (nbytes-llcounter) < readsize  &&  error_in_stripe == 0 )
               readsize = nbytes-llcounter;

#ifdef INT_CRC
            PRINTdbg("ne_read: read %lu from datafile %d\n", bsz+sizeof(crc), counter);
            // ret_in = HNDLOP(read, handle->FDArray[counter], handle->buffs[counter], bsz+sizeof(crc));
            ret_in = read_all(&handle->FDArray[counter], handle->buffs[counter], bsz+sizeof(crc));
            ret_in -= (sizeof(u32)+tmpoffset);
#else
            PRINTdbg("ne_read: read %d from datafile %d\n", readsize, counter);
            // ret_in = HNDLOP(read, handle->FDArray[counter], handle->buffs[counter], readsize);
            ret_in = read_all(&handle->FDArray[counter], handle->buffs[counter], readsize);
#endif
            if (handle->stat_flags & SF_RW) {
               fast_timer_stop(&handle->stats[counter].read);
               log_histo_add_interval(&handle->stats[counter].read_h,
                                      &handle->stats[counter].read);
            }

            //check for a read error
            if ( ret_in < readsize ) {

               PRINTerr( "ne_read: error encountered while reading data file %d "
                         "(expected %d but received %d)\n",
                         counter, readsize, ret_in);
               if ( counter > maxNerr )  maxNerr = counter;
               if ( counter < minNerr )  minNerr = counter;
               handle->src_in_err[counter] = 1;
               handle->src_err_list[handle->nerr] = counter;
               handle->nerr++;
               nsrcerr++;
               handle->e_ready = 0; //indicate that erasure structs require re-initialization
               ret_in = 0;
               counter--;

               //if this is the first encountered error for the stripe, we must start over
               if ( error_in_stripe == 0 ) {
                  for( tmp = counter; tmp >=0; tmp-- ) {
                     llcounter -= datasz[tmp];
                  }
                  PRINTdbg( "ne_read: restarting stripe read, reset total read to %lu\n", (unsigned long)llcounter);
                  goto read;
               }

               continue;
            }


#ifdef INT_CRC
            else {
               //calculate and verify crc
               if (handle->stat_flags & SF_CRC)
                  fast_timer_start(&handle->stats[counter].crc);

               crc = crc32_ieee( TEST_SEED, handle->buffs[counter], bsz );
               int cmp = memcmp( handle->buffs[counter]+bsz, &crc, sizeof(u32) );

               if (handle->stat_flags & SF_CRC) {
                  fast_timer_stop(&handle->stats[counter].crc);
                  log_histo_add_interval(&handle->stats[counter].crc_h,
                                         &handle->stats[counter].crc);
               }

               if ( cmp != 0 ){
                  PRINTerr( "ne_read: mismatch of int-crc for file %d while reading with rebuild\n", counter);
                  if ( counter > maxNerr )  maxNerr = counter;
                  if ( counter < minNerr )  minNerr = counter;
                  handle->src_in_err[counter] = 1;
                  handle->src_err_list[handle->nerr] = counter;
                  handle->nerr++;
                  nsrcerr++;
                  handle->e_ready = 0; //indicate that erasure structs require re-initialization
                  counter--;
                  ret_in = 0;
                  //if this is the first encountered error for the stripe, we must start over
                  if ( error_in_stripe == 0 ) {
                     for( tmp = counter; tmp >=0; tmp-- ) {
                        llcounter -= datasz[tmp];
                     }
                     PRINTdbg( "ne_read: restarting stripe read, reset total read to %lu\n", (unsigned long)llcounter);
                     goto read;
                  }
                  continue;
               }
            }
#endif

            if ( firstchunk == 0 ) {
               llcounter += ret_in;
               if ( llcounter < nbytes ) {
                  datasz[counter] = ret_in;
               }
               else {
                  datasz[counter] = nbytes - (llcounter - ret_in);
                  llcounter = nbytes;
               }
            }
            else if ( counter == startpart ) {
               llcounter += (ret_in - (startoffset-tmpoffset) < (nbytes-llcounter) ) ? ret_in-(startoffset-tmpoffset) : (nbytes-llcounter);
               datasz[counter] = llcounter - out_off;
               firstchunk = 0;
            }

         }

         tmpoffset = 0;

      } //completion of read from stripe



      //notice, we only need the erasure stripes if we hit an error
      counter = N;
      while ( counter < mtot ) { //&&  error_in_stripe == 1 ) {

#ifdef INT_CRC
         readsize = bsz+sizeof(u32);
#else
         readsize = bsz; //may want to limit later
#endif

         if ( handle->src_in_err[counter] == 0 ) {

            PRINTdbg("ne_read: reading %d from erasure %d\n",readsize,counter);
            if (handle->stat_flags & SF_RW)
               fast_timer_start(&handle->stats[counter].read);

            // ret_in = HNDLOP(read, handle->FDArray[counter], handle->buffs[counter], readsize);
            ret_in = read_all(&handle->FDArray[counter], handle->buffs[counter], readsize);

            if (handle->stat_flags & SF_RW) {
               fast_timer_stop(&handle->stats[counter].read);
               log_histo_add_interval(&handle->stats[counter].read_h,
                                      &handle->stats[counter].read);
            }

            if ( ret_in < readsize ) {
               if ( ret_in < 0 ) {
                  ret_in = 0;
               }

               handle->src_in_err[counter] = 1;
               handle->src_err_list[handle->nerr] = counter;
               handle->nerr++;
               handle->e_ready = 0; //indicate that erasure structs require re-initialization
              // error_in_stripe = 1;
               PRINTerr("ne_read: failed to read all erasure data in file %d\n", counter);
               PRINTerr("ne_read: zeroing data for faulty erasure %d from %lu to %d\n",counter,ret_in,bsz);
               bzero(handle->buffs[counter]+ret_in,bsz-ret_in);
               PRINTdbg("ne_read: zeroing temp_data for faulty erasure %d\n",counter);
               bzero(temp_buffs[counter],bsz);
               PRINTdbg("ne_read: done zeroing %d\n",counter);
            }
#ifdef INT_CRC
            else {
               //calculate and verify crc
               if (handle->stat_flags & SF_CRC)
                  fast_timer_start(&handle->stats[counter].crc);

               crc = crc32_ieee( TEST_SEED, handle->buffs[counter], bsz );
               int cmp = memcmp( handle->buffs[counter]+bsz, &crc, sizeof(u32) );

               if (handle->stat_flags & SF_CRC) {
                  fast_timer_stop(&handle->stats[counter].crc);
                  log_histo_add_interval(&handle->stats[counter].crc_h,
                                         &handle->stats[counter].crc);
               }

               if ( cmp != 0 ){
                  PRINTerr("ne_read: mismatch of int-crc for file %d (erasure)\n", counter);
                  if ( counter > maxNerr )  maxNerr = counter;
                  if ( counter < minNerr )  minNerr = counter;
                  handle->src_in_err[counter] = 1;
                  handle->src_err_list[handle->nerr] = counter;
                  handle->nerr++;
                  nsrcerr++;
                  handle->e_ready = 0; //indicate that erasure structs require re-initialization
                  //error_in_stripe = 1;
               }
            }
#endif
         }
         else {
            PRINTdbg( "ne_read: ignoring data for faulty erasure %d\n", counter );
         }
         counter++;
      }

      /**** regenerate from erasure ****/
      if ( error_in_stripe == 1 ) {

         if (handle->stat_flags & SF_ERASURE)
            fast_timer_start(&handle->erasure_timer);

         /* If necessary, initialize the erasure structures */
         if ( handle->e_ready == 0 ) {
            // Generate encode matrix encode_matrix
            // The matrix generated by gf_gen_rs_matrix
            // is not always invertable.
            PRINTdbg("ne_read: initializing erasure structs...\n");
            gf_gen_rs_matrix(handle->encode_matrix, mtot, N);

            // Generate g_tbls from encode matrix encode_matrix
            ec_init_tables(N, E, &(handle->encode_matrix[N * N]), handle->g_tbls);

            ret_in = gf_gen_decode_matrix( handle->encode_matrix, handle->decode_matrix,
                  handle->invert_matrix, decode_index, handle->src_err_list, handle->src_in_err,
                  handle->nerr, nsrcerr, N, mtot);

            if (ret_in != 0) {
               PRINTerr("ne_read: failure to generate decode matrix, errors may exceed erasure limits\n");
               errno=ENODATA;

               for ( counter = 0; counter < temp_buffs_alloc; counter++ )
                  free(temp_buffs[counter]);

               if (handle->stat_flags & SF_ERASURE) {
                  fast_timer_stop(&handle->erasure_timer);
                  log_histo_add_interval(&handle->erasure_h,
                                         &handle->erasure_timer);
               }
               return -1;
            }

            for (tmp = 0; tmp < N; tmp++) {
               handle->recov[tmp] = handle->buffs[decode_index[tmp]];
            }

            PRINTdbg( "ne_read: init erasure tables nsrcerr = %d e_ready = %d...\n", nsrcerr, handle->e_ready );
            ec_init_tables(N, handle->nerr, handle->decode_matrix, handle->g_tbls);

            handle->e_ready = 1; //indicate that rebuild structures are initialized
         }
         PRINTdbg( "ne_read: performing regeneration from erasure...\n" );

         ec_encode_data(bsz, N, handle->nerr, handle->g_tbls, handle->recov, &temp_buffs[N]);

         if (handle->stat_flags & SF_ERASURE) {
            fast_timer_stop(&handle->erasure_timer);
            log_histo_add_interval(&handle->erasure_h,
                                   &handle->erasure_timer);
         }

      }

      /**** write appropriate data out ****/
      for( counter=startpart, tmp=0; counter <= endchunk; counter++ ) {
         readsize = datasz[counter];

#if DEBUG_NE
         if ( readsize+out_off > llcounter ) {
           fprintf(stderr,"ne_read: out_off + readsize(%lu) > llcounter at counter = %d!!!\n",(unsigned long)readsize,counter);

           for ( counter = 0; counter < temp_buffs_alloc; counter++ )
              free(temp_buffs[counter]);

           return -1;
         }
#endif

         if (handle->stat_flags & SF_RW)
            fast_timer_start(&handle->stats[counter].write);

         if ( handle->src_in_err[counter] == 0 ) {
            PRINTdbg( "ne_read: performing write of %d from chunk %d data\n", readsize, counter );

#ifdef INT_CRC
            if ( firststripe  &&  counter == startpart )
#else
            if ( firststripe  &&  counter == startpart  &&  error_in_stripe )
#endif
            {
               PRINTdbg( "ne_read:   with offset of %d\n", startoffset );
               memcpy( buffer+out_off, (handle->buffs[counter])+startoffset, readsize );
            }
            else {
               memcpy( buffer+out_off, handle->buffs[counter], readsize );
            }
         }
         else {

            for ( tmp = 0; counter != handle->src_err_list[tmp]; tmp++ ) {
               if ( tmp == handle->nerr ) {
                  PRINTerr( "ne_read: improperly definded erasure structs, failed to locate %d in src_err_list\n", tmp );
                  errno = ENOTRECOVERABLE;

                  for ( counter = 0; counter < temp_buffs_alloc; counter++ )
                     free(temp_buffs[counter]);

                  if (handle->stat_flags & SF_RW) {
                     fast_timer_stop(&handle->stats[counter].write);
                     log_histo_add_interval(&handle->stats[counter].write_h,
                                            &handle->stats[counter].write);
                  }

                  return -1;
               }
            }

            if ( firststripe == 0  ||  counter != startpart ) {
               PRINTdbg( "ne_read: performing write of %d from regenerated chunk %d data, src_err = %d\n",
                            readsize, counter, handle->src_err_list[tmp] );
               memcpy( buffer+out_off, temp_buffs[N+tmp], readsize );
            }
            else {
               PRINTdbg( "ne_read: performing write of %d from regenerated chunk %d data with offset %d, src_err = %d\n",
                            readsize, counter, startoffset, handle->src_err_list[tmp] );
               memcpy( buffer+out_off, (temp_buffs[N+tmp])+startoffset, readsize );
            }

         } //end of src_in_err = true block

         out_off += readsize;

         if (handle->stat_flags & SF_RW) {
            fast_timer_stop(&handle->stats[counter].write);
            log_histo_add_interval(&handle->stats[counter].write_h,
                                   &handle->stats[counter].write);
         }

      } //end of output loop for stipe data

      if ( out_off != llcounter ) {
         PRINTerr( "ne_read: internal mismatch : llcounter (%lu) and out_off (%zd)\n", (unsigned long)llcounter, out_off );
         errno = ENOTRECOVERABLE;

         for ( counter = 0; counter < temp_buffs_alloc; counter++ )
            free(temp_buffs[counter]);

         return -1;
      }

      firststripe=0;
      tmpoffset = 0; tmpchunk = 0; startpart=0;

    }//end of generating loop for each stripe

   if ( error_in_stripe == 1 ) {
      handle->buff_offset -= ( handle->buff_offset % (N*bsz) );
   }

   //copy regenerated blocks and note length of cached stripe
   for ( counter = 0; counter < mtot; counter++ ) {
      if ( error_in_stripe == 1  &&  counter < N ) {
         if ( handle->src_in_err[counter] == 1 ) {
            for ( tmp = 0; counter != handle->src_err_list[tmp]; tmp++ ) {
               if ( tmp == handle->nerr ) {
                  PRINTerr( "ne_read: improperly definded erasure structs, failed to locate %d in src_err_list while caching\n", tmp );
                  mtot=0;
                  tmp=0;
                  handle->buff_rem -= bsz; //just to offset the later addition
                  break;
               }
            }
            PRINTdbg( "ne_read: caching %d from regenerated chunk %d data, src_err = %d\n", bsz, counter, handle->src_err_list[tmp] );
            memcpy( handle->buffs[counter], temp_buffs[N+tmp], bsz );
         }
         handle->buff_rem += bsz;
      }
      else if ( counter < N ) {
         handle->buff_rem += datasz[counter];
      }
   }

   for ( counter = 0; counter < temp_buffs_alloc; counter++ )
      free(temp_buffs[counter]);

   PRINTdbg( "ne_read: cached %lu bytes from stripe at offset %zd\n", handle->buff_rem, handle->buff_offset );

   return llcounter; 
}

void sync_file(ne_handle handle, int block_index) {
#if 0
  char path[MAXNAME];
  int  block_number = ((handle->status->O + block_index)
                       % (handle->status->N + handle->status->E));
  handle->snprintf(path, MAXNAME, handle->path, block_number, handle->state);
  strcat(path, WRITE_SFX);

  HNDLOP(close, handle->FDArray[block_index]);
  OPEN(handle->FDArray[block_index], handle, path, O_WRONLY);
  if(FD_ERR(handle->FDArray[block_index])) {
    PRINTerr( "failed to reopen file\n");
    handle->src_in_err[block_index] = 1;
    handle->src_err_list[handle->nerr] = block_index;
    handle->nerr++;
    return;
  }

  off_t seek = HNDLOP(lseek, handle->FDArray[block_index],
                      handle->written[block_index],
                      SEEK_SET);
  if(seek < handle->written[block_index]) {
    PRINTerr( "failed to seek reopened file\n");
    handle->src_in_err[block_index] = 1;
    handle->src_err_list[handle->nerr] = block_index;
    handle->nerr++;
    HNDLOP(close, handle->FDArray[block_index]);
    return;
  }

#else
  HNDLOP(fsync, handle->FDArray[block_index]);

#endif
}



/**
 * Writes nbytes from buffer into the erasure striping specified by the provided handle
 * @param ne_handle handle : Handle for the erasure striping to be written to
 * @param void* buffer : Buffer containing the data to be written
 * @param int nbytes : Number of data bytes to be written from buffer
 * @return int : Number of bytes written or -1 on error
 */
ssize_t ne_write( ne_handle handle, const void *buffer, size_t nbytes )
{
 
   int N;                       /* number of raid parts not including E */ 
   int E;                       /* num erasure stripes */
   unsigned int bsz;                     /* chunksize in k */ 
   int counter;                 /* general counter */
   int ecounter;                /* general counter */
   ssize_t ret_out;             /* Number of bytes returned by read() and write() */
   unsigned long long totsize;  /* used to sum total size of the input file/stream */
   int mtot;                    /* N + numerasure stripes */
   u32 readsize;
   u32 writesize;
   u32 crc;                     /* crc 32 */

   if (nbytes > UINT_MAX) {
     PRINTerr( "ne_write: not yet validated for write-sizes above %lu\n", UINT_MAX);
     errno = EFBIG;             /* sort of */
     return -1;
   }

   if ( handle-> mode != NE_WRONLY  &&  handle->mode != NE_REBUILD ) {
     PRINTerr( "ne_write: handle is in improper mode for writing!\n" );
     errno = EPERM;
     return -1;
   }

   N = handle->status->N;
   E = handle->status->E;
   bsz = handle->status->bsz;

   mtot=N+E;


   /* loop until the file input or stream input ends */
   totsize = 0;
   while (1) { 

      counter = handle->buff_rem / bsz;
      /* loop over the parts and write the parts, sum and count bytes per part etc. */
      // NOTE: regarding benchmark timers, this routine just hands off work asynchronously to
      // bq_writer threads.  We let each individual thread maintain its own stats.
      while (counter < N) {

         writesize = ( handle->buff_rem % bsz ); // ? amount being written to block (block size - already written).
         readsize = bsz - writesize; // amount being read for block[block_index] from source buffer

         //avoid reading beyond end of buffer
         if ( totsize + readsize > nbytes ) { readsize = nbytes-totsize; }

         if ( readsize < 1 ) {
            PRINTdbg("ne_write: reading of input is now complete\n");
            break;
         }

         // I think we can understand this as follows: the "read offset" is an
         // offset in the generated erasure data, not including the user's data,
         // and the "write offset" is the logical position in the total output,
         // not including the 4-bytes-per-block of CRC data.
         PRINTdbg( "ne_write: reading input for %lu bytes with offset of %llu "
                   "and writing to offset of %lu in handle buffer\n",
                   (unsigned long)readsize, totsize, handle->buff_rem );
         //memcpy ( handle->buffer + handle->buff_rem, buffer+totsize, readsize);
         int queue_result = bq_enqueue(&handle->blocks[counter], buffer+totsize, readsize);
         if(queue_result == -1) {
           // bq_enqueue will set errno.
           return -1;
         }
         else if(queue_result != 0 && !handle->src_in_err[counter]) {
           handle->src_in_err[counter] = 1;
           handle->src_err_list[handle->nerr] = counter;
           handle->nerr++;
         }
         
         PRINTdbg( "ne_write:   ...copy complete.\n");

         totsize += readsize;
         writesize = readsize + ( handle->buff_rem % bsz );
         handle->buff_rem += readsize;

         if ( writesize < bsz ) {  //if there is not enough data to write a full block, stash it in the handle buffer
            PRINTdbg("ne_write: reading of input is complete, stashed %lu bytes in handle buffer\n", (unsigned long)readsize);
            break;
         }

#ifdef INT_CRC
         writesize += sizeof(crc);
#endif

         handle->written[counter] += writesize;

#ifdef INT_CRC
         writesize -= sizeof(crc);
#endif

         handle->nsz[counter] += writesize;
         handle->ncompsz[counter] += writesize;
         
         counter++;
      } //end of writes for N

      // If we haven't written a whole stripe, terminate. This happens
      // if there is not enough data to form a complete stripe.
      if ( counter != N ) {
         break;
      }


      /* calculate and write erasure */
      if (handle->stat_flags & SF_ERASURE)
         fast_timer_start(&handle->erasure_timer);

      if ( handle->e_ready == 0 ) {
         PRINTdbg( "ne_write: initializing erasure matricies...\n");
         // Generate encode matrix encode_matrix
         // The matrix generated by gf_gen_rs_matrix]
         // is not always invertable.
         gf_gen_rs_matrix(handle->encode_matrix, mtot, N);
         // Generate g_tbls from encode matrix encode_matrix
         ec_init_tables(N, E, &(handle->encode_matrix[N * N]), handle->g_tbls);

         handle->e_ready = 1;
      }

      PRINTdbg( "ne_write: calculating %d recovery blocks from %d data blocks\n",E,N);
      // Perform matrix dot_prod for EC encoding
      // using g_tbls from encode matrix encode_matrix
      // Need to lock the two buffers here.
      int i;
      int buffer_index;
      for(i = N; i < handle->status->N + handle->status->E; i++) {
        BufferQueue *bq = &handle->blocks[i];
        if(pthread_mutex_lock(&bq->qlock) != 0) {
          PRINTerr("Failed to acquire lock for erasure blocks\n");
          return -1;
        }
        while(bq->qdepth == MAX_QDEPTH) {
          pthread_cond_wait(&bq->empty, &bq->qlock);
        }
        if(i == N) {
          buffer_index = bq->tail;
        }
        else {
          assert(buffer_index == bq->tail);
        }
      }

      ec_encode_data(bsz, N, E, handle->g_tbls,
                     (unsigned char **)handle->block_buffs[buffer_index],
                     (unsigned char **)&(handle->block_buffs[buffer_index][N]));

      if (handle->stat_flags & SF_ERASURE) {
         fast_timer_stop(&handle->erasure_timer);
         log_histo_add_interval(&handle->erasure_h,
                                &handle->erasure_timer);
      }

      for(i = N; i < handle->status->N + handle->status->E; i++) {
        BufferQueue *bq = &handle->blocks[i];
        bq->qdepth++;
        bq->tail = (bq->tail + 1) % MAX_QDEPTH;
        pthread_cond_signal(&bq->full);
        pthread_mutex_unlock(&bq->qlock);
        handle->nsz[i] += bsz;
        handle->ncompsz[i] += bsz;
      }

      //now that we have written out all data, reset buffer
      handle->buff_rem = 0; 
   }
   handle->status->totsz += totsize; //as it is impossible to write at an offset, the sum of writes will be the total size

   // If the errors exceed the minimum protection threshold number of
   // errrors then fail the write.
   if( UNSAFE(handle) ) {
     PRINTerr("ne_write: errors exceed minimum protection level (%d)\n",
              MIN_PROTECTION);
     errno = EIO;
     return -1;
   }
   else {
     return totsize;
   }
}



int show_handle_stats(ne_handle handle) {

   if (! handle->stat_flags)
      printf("No stats\n");

   else {
      int simple = (handle->stat_flags & SF_SIMPLE);

      fast_timer_show(&handle->handle_timer,  simple, "handle:  ");
      fast_timer_show(&handle->erasure_timer, simple, "erasure: ");
      printf("\n");
         
      int i;
      int N = handle->status->N;
      int E = handle->status->E;
      for (i=0; i<N+E; ++i) {
         printf("-- block %d\n", i);

         fast_timer_show(&handle->stats[i].thread, simple, "thread:  ");
         fast_timer_show(&handle->stats[i].open,   simple, "open:    ");

         fast_timer_show(&handle->stats[i].read,   simple, "read:    ");
         log_histo_show(&handle->stats[i].read_h,  simple, "read_h:  ");

         fast_timer_show(&handle->stats[i].write,  simple, "write:   ");
         log_histo_show(&handle->stats[i].write_h, simple, "write_h: ");

         fast_timer_show(&handle->stats[i].close,  simple, "close:   ");
         fast_timer_show(&handle->stats[i].rename, simple, "rename:  ");

         fast_timer_show(&handle->stats[i].crc,    simple, "CRC:     ");
         log_histo_show(&handle->stats[i].crc_h,   simple, "CRC_h:   ");
      }
   }

   return 0;
}

/**
 * Closes the erasure striping indicated by the provided handle and flushes
 * the handle buffer, if necessary.
 *
 * @param ne_handle handle : Handle for the striping to be closed
 *
 * @return int : Status code.  Success is indicated by 0, and failure by -1.
 *               A positive value indicates that the operation was
 *               successful, but that errors were encountered in the
 *               stripe.  The Least-Significant Bit of the return code
 *               corresponds to the first of the N data stripe files, while
 *               each subsequent bit corresponds to the next N files and
 *               then the E files.  A 1 in these positions indicates that
 *               an error was encountered while acessing that specific
 *               file.  Note, this code does not account for the offset of
 *               the stripe.  The code will be relative to the file names
 *               only.  (i.e. an error in "<output_path>1<output_path>"
 *               would be encoded in the second bit of the output, a
 *               decimal value of 2)
 */
int ne_close( ne_handle handle ) 
{
   int counter;
   char xattrval[XATTRLEN];
   char file[MAXNAME];       /* array name of files */
   char nfile[MAXNAME];       /* array name of files */
   int N;
   int E;
   unsigned int bsz;
   int ret = 0;
   int tmp;
   unsigned char *zero_buff;

   time_t curtime;
   time(&curtime);


   if ( handle == NULL ) {
      PRINTerr( "ne_close: received a NULL handle\n" );
      errno = EINVAL;
      return -1;
   }
   N = handle->status->N;
   E = handle->status->E;
   bsz = handle->status->bsz;


   /* flush the handle buffer if necessary */
   if ( handle->mode == NE_WRONLY  &&  handle->buff_rem != 0 ) {
      PRINTdbg( "ne_close: flushing handle buffer...\n" );
      //zero the buffer to the end of the stripe
      tmp = (N*bsz) - handle->buff_rem;
      zero_buff = malloc(sizeof(char) * tmp);
      bzero(zero_buff, tmp );

      if ( tmp != ne_write( handle, zero_buff, tmp ) ) { //make ne_write do all the work
         PRINTerr( "ne_close: failed to flush handle buffer\n" );
         ret = -1;
      }

      handle->status->totsz -= tmp;
      free( zero_buff );
   }


   /* Close file descriptors, free buffs, and set xattrs for written files */
   counter = 0;
   while (counter < N+E) {

      char no_rename = 0;
      if (handle->mode == NE_WRONLY ) {
        bq_close(&handle->blocks[counter]);
      }
      else if (! FD_ERR(handle->FDArray[counter])) {

         // RDMA leaves FDs open on the server until timeout or we close.
         // We don't currently have reader-threads (corresponding to
         // bq_writer), so instead of signalling a reader thread to close
         // its connection with the corresponding server, we'll just do it
         // ourselves, right here.
         PRINTdbg( "ne_close: closing read-only block %d\n", counter);

         if ( (HNDLOP(close, handle->FDArray[counter]) != 0)
             && (handle->src_in_err[counter] == 1)  &&  (handle->mode == NE_REBUILD) ) {
            // as this operation can only be read or rebuild, we only 
            // really care if close fails for a rebuild output file
            ret = -1;
            no_rename = 1;
            PRINTerr( "ne_close: close failed for rebuild output file %d, aborting rename for that file\n", counter );
         }
         handle->FDArray[counter] = -1;
      }


      if (handle->mode == NE_REBUILD && handle->src_in_err[counter] == 1 ) {
         // if mode is NE_WRONLY this will be handled by the BQ thread.
         if(set_block_xattr(handle, counter) != 0) {
           no_rename = 1;
           ret = -1;
           DBG_FPRINTF( stderr, "ne_close: failed to set xattr for rebuilt file %d\n", counter );
           // isn't this dead code?
           if(handle->src_in_err[counter] == 0) {
             handle->src_in_err[counter] = 1;
             handle->src_err_list[handle->nerr] = counter;
             handle->nerr++;
           }
           // this should cause a rebuild to fail
           ret = -1;
         }

         handle->snprintf( file, MAXNAME, handle->path, (counter+handle->status->O)%(N+E), handle->state );
         strncpy( nfile, file, strlen(file) + 1);

         // save the original file
         if( handle->e_ready == 1  &&  no_rename == 0 ) {
            char timestamp[30];

            strftime( timestamp, 30, ".rebuild_bkp.%m%d%y-%H%M%S", localtime(&curtime) );

            strncat( file, timestamp, 30 );
            
            // perform the rename
            errno = 0;
            if( rename( nfile, file )  &&  errno != ENOENT ) { //if there is no original, this is not an error
               DBG_FPRINTF( stderr, "ne_close: failed to rename original file \"%s\" to \"%s\"\n", nfile, file );
               ret = -1;
               no_rename = 1;
            }

            strncpy( file, nfile, strlen(nfile) + 1);
         }

         strncat( file, REBUILD_SFX, strlen(REBUILD_SFX) + 1 );

         if( PATHOP( chown, handle->impl, handle->auth, file, handle->status->owner, handle->status->group) ) {
            DBG_FPRINTF( stderr, "ne_close: failed to chown rebuilt file\n" );
            //no_rename = 1;
            //ret = -1;
         }

         if ( handle->e_ready == 1  &&  no_rename == 0 ) {

            if ( PATHOP( rename, handle->impl, handle->auth, file, nfile ) != 0 ) {
               PRINTerr( "ne_close: failed to rename rebuilt file\n" );
               // rebuild should fail even if only one file can't be renamed
               ret = -1;
            }

#ifdef META_FILES
            // corresponding "meta" file ...
            strncat( file,  META_SFX, strlen(META_SFX)+1 );
            strncat( nfile, META_SFX, strlen(META_SFX)+1 );

            if ( PATHOP( rename, handle->impl, handle->auth, file, nfile ) != 0 ) {
               PRINTerr( "ne_close: failed to rename rebuilt meta file\n" );
               // rebuild should fail even if only one file can't be renamed
               ret = -1;
            }
#endif

         }
         else{

            PRINTerr( "ne_close: cleaning up file %s from failed rebuild\n", file );
            PATHOP( unlink, handle->impl, handle->auth, file );

#ifdef META_FILES
            // corresponding "meta" file ...
            strncat( file, META_SFX, strlen(META_SFX)+1 );
            PRINTerr( "ne_close: cleaning up file %s from failed rebuild\n", file );
            PATHOP( unlink, handle->impl, handle->auth, file );
#endif

         }
      }

      counter++;
   }

   if(handle->mode == NE_WRONLY) {
     int i;
     /* wait for the threads */
     for(i = 0; i < handle->status->N + handle->status->E; i++) {
       pthread_join(handle->threads[i], NULL);
       /* add up the errors */
       if((handle->blocks[i].flags & BQ_ERROR) && !handle->src_in_err[i]) {
         handle->src_in_err[i] = 1;
         handle->src_err_list[handle->nerr] = i; //not sure we care about this any more
         handle->nerr++;
       }
       bq_destroy(&handle->blocks[i]);
     }

     /* free the buffers */
     for(i = 0; i < MAX_QDEPTH; i++) {
       free(handle->buffer_list[i]);
     }
   }
   else { // still need to do it the old way for non-writes
     free(handle->buffer);
   }

   if (handle->stat_flags) {
      fast_timer_stop(&handle->handle_timer);
      show_handle_stats(handle);
   }

   if( (UNSAFE(handle) && handle->mode == NE_WRONLY) ) {
     DBG_FPRINTF( stderr, "ne_close: detected unsafe error levels following write operation\n" );
     ret = -1;
   }
   else if( handle->mode == NE_REBUILD  &&  handle->e_ready == 0 ) {
     DBG_FPRINTF( stderr, "ne_close: detected an incomplete/failed rebuild process\n" );
     ret = -1;
   }
   else if ( handle->nerr > handle->E  &&  handle->mode == NE_RDONLY ) { /* for non-writes */
     DBG_FPRINTF( stderr, "ne_close: detected excessive errors following a read operation\n" );
     ret = -1;
   }
   if ( ret == 0 ) {
      PRINTdbg( "ne_close: encoding error pattern in return value...\n" );
      /* Encode any file errors into the return status */
      for( counter = 0; counter < N+E; counter++ ) {
         // note both data/erasure errrors and manifest errors in this code
         if ( handle->src_in_err[counter]  ||  handle->status->manifest_status[INDEX_TRANS(counter,handle->status->O,N,E)] ) {
            ret += ( 1 << INDEX_TRANS(counter,handle->status->O,N,E) );
         }
      }
   }

   if ( handle->path != NULL )
      free(handle->path);

   free(handle->encode_matrix);
   free(handle->decode_matrix);
   free(handle->invert_matrix);
   free(handle->g_tbls);
   
   if (handle->stat_flags & SF_HANDLE)
      fast_timer_stop(&handle->handle_timer); /* overall cost of this op */

   free(handle);
   return ret;
}


/**
 * Determines whether the parent directory of the given file exists
 * @param char* path : Character string to be searched
 * @param int max_length : Maximum length of the character string to be scanned
 * @return int : 0 if the parent directory does exist and -1 if not
 */
int parent_dir_missing(uDALType itype, SktAuth auth, char* path, int max_length ) {
   char*       tmp   = path;
   int         len   = 0;
   int         index = -1;
   const uDAL* impl  = get_impl(itype);

   struct stat status;
   int         res;

   while ( (len < max_length) &&  (*tmp != '\0') ) {
      if( *tmp == '/' )
         index = len;
      len++;
      tmp++;
   }
   
   tmp = path;
   *(tmp + index) = '\0';
   res = PATHOP(stat, impl, auth, tmp, &status );
   PRINTdbg( "parent_dir_missing: stat of \"%s\" returned %d\n", path, res );
   *(tmp + index) = '/';

   return res;
}


/**
 * Deletes the erasure striping of the specified width with the specified path format
 *
 * ne_delete(path, width)  calls this with fn=ne_default_snprintf, and state=NULL
 *
 * @param char* path : Name structure for the files of the desired striping.  This should contain a single "%d" field.
 * @param int width : Total width of the erasure striping (i.e. N+E)
 * @return int : 0 on success and -1 on failure
 */
int ne_delete1( SnprintfFunc snprintf_fn, void* state,
                uDALType itype, SktAuth auth, StatFlagsValue stat_flags,
                char* path, int width ) {

   char  file[MAXNAME];       /* array name of files */
   char  partial[MAXNAME];
   int   counter;
   int   ret = 0;
   int   parent_missing;

   const uDAL* impl = get_impl(itype);

   // flags control collection of timing stats
   FastTimer  timer;            // we don't have an ne_handle
   if (stat_flags & SF_HANDLE) {
      fast_timer_inits();
      fast_timer_start(&timer); /* start overall timer */
   }

   for( counter=0; counter<width; counter++ ) {
      parent_missing = -2;
      bzero( file, sizeof(file) );

      snprintf_fn( file,    MAXNAME, path, counter, state );

      snprintf_fn( partial, MAXNAME, path, counter, state );
      strncat( partial, WRITE_SFX, MAXNAME - strlen(partial) );

      // unlink the file or the unfinished file.  If both fail, check
      // whether the parent directory exists.  If not, indicate an error.
      if ( ne_delete_block1(impl, auth, file)
           &&  PATHOP(unlink, impl, auth, partial )
           &&  (parent_missing = parent_dir_missing(itype, auth, file, MAXNAME)) ) {

         ret = -1;
      }
   }

   if (stat_flags & SF_HANDLE) {
      fast_timer_stop(&timer);
      fast_timer_show(&timer, (stat_flags & SF_SIMPLE),  "delete: ");
   }

   return ret;
}

int ne_delete(char* path, int width ) {

   // This is safe for builds with/without sockets and/or socket-authentication enabled.
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return -1;
   }

   return ne_delete1(ne_default_snprintf, NULL, UDAL_POSIX, auth, 0, path, width);
}



// This is only used from libneTest?  (Maybe this was developed to give
// libneTest an easy way to determin the size of a striped object, so that
// that size could then be provided on a subsequent command-line, to do a
// "read" of the whole file?  If so, you can now just skip providing the
// size on the command-line for a "read", and libneTest will just read the
// whole thing.)
//
// Tweaked: Instead of expecting a template pattern for the path, and then
// adding ".meta", to generate our local template, we expect the caller to
// provide the appropriate template as the path, including ".meta", if they
// want meta.  libneTest will do this.

off_t ne_size1( SnprintfFunc snprintf_fn, void* state,
                uDALType itype, SktAuth auth, StatFlagsValue flags,
                const char* ptemplate, int quorum, int max_stripe_width ) {

   char file[MAXNAME];
   char xattrval[XATTRLEN];

   if( max_stripe_width < 1 )
      max_stripe_width = MAXPARTS;
   if( quorum < 1 )
      quorum = max_stripe_width;
   if( quorum > max_stripe_width ) {
      PRINTerr( "ne_size: received a quorum value greater than the max_stripe_width\n" );
      errno = EINVAL;
      return -1;
   }

   // see comments above OPEN() defn
   GenericFD      fd   = {0};
   struct handle  hndl = {0};
   ne_handle      handle = &hndl;

   handle->impl = get_impl(itype);
   handle->auth = auth;
   if (! handle->impl) {
      PRINTerr( "ne_size: couldn't find implementation for itype %d\n", itype );
      errno = EINVAL;
      return -1;
   }


   off_t sizes_reported[max_stripe_width];
   int   match     = 0;
   off_t prev_size = -1;
   int   i;

   for( i = 0; i < max_stripe_width  &&  match < quorum; i++ ) {
      snprintf_fn( file, MAXNAME, ptemplate, i, state );

      PRINTdbg("ne_size: opening file %s\n", file);
      OPEN( fd, handle, file, O_RDONLY );
      if ( FD_ERR(fd) ) { 
         PRINTerr("ne_size: failed to open file %s\n", file);
         continue;
      }

#ifdef META_FILES

      int tmp = HNDLOP(read, fd, &xattrval[0], XATTRLEN );
      if ( tmp < 0 ) {
         PRINTerr("ne_size: failed to read from file %s\n", file);
         HNDLOP( close, fd );
         continue;
      }
      else if(tmp == 0) {
         PRINTerr( "ne_size: read 0 bytes from metadata file %s\n", file);
         HNDLOP( close, fd );
         continue;
      }

#else

#  if (AXATTR_GET_FUNC == 4)
      if( HNDLOP(fgetxattr, file, XATTRKEY, &xattrval[0], XATTRLEN) )
         continue;
#  else
      if( HNDLOP(fgetxattr, file, XATTRKEY, &xattrval[0], XATTRLEN, 0, 0) )
         continue;
#  endif

#endif //META_FILES

      tmp = HNDLOP( close, fd );
      if ( tmp < 0 ) {
         PRINTerr("ne_size: failed to close file %s\n", file);
         continue;
      }

      PRINTdbg( "ne_size: file %s xattr returned %s\n", file, xattrval );

      sscanf( xattrval, "%*s %*s %*s %*s %*s %*s %*s %zd", &sizes_reported[i] );

      if ( prev_size == -1  ||  sizes_reported[i] == prev_size ) {
         match++;
      }
      else { 
         match = 1;
         int k;
         for( k = 0; k < i; k++ ) {
            if( sizes_reported[k] == sizes_reported[i] ) match++;
         }
      }

      prev_size = sizes_reported[i];
   }

   if( prev_size == -1 ) {
      errno = ENOENT;
      return -1;
   }
   if( match < quorum ) {
      errno = ENODATA;
      return -1;
   }

   return prev_size;
}

off_t ne_size( const char* path, int quorum, int max_stripe_width ) {

   // This is safe for builds with/without sockets and/or socket-authentication enabled.
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return -1;
   }

   return ne_size1(ne_default_snprintf, NULL, UDAL_POSIX, auth, 0, path, quorum, max_stripe_width);
}


/**
 * Internal helper function intended to provide the acceptible number of matching values for manifest_check()
 * @param int N: The N (data) width of the stripe
 * @param int E: The E (erasure) width of the stripe
 * @return int: The acceptible number of matching values before manifest_check() can terminate
 */
int get_consensus( int N, int E ) {
   E = ( E < 0 ) ? MAXE : E;
   N = ( N < 0 ) ? MAXN : N;
   if ( (N >= E)  &&  (E > 0) ) {
      // for large N and small E, verifying E+1 is sufficient
      return E+1;
   }
   else {
      // if N < E, verify N+1; otherwise, verify N
      return (E > 0) ? N+1 : N;
   }
}


/**
 * Internal helper function intended to access and verify erasure stripe manifest info
 * @param 
 * @param 
 * @param 
 * @param 
 * @param char* path : Name structure for the files of the desired striping (should contain a single "%d" field)
 * @param ne_mode mode : Mode flag indicating whether this is a read operation or a rebuild/verify (indicated by NE_WRONLY)
 * @param ne_status status : Status struct to be populated with N/E/O/bsz/totsz and manifest error info
 * @return int : Status code, with 0 indicating success and -1 indicating failure
 */
int manifest_check( SnprintfFunc fn, void* state, const uDAL* impl, SktAuth auth, char *path, ne_mode mode, ne_status status ) {
#define DEFAULT_CONSENSUS 2
   char xattrval[XATTRLEN];   /* char array for storing manifest strings */
   char file[MAXNAME];        /* char array for storing path names */
   char xattrN[5];            /* char array to get n parts from xattr */
   char xattrerasure[5];      /* char array to get erasure parts from xattr */
   char xattroffset[5];       /* char array to get erasure_offset from xattr */
   char xattrchunksizek[20];  /* char array to get chunksize from xattr */
   char xattrnsize[20];       /* char array to get total size from xattr */
   char xattrncompsize[20];   /* char array to get ncompsz from xattr */
   char xattrcsum[50];        /* char array to get check-sum from xattr */
   char xattrtotsize[160];    /* char array to get totsz from xattr */
   int N_list[ MAXPARTS ] = { -1 };
   int E_list[ MAXPARTS ] = { -1 };
   int O_list[ MAXPARTS ] = { -1 };
   unsigned int bsz_list[ MAXPARTS ] = { 0 };
   u64 totsz_list[ MAXPARTS ] = { 0 };
   int N_match = 0;
   int E_match = 0;
   int O_match = 0;
   int bsz_match = 0;
   int totsz_match = 0;

   char info_known = -1; //initialized at -1, set to 0 if temporary N/E are set, and set to 1 if N/E were passed in
   int boundry;   //This is intended to stop manifest check running over files that we know don't exist
   int consensus;
   status->totsz = 0;
   // check if N is already known
   if ( status->N > 0 ) {
      info_known = 1;
      // Note: we now assume that N/E/O/bsz values provided in the status structure are valid
      // Other values (nsz,ncompsz,totsz,etc.) are always assumed to be zero
      boundry = status->N + status->E;
      consensus = get_consensus( status->N, status->E );
      // TODO: if we have N/E/O already, it would be nice to start our check at the offset, in order to avoid overloading the first E+1 servers
   }
   else {
      status->N = -1;
      status->E = -1;
      status->O = -1;
      status->bsz = 0;
      // if not, assume the maximum stripe width
      boundry = MAXPARTS;
      // super arbitrary initial limit for recalulating consensus
      // see the note near the end of this function (starts with "This is a bit tricky") for more explanation.
      consensus = DEFAULT_CONSENSUS;
   }

   if ( (mode != NE_WRONLY) && (mode != NE_RDONLY) ) {
      PRINTerr( "manifest_check: received an unexpected mode argument\n" );
      errno=EINVAL;
      return -1;
   }

   int minmatch=0;      //Indicates the smallest number of matches found amongst manifest values
   int death_knell = 0; //Intended to allow early termination when dealing with very small N/E values
   int counter;
   int tmp_N = -1;
   int tmp_E = -1;
   int ret;
   // loop until we have covered all possible files, or until all values have achieved a minimum consensus
   for ( counter = 0; ( counter < boundry )  &&  ( (mode == NE_WRONLY)  ||  (minmatch < consensus) ); counter++ ) {
      status->manifest_status[ counter ] = 0;  // initialize the status to 'good'
      bzero(file, sizeof(file));
      fn( file, sizeof(file), path, counter, state );
      ret = ne_get_xattr1(impl, auth, file, xattrval, sizeof(xattrval));
      if ( ret < 0 ) {
         PRINTerr( "manifest_check: failure of manifest retrieval for file %s (%d)\n", file, counter );
         status->manifest_status[counter] = 1;
         death_knell++;
         // if we are repeatedly failing to find manifests...
         if ( death_knell > consensus ) {
            if ( info_known == -1 ) {
               if ( (tmp_N != -1)  ||  (tmp_E > 0) ) { // don't use N == -1 and E == 0, that would lock us into finding MAXN matching values!
                  // if we have at least potential values for N or E, use them to calculate consensus
                  consensus = get_consensus( tmp_N, tmp_E );
                  info_known = 0;
                  PRINTdbg( "manifest_check: we have failed to find %d manifests in a row and are substituting potential N/E of %d/%d (new consensus = %d\n", death_knell, tmp_N, tmp_E, consensus );
                  if ( consensus < DEFAULT_CONSENSUS ) { counter = 0; } //special case to restart the scan if the stripe is very narrow
               }
               // otherwise, just continue
            }
            else if ( mode == NE_RDONLY ) {
               // if we have a good idea of N or E and have hit this many errors already, just give up
               PRINTerr( "manifest_check: we have failed to locate %d manifests in a row while consensus = %d.  Giving up now!\n", death_knell, consensus );
               return -1;
            }
         }
         continue;
      }
      death_knell = 0;
      PRINTdbg("manifest_check: file %s (%d) returned manifest \"%s\"\n", file, counter, xattrval );

      ret = sscanf(xattrval,"%4s %4s %4s %19s %19s %19s %49s %159s",
         xattrN,
         xattrerasure,
         xattroffset,
         xattrchunksizek,
         xattrnsize,
         xattrncompsize,
         xattrcsum,
         xattrtotsize);
      if (ret != 8) {
         PRINTerr( "manifest_check: sscanf parsed only %d values from manifest of '%s'\n", ret, file);
         status->manifest_status[counter] = 1;
         continue;
      }

      // parse each value from the manifest, do a sanity check, and count the number of matching values amongst previous manifests
      char* endptr;
      N_list[ counter ] = (int)strtol(xattrN,&(endptr),10);
      if ( *(endptr) != '\0'  ||  (N_list[ counter ] == -1) ) {
         // specifically ignore reading in a -1 as we don't want all of the 'misread' filler values to be counted as matches
         if ( N_list[ counter ] == -1 ) {
            PRINTerr( "manifest_check: got unacceptable value of \'-1\' when parsing N for file %d\n", counter );
         }
         else {
            PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing N for file %d\n", *(endptr), counter);
            N_list[ counter ] = -1;
         }
         status->manifest_status[counter] = 1;
         N_match = 0;
      }
      else if ( info_known != 1 ) {  //TODO: SHOULD PULL THIS OUT AS A MACRO!  REPEATED FOR VARIOUS TYPES BELOW
         // set N_match to indicate the number of matches with the current value
         if ( counter == 0  ||  N_list[ counter ] == N_list[ counter-1 ] ) {
            N_match++;
         }
         else {
            tmp_N = N_list[ counter ];
            N_match = 1;
            int tcnt;
            for( tcnt = 0; tcnt < counter; tcnt++ ) {
               if ( tmp_N == N_list[ tcnt ] )
                  N_match++;
            }
         }
      }
      E_list[ counter ] = (int)strtol(xattrerasure,&(endptr),10);
      if ( *(endptr) != '\0'  ||  (E_list[ counter ] == -1) ) {
         if ( E_list[ counter ] == -1 ) {
            PRINTerr( "manifest_check: got unacceptable value of \'-1\' when parsing E for file %d\n", counter );
         }
         else {
            PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing E for file %d\n", *(endptr), counter);
            E_list[ counter ] = -1;
         }
         status->manifest_status[counter] = 1;
         E_match = 0;
      }
      else if ( info_known != 1 ) {
         // set E_match to indicate the number of matches with the current value
         if ( counter == 0  ||  E_list[ counter ] == E_list[ counter-1 ] ) {
            E_match++;
         }
         else {
            tmp_E = E_list[ counter ];
            E_match = 1;
            int tcnt;
            for( tcnt = 0; tcnt < counter; tcnt++ ) {
               if ( tmp_E == E_list[ tcnt ] )
                  E_match++;
            }
         }
      }
      O_list[ counter ] = (int)strtol(xattroffset,&(endptr),10);
      if ( *(endptr) != '\0'  ||  (O_list[ counter ] == -1) ) {
         if ( O_list[ counter ] == -1 ) {
            PRINTerr( "manifest_check: got unacceptable value of \'-1\' when parsing O for file %d\n", counter );
         }
         else {
            PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing O for file %d\n", *(endptr), counter);
            O_list[ counter ] = -1;
         }
         status->manifest_status[counter] = 1;
         O_match = 0;
      }
      else if ( info_known != 1 ) {
         // set O_match to indicate the number of matches with the current value
         if ( counter == 0  ||  O_list[ counter ] == O_list[ counter-1 ] ) {
            O_match++;
         }
         else {
            O_match = 1;
            int tcnt;
            for( tcnt = 0; tcnt < counter; tcnt++ ) {
               if ( O_list[ counter ] == O_list[ tcnt ] )
                  O_match++;
            }
         }
      }
      bsz_list[ counter ] = (unsigned int)strtoul(xattrchunksizek,&(endptr),10);
      if ( *(endptr) != '\0'  ||  (bsz_list[ counter ] == 0) ) {
         if ( bsz_list[ counter ] == 0 ) {
            PRINTerr( "manifest_check: got unacceptable value of \'0\' when parsing bsz for file %d\n", counter );
         }
         else {
            PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing bsz for file %d\n", *(endptr), counter);
            bsz_list[ counter ] = 0;
         }
         status->manifest_status[counter] = 1;
         bsz_match = 0;
      }
      else if ( info_known != 1 ) {
         // set bsz_match to indicate the number of matches with the current value
         if ( counter == 0  ||  bsz_list[ counter ] == bsz_list[ counter-1 ] ) {
            bsz_match++;
         }
         else {
            bsz_match = 1;
            int tcnt;
            for( tcnt = 0; tcnt < counter; tcnt++ ) {
               if ( bsz_list[ counter ] == bsz_list[ tcnt ] )
                  bsz_match++;
            }
         }
      }
      status->nsz[ counter ] = strtoul(xattrnsize,&(endptr),10);
      if ( *(endptr) != '\0' ) {
         PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing nsz for file %d\n", *(endptr), counter);
         status->manifest_status[counter] = 1;
         status->nsz[ counter ] = 0;
      }
      status->ncompsz[ counter ] = strtoul(xattrncompsize,&(endptr),10);
      if ( *(endptr) != '\0' ) {
         PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing ncompsz for file %d\n", *(endptr), counter);
         status->manifest_status[counter] = 1;
         status->ncompsz[ counter ] = 0;
      }
      status->csum[ counter ] = strtoull(xattrcsum,&(endptr),10);
      if ( *(endptr) != '\0' ) {
         PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing csum for file %d\n", *(endptr), counter);
         status->manifest_status[counter] = 1;
         status->csum[ counter ] = 0;
      }
      totsz_list[ counter ] = strtoull(xattrtotsize,&(endptr),10);
      if ( (*(endptr) != '\0')  ||  (totsz_list[ counter ] == 0) ) {
         if ( totsz_list[ counter ] == 0 ) {
            PRINTerr( "manifest_check: got unacceptable value of \'0\' when parsing totsz for file %d\n", counter );
         }
         else {
            PRINTerr( "manifest_check: strtol detected an invalid character (\'%c\') when parsing totsz for file %d\n", *(endptr), counter);
            totsz_list[ counter ] = 0;
         }
         status->manifest_status[counter] = 1;
         totsz_match = 0;
      }
      else { //at least for now, we still need this for ne_read, even if we already have N/E/O
         // set totsz_match to indicate the number of matches with the current value
         if ( counter == 0  ||  totsz_list[ counter ] == totsz_list[ counter-1 ] ) {
            totsz_match++;
         }
         else {
            totsz_match = 1;
            int tcnt;
            for( tcnt = 0; tcnt < counter; tcnt++ ) {
               if ( totsz_list[ counter ] == totsz_list[ tcnt ] )
                  totsz_match++;
            }
         }
      }

      minmatch = MAXPARTS; //temporarily set this
      int tmp_cnt;
      if ( info_known == 1 ) {
         // check for incorrect N/E/O/bsz values
         if ( status->N != N_list[ counter ]  ||  status->E != E_list[ counter ]  ||  status->O != O_list[ counter ]  ||  status->bsz != bsz_list[ counter ] ) {
            PRINTerr( "manifest_check: detected mismatch between file %d manifest (N/E/O/bsz = %d/%d/%d/%u) and expected values (N/E/O/bsz = %d/%d/%d/%u)\n", counter, N_list[ counter ], E_list[ counter ], O_list[ counter ], bsz_list[ counter ], status->N, status->E, status->O, status->bsz );
            status->manifest_status[ counter ] = 1;
         }
      }
      else {
         // This is a bit tricky.  We need to be able to adapt to reading in correct N/E values and lowering the consensus limit appropriately.
         // However, we don't want to hit a single bad manifest with very low values (N=1,E=0) and suddenly take it at its word (consensus = 1).
         // Therefore, I've arbitrarily set consensus to a low  value that seemed at least reasonable (i.e. 2).  Once at least that many values 
         // agree for N or E, we recalculate consensus.

         if ( info_known == -1 ) {
               // if we've been using a fake consensus value, correct it as soon as we have some level of certainty
               if ( (N_match >= consensus)  ||  ((E_match >= consensus)  &&  (tmp_E != 0)) ) {
                  // note that we don't want to call this when tmp_N==-1 and tmp_E==0
                  // as that would lock us into finding MAXN matching values!
                  consensus = get_consensus( tmp_N, tmp_E );
                  info_known = 0;
                  PRINTdbg( "manifest_check: temporary consensus reached, using new consensus value of %d (file %d)\n", consensus, counter );
               }
               else {
                  // if we still have no good idea of a true consensus, make sure not to set other values
                  continue;
               }
         }

         // update values of N/E, if necessary
         char setNE = 0;
         if ( status->N == -1 ) {
            minmatch = N_match; //update minmatch
            if ( N_match >= consensus ) {
               status->N = N_list[ counter ];
               if( N_match != counter+1 ) { //make sure all non-matching manifests are marked as wrong
                  for ( tmp_cnt = 0; tmp_cnt < counter; tmp_cnt++ ) {
                     if( N_list[tmp_cnt] != N_list[counter] ) { status->manifest_status[ tmp_cnt] = 1; }
                  }
               }
               PRINTdbg( "manifest_check: consensus of %d achieved for N value %d from file %d\n", consensus, status->N, counter );
               setNE = 1;
            }
         }
         else if ( status->N != N_list[ counter ] ) { status->manifest_status[ counter ] = 1; }
         if ( status->E == -1 ) {
            if ( E_match < minmatch ) { minmatch = E_match; } //update minmatch
            if ( E_match >= consensus ) {
               status->E = E_list[ counter ];
               if( E_match != counter+1 ) { //make sure all non-matching manifests are marked as wrong
                  for ( tmp_cnt = 0; tmp_cnt < counter; tmp_cnt++ ) {
                     if( E_list[tmp_cnt] != E_list[counter] ) { status->manifest_status[ tmp_cnt] = 1; }
                  }
               }
               PRINTdbg( "manifest_check: consensus of %d achieved for E value %d from file %d\n", consensus, status->E, counter );
               setNE = 1;
            }
         }
         else if ( status->E != E_list[ counter ] ) { status->manifest_status[ counter ] = 1; }
         if ( setNE ) {
            // if either N or E changed, we need to update the consensus threshold and boundry
            consensus = get_consensus( status->N, status->E );
            if ( status->N != -1  &&  status->E != -1 )
               boundry = status->N + status->E;
         }

         // set Offset and Blk-Size, if not already set
         if ( status->O == -1 ) {
            if ( O_match < minmatch ) { minmatch = O_match; } //update minmatch
            if ( O_match >= consensus ) {
               status->O = O_list[ counter ];
               if( O_match != counter+1 ) { //make sure all non-matching manifests are marked as wrong
                  for ( tmp_cnt = 0; tmp_cnt < counter; tmp_cnt++ ) {
                     if( O_list[tmp_cnt] != O_list[counter] ) { status->manifest_status[ tmp_cnt] = 1; }
                  }
               }
               PRINTdbg( "manifest_check: consensus of %d achieved for O value %d from file %d\n", consensus, status->O, counter );
            }
         }
         else if ( status->O != O_list[ counter ] ) { status->manifest_status[ counter ] = 1; }
         if ( status->bsz == 0 ) {
            if ( bsz_match < minmatch ) { minmatch = bsz_match; } //update minmatch
            if ( bsz_match >= consensus ) {
               status->bsz = bsz_list[ counter ];
               if( bsz_match != counter+1 ) { //make sure all non-matching manifests are marked as wrong
                  for ( tmp_cnt = 0; tmp_cnt < counter; tmp_cnt++ ) {
                     if( bsz_list[tmp_cnt] != bsz_list[counter] ) { status->manifest_status[ tmp_cnt] = 1; }
                  }
               }
               PRINTdbg( "manifest_check: consensus of %d achieved for bsz value %d from file %d\n", consensus, status->bsz, counter );
            }
         }
         else if ( status->bsz != bsz_list[ counter ] ) { status->manifest_status[ counter ] = 1; }

      }

      // set totsz if not set and consensus achieved
      if ( status->totsz == 0 ) {
         if ( totsz_match < minmatch ) { minmatch = totsz_match; } //update minmatch
         if ( totsz_match >= consensus ) {
            status->totsz = totsz_list[ counter ];
            if( totsz_match != counter+1 ) { //make sure all non-matching manifests are marked as wrong
               for ( tmp_cnt = 0; tmp_cnt < counter; tmp_cnt++ ) {
                  if( totsz_list[tmp_cnt] != totsz_list[counter] ) { status->manifest_status[ tmp_cnt] = 1; }
               }
            }
            PRINTdbg( "manifest_check: consensus of %d achieved for totsz value %d from file %d\n", consensus, status->totsz, counter );
         }
      }
      else if ( status->totsz != totsz_list[ counter ] ) { status->manifest_status[ counter ] = 1; }

   } //END FOR-LOOP (reading manifests)

   if ( minmatch < consensus ) { //if we failed to reach consensus, terminate
      PRINTerr( "manifest_check: failed to achieve sufficient consensus amongst values (smallest-match=%d,consensus=%d)\n", minmatch, consensus );
      return -1;
   }

   // perform some last-minute sanity checks on values
   if ( info_known < 1 ) {
      if ( status->N + status->E <= status->O ) {
         PRINTerr( "manifest_check: N/E values of %d/%d are inconsistent with offset of %d\n", status->N, status->E, status->O );
         return -1;
      }
   }
   int valid_cnt = 0;
   for( counter = 0; counter < status->N + status->E; counter++ ) {
      if ( status->nsz[counter] % status->bsz != 0 ) {
         PRINTerr( "manifest_check: bsz value of %u is inconsistent with nsz value of %lu from file %d\n", status->bsz, status->nsz[counter], counter );
         status->manifest_status[counter] = 1;
      }
      if ( (status->nsz[counter] * status->N) - status->totsz >= (status->bsz * status->N) ) {
         PRINTerr( "manifest_check: totsz/bsz/N of %llu/%u/%d are inconsistent with nsz of %lu from file %d\n", status->totsz, status->bsz, status->N, status->nsz[counter], counter );
         status->manifest_status[counter] = 1;
      }
      if ( status->manifest_status[counter] != 1 ) { valid_cnt++; }
   }

   // after marking extra manifests as invalid, make sure we actually have a reasonable number of valid manifests before returning
   if ( valid_cnt < consensus ) {
      PRINTerr( "manifest_check: insufficient valid files detected (found %d valid, but need consensus of %d)\n", valid_cnt, consensus );
      return -1;
   }

   return 0;
}


/**
 * Internal helper function intended to verify the existance/size of all data/erasure files associated with a libne stripe.
 * @param
 * @param
 * @param
 * @param
 * @param char* path: Name structure for the files of the desired striping (should contain a single "%d" field)
 * @param ne_status status: Pointer to the ne_status_structure intended to store the state of each file
 * @return int: 0 on success and -1 on a failure
 */
int part_check( SnprintfFunc fn, void* state, const uDAL* impl, SktAuth auth, char *path, ne_status status ) {
   char file[MAXNAME];       /* char array for names of files */
   struct stat* partstat = malloc (sizeof(struct stat));

   if( partstat == NULL ) {
      PRINTerr( "part_check: failed to allocate memory for a stat struct\n" );
      errno=ENOMEM;
      return -1;
   }

   int faults = 0;
   int counter;
   for( counter = 0; counter < (status->N + status->E); counter++ ) {
      // generate the appropriate file name
      bzero(file,sizeof(file));
      fn( file, MAXNAME, path, counter, state );

      // stat each part-file, verifying existance
      int ret = PATHOP(stat, impl, auth, file, partstat);
      if( ret != 0 ) {
         PRINTerr( "part_check: failed to locate \"%s\" (file %d)\n", file, counter );
         status->data_status[ counter ] = 1;
         faults++;
         continue;
      }
      
      status->owner = partstat->st_uid;
      status->group = partstat->st_gid;

      // If we ever do compression, this will need to be changed to compare against ncompsz
#ifdef INT_CRC
      int blocks = status->nsz[ counter ] / status->bsz;

      if( ( status->nsz[ counter ] + (blocks*32) ) != partstat->st_size )
#else
      if( status->nsz[ counter ] != partstat->st_size )
#endif
      {
         PRINTerr( "part_check: size of %zd for \"%s\" (file %d) is inconsistent with nsz %lu\n", partstat->st_size, file, counter, status->bsz );
         status->data_status[ counter ] = 1;
         faults++;
         continue;
      }

      status->data_status[ counter ] = 0;
   }

   free( partstat );

   // if too many errors were encountered, fail
   if( faults > status->E ) {
      PRINTerr( "part_check: failure -- %d part faults is beyond the erasure limit of %d\n", faults, status->E );
      return -1;
   }

   return 0;
}


/**
 * Internal helper function intended to access xattrs for the purpose of validating/identifying handle information
 * @param ne_handle handle : The handle for the current erasure striping
 * @param char* path : Name structure for the files of the desired striping.  This should contain a single "%d" field.
 * @return int : Status code, with 0 indicating success and -1 indicating failure
 */
int xattr_check( ne_handle handle, char *path ) 
{
   char file[MAXNAME];       /* array name of files */
#ifdef META_FILES
   char nfile[MAXNAME];       /* array name of files */
#endif
   int counter;
   int bcounter;
   int ret;
   int tmp;
   int filefd;
   char xattrval[XATTRLEN];
   char xattrchunks[20];       /* char array to get n parts from xattr */
   char xattrchunksizek[20];   /* char array to get chunksize from xattr */
   char xattrnsize[20];        /* char array to get total size from xattr */
   char xattrerasure[20];      /* char array to get erasure from xattr */
   char xattroffset[20];      /* char array to get erasure_offset from xattr */
   char xattrncompsize[20];    /* general char for xattr manipulation */
   char xattrnsum[50];         /* char array to get xattr sum from xattr */
   char xattrtotsize[160];
   int N = handle->status->N;
   int E = handle->status->E;
   int erasure_offset = handle->status->O;
   unsigned int bsz = handle->status->bsz;
   unsigned long nsz;
   unsigned long ncompsz;
   char goodfile = 0;
   u64 csum;
   u64 totsz;
#ifdef INT_CRC
   unsigned int blocks;
   u32 crc;
#endif
   int N_list[ MAXPARTS ] = { 0 };
   int E_list[ MAXPARTS ] = { 0 };
   int O_list[ MAXPARTS ] = { -1 };
   unsigned int bsz_list[ MAXPARTS ] = { 0 };
   u64 totsz_list[ MAXPARTS ] = { 0 };
   int N_match[ MAXPARTS ] = { 0 };
   int E_match[ MAXPARTS ] = { 0 };
   int O_match[ MAXPARTS ] = { 0 };
   int bsz_match[ MAXPARTS ] = { 0 };
   int totsz_match[ MAXPARTS ] = { 0 };

   struct stat* partstat = malloc (sizeof(struct stat));
   int lN;
   int lE;
  
   if ( handle->mode == NE_STAT  &&  N == 0 ) {
      N = MAXN;
      E = MAXE;
   }

   lN = N;
   lE = E;

#ifdef META_FILES
   GenericFD MetaFDArray[ MAXPARTS ];
   memset(MetaFDArray, 0, sizeof(MetaFDArray));
#endif

   for ( counter = 0; counter < lN+lE; counter++ ) {
      bzero(file,sizeof(file));
      handle->snprintf( file, MAXNAME, path, (counter+handle->status->O)%(lN+lE), handle->state );

      ret = PATHOP(stat, handle->impl, handle->auth, file, partstat);
      PRINTdbg( "xattr_check: stat of file %s returns %d\n", file, ret );
      handle->csum[counter]=0; //reset csum to make results clearer
      if ( ret != 0 ) {
         PRINTerr( "xattr_check: file %s: failure of stat\n", file );
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }
      handle->status->owner = partstat->st_uid;
      handle->status->group = partstat->st_gid;
      bzero(xattrval,sizeof(xattrval));

      ret = ne_get_xattr1(handle->impl, handle->auth, file, xattrval, sizeof(xattrval));
      if (ret < 0) {
         PRINTerr( "xattr_check: failure of xattr retrieval for file %s\n", file);
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }
      PRINTdbg("xattr_check: file %d (%s) xattr returned \"%s\"\n",counter,file,xattrval);

      ret = sscanf(xattrval,"%s %s %s %s %s %s %s %s",
                   xattrchunks,
                   xattrerasure,
                   xattroffset,
                   xattrchunksizek,
                   xattrnsize,
                   xattrncompsize,
                   xattrnsum,
                   xattrtotsize);
      if (ret != 8) {
         PRINTerr( "xattr_check: sscanf parsed only %d values in MD from '%s'\n", ret, file);
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }

      N = atoi(xattrchunks);
      E = atoi(xattrerasure);
      erasure_offset = atoi(xattroffset);
      bsz = atoi(xattrchunksizek);
      nsz = strtol(xattrnsize,NULL,0);
      ncompsz = strtol(xattrncompsize,NULL,0);
      csum = strtoll(xattrnsum,NULL,0);
      totsz = strtoll(xattrtotsize,NULL,0);

#ifdef INT_CRC
      blocks = nsz / bsz;
#endif

      if ( handle->mode != NE_STAT ) { // for 'stat' these handle values will be uninitialized

         /* verify xattr */
         if ( N != handle->status->N ) {
            PRINTerr( "xattr_check: filexattr N = %d did not match handle value  %d\n", N, handle->status->N); 
            handle->src_in_err[counter] = 1;
            handle->src_err_list[handle->nerr] = counter;
            handle->nerr++;
            continue;
         }
         else if ( E != handle->status->E ) {
            PRINTerr( "xattr_check: filexattr E = %d did not match handle value  %d\n", E, handle->status->E); 
            handle->src_in_err[counter] = 1;
            handle->src_err_list[handle->nerr] = counter;
            handle->nerr++;
            continue;
         }
         else if ( bsz != handle->status->bsz ) {
            PRINTerr( "xattr_check: filexattr bsz = %d did not match handle value  %d\n", bsz, handle->status->bsz); 
            handle->src_in_err[counter] = 1;
            handle->src_err_list[handle->nerr] = counter;
            handle->nerr++;
            continue;
         }
         else if ( erasure_offset != handle->status->O ) {
            PRINTerr( "xattr_check: filexattr offset = %d did not match handle value  %d\n", erasure_offset, handle->status->O); 
            handle->src_in_err[counter] = 1;
            handle->src_err_list[handle->nerr] = counter;
            handle->nerr++;
            continue;
         }

      }

      if
#ifdef INT_CRC
         ( ( nsz + (blocks*sizeof(crc)) ) != partstat->st_size )
#else
         ( nsz != partstat->st_size )
#endif
      {
         PRINTerr( "xattr_check: filexattr nsize = %lu did not match stat value %zd (possible missing internal crcs)\n", nsz, partstat->st_size); 
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }
      else if ( (nsz % bsz) != 0 ) {
         PRINTerr( "xattr_check: filexattr nsize = %lu is inconsistent with block size %d \n", nsz, bsz); 
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }
      else if ( (N + E) <= erasure_offset ) {
         PRINTerr( "xattr_check: filexattr offset = %d is inconsistent with stripe width %d\n", erasure_offset, (N+E)); 
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }
      else if
#ifdef INT_CRC
         ( ( ncompsz + (blocks*sizeof(crc)) ) != partstat->st_size )
#else
         ( ncompsz != partstat->st_size )
#endif
      {
         PRINTerr( "xattr_check: filexattr ncompsize = %lu did not match stat value %zd (possible missing crcs)\n", ncompsz, partstat->st_size); 
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }
      else if ( ((ncompsz * N) - totsz) >= bsz*N ) {
         PRINTerr( "xattr_check: filexattr total_size = %llu is inconsistent with ncompsz %lu\n", (unsigned long long)totsz, ncompsz); 
         handle->src_in_err[counter] = 1;
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         continue;
      }
      else {
         PRINTdbg( "setting csum for file %d to %llu\n", counter, (unsigned long long)csum);
         handle->csum[counter] = csum;
         if ( handle->mode == NE_RDONLY ) {
            if( ! handle->status->totsz )
               handle->status->totsz = totsz; //only set the file size if it is not already set (i.e. by a call with mode=NE_STAT)
            continue;
         }

         // This bundle of spaghetti acts to individually verify each "important" xattr value and count matches amongst all files
         char nc = 1, ec = 1, of = 1, bc = 1, tc = 1;
         if ( handle->mode != NE_STAT ) { nc = 0; ec = 0; of = 0; bc = 0; } //if these values are already initialized, skip setting them
         for ( bcounter = 0;
               (( nc || ec || bc || tc || of )  &&  (bcounter < MAXPARTS));
               bcounter++ ) {

            if ( nc ) {
               if ( N_list[bcounter] == N ) {
                  N_match[bcounter]++;
                  nc = 0;
               }
               else if ( N_list[bcounter] == 0 ) {
                  N_list[bcounter] = N;
                  N_match[bcounter]++;
                  nc = 0;
               }
            }

            if ( ec ) {
               if ( E_list[bcounter] == E ) {
                  E_match[bcounter]++;
                  ec = 0;
               }
               else if ( E_list[bcounter] == 0 ) {
                  E_list[bcounter] = E;
                  E_match[bcounter]++;
                  ec = 0;
               }
            }

            if ( of ) {
               if ( O_list[bcounter] == erasure_offset ) {
                  O_match[bcounter]++;
                  of = 0;
               }
               else if ( O_list[bcounter] == -1 ) {
                  O_list[bcounter] = erasure_offset;
                  O_match[bcounter]++;
                  of = 0;
               }
            }

            if ( bc ) {
               if ( bsz_list[bcounter] == bsz ) {
                  bsz_match[bcounter]++;
                  bc = 0;
               }
               else if ( bsz_list[bcounter] == 0 ) {
                  bsz_list[bcounter] = bsz;
                  bsz_match[bcounter]++;
                  bc = 0;
               }
            }

            if ( tc ) {
               if ( totsz_list[bcounter] == totsz ) {
                  totsz_match[bcounter]++;
                  tc = 0;
               }
               else if ( totsz_list[bcounter] == 0 ) {
                  totsz_list[bcounter] = totsz;
                  totsz_match[bcounter]++;
                  tc = 0;
               }
            }
         } //end of value-check loop



         // After we've found some minimum number of metadata values, which
         // all agree on the values of N+E, let's believe that we only need
         // to stat N+E metadata files.

         if ((   lN == MAXN)
             && (lE == MAXE)
             && ((! MIN_MD_CONSENSUS)
                 || (counter >= (MIN_MD_CONSENSUS -1)))) {

            PRINTdbg( "xattr_check: testing for consensus on iteration %d\n", counter);
            if ((     N_match[0] == 0 )     || ( N_match[0] >= MIN_MD_CONSENSUS)
                && (( E_match[0] == 0 )     || ( E_match[0] >= MIN_MD_CONSENSUS))
                && (( O_match[0] == 0 )     || ( O_match[0] >= MIN_MD_CONSENSUS))
                && (( bsz_match[0] == 0 )   || ( bsz_match[0] >= MIN_MD_CONSENSUS))
                && (( totsz_match[0] == 0 ) || ( totsz_match[0] >= MIN_MD_CONSENSUS))) {

               PRINTdbg( "xattr_check: consensus achieved N=%d, E=%d\n", N, E);
               lN = N;
               lE = E;
            }
         }
             


      } //end of else at end of xattr checks


   } //end of loop over files

   free(partstat);
   ret = 0;

   if ( handle->mode != NE_RDONLY ) { //if the handle is uninitialized, store the necessary info

      //loop through the counts of matching xattr values and identify the most prevalent match
      int maxmatch=0;
      int match=-1;
      for ( bcounter = 0; bcounter < MAXPARTS; bcounter++ ) {
         if ( totsz_match[bcounter] > maxmatch ) {
            maxmatch = totsz_match[bcounter];
            match = bcounter;
         }
         if ( bcounter > 0 && N_match[bcounter] > 0 )
            ret = 1;
      }
      if ( match != -1 )
         handle->status->totsz = totsz_list[match];
      else {
         PRINTerr( "xattr_check: failed to locate any matching totsz xattr vals!\n" );
         errno = ENODATA;
         return -1;
      }


      if ( handle->mode == NE_STAT ) {

         //loop through the counts of matching xattr values and identify the most prevalent match
         maxmatch=0;
         match=-1;
         for ( bcounter = 0; bcounter < MAXPARTS; bcounter++ ) {
            if ( N_match[bcounter] > maxmatch ) {
               maxmatch = N_match[bcounter];
               match = bcounter;
            }
            if ( bcounter > 0 && N_match[bcounter] > 0 )
               ret = 1;
         }
         if ( match != -1 )
            handle->status->N = N_list[match];
         else {
            PRINTerr( "xattr_check: failed to locate any matching N xattr vals!\n" );
            errno = ENODATA;
            return -1;
         }


         //loop through the counts of matching xattr values and identify the most prevalent match
         maxmatch=0;
         match=-1;
         for ( bcounter = 0; bcounter < MAXPARTS; bcounter++ ) {
            if ( E_match[bcounter] > maxmatch ) {
               maxmatch = E_match[bcounter];
               match = bcounter;
            }
            if ( bcounter > 0 && N_match[bcounter] > 0 )
               ret = 1;
         }
         if ( match != -1 )
            handle->status->E = E_list[match];
         else {
            PRINTerr( "xattr_check: failed to locate any matching E xattr vals!\n" );
            errno = ENODATA;
            return -1;
         }


         //loop through the counts of matching xattr values and identify the most prevalent match
         maxmatch=0;
         match=-1;
         for ( bcounter = 0; bcounter < MAXPARTS; bcounter++ ) {
            if ( O_match[bcounter] > maxmatch ) {
               maxmatch = O_match[bcounter];
               match = bcounter;
            }
            if ( bcounter > 0 && N_match[bcounter] > 0 )
               ret = 1;
         }
         if ( match != -1 )
            handle->status->O = O_list[match];
         else {
            PRINTerr( "xattr_check: failed to locate any matching offset xattr vals!\n" );
            errno = ENODATA;
            return -1;
         }


         //loop through the counts of matching xattr values and identify the most prevalent match
         maxmatch=0;
         match=-1;
         for ( bcounter = 0; bcounter < MAXPARTS; bcounter++ ) {
            if ( bsz_match[bcounter] > maxmatch ) {
               maxmatch = bsz_match[bcounter];
               match = bcounter;
            }
            if ( bcounter > 0 && N_match[bcounter] > 0 )
               ret = 1;
         }
         if ( match != -1 )
            handle->status->bsz = bsz_list[match];
         else {
            PRINTerr( "xattr_check: failed to locate any matching bsz xattr vals!\n" );
            errno = ENODATA;
            return -1;
         }

      } //end of NE_STAT exclusive checks
   }

   /* If no usable file was located or the number of errors is too great, notify of failure */
   if ( handle->mode != NE_STAT  &&  handle->nerr > handle->status->E ) {
      errno = ENODATA;
      return -1;
   }

   if ( ret != 0 ) {
      PRINTerr( "xattr_check: mismatched xattr values were detected, but not identified!" );
      return 1;
   }

   return 0;
}

// Rebuild functions begin here
typedef struct rebuild_err_struct {
   int FDArray[ MAXPARTS ]; // file descriptors for data/erasure parts in which an error was found
   // per-stripe error info
   int nerr;
   unsigned char src_in_err[ MAXPARTS ];
   unsigned char src_err_list[ MAXPARTS ];
   // per rebuild run error info
   unsigned char per_rebuild_err[ MAXPARTS ];
   // permanent error info
   unsigned char permanent_err[ MAXPARTS ];
} *rebuild_err;


// sets per-stripe error pattern info for an ongoing rebuild
void update_rebuild_err( rebuild_err epat, int block ) {
   if( epat->src_in_err[block] )
     return; //nothing to do

   epat->src_in_err[ block ] = 1;
   //ensure that sources are listed in order
   int i, tmp;
   for ( i = 0; i < epat->nerr; i++ ) {
      if ( epat->src_err_list[i] > block ) { break; }
   }
   while ( i < epat->nerr ) {
      // re-sort the error list.
      tmp = epat->src_err_list[i];
      epat->src_err_list[i] = block;
      block = tmp;
      i++;
   }
   epat->src_err_list[epat->nerr] = block;
   epat->nerr++;
}


// reset per-stripe error info, but keep permanent errors and file descriptors
// returns 0 if erasure structs will require re-initialization and 1 otherwise
int rebuild_err_reset( rebuild_err epat, int stripe_width ) {
   int block, onerr = epat->nerr;
   epat->nerr = 0;
   for( block=0; block < stripe_width; block++ ) {
      epat->src_in_err[ block ] = 0;
      epat->src_err_list[ block ] = 0;
      if( epat->permanent_err[ block ] )
        epat->per_rebuild_err[ block ] = 1;
      //because we have reset nerr and are clearing src_err_list, it is safe to reinsert persistent errors
      if( epat->per_rebuild_err[ block ] )
         update_rebuild_err( epat, block );
   }
   // if nerr is the same before and after, the error pattern hasn't changed
   return ( onerr == epat->nerr ) ? 1 : 0;
}


static int reopen_for_rebuild(ne_handle handle, int block, rebuild_err epat) {
  char file[MAXNAME];

  handle->snprintf(file, MAXNAME, handle->path,
                   (block+handle->status->O)%(handle->status->N+handle->status->E),
                   handle->state);

  PRINTdbg( "   stashing handle for %s\n", &file[0] );
  epat->FDArray[block] = handle->FDArray[block];
  update_rebuild_err( epat, block );
  //Maybe we could close the file here, if a "permanent error" was detected?

  if( handle->mode == NE_STAT ) {
     PRINTdbg( "   setting FD %d to -1\n", block );
     FD_INIT(handle->FDArray[block], handle);
  }
  else {
     PRINTdbg( "   opening %s for write\n", file );
     OPEN(handle->FDArray[block], handle,
          strncat( file, REBUILD_SFX, strlen(REBUILD_SFX)+1 ),
          O_WRONLY | O_CREAT, 0666 );
  }

  // if the error has already been set, just return
  if( handle->src_in_err[block] )
    return 0;

  handle->src_in_err[block] = 1;

  //ensure that sources are listed in order
  int i, tmp;
  for ( i = 0; i < handle->nerr; i++ ) {
    if ( handle->src_err_list[i] > block)
      break;
  }
  while ( i < handle->nerr ) {
    // re-sort the error list.
    tmp = handle->src_err_list[i];
    handle->src_err_list[i] = block;
    block = tmp;
    i++;
  }

  handle->src_err_list[handle->nerr] = block;
  handle->nerr++;
  handle->e_ready = 0; //indicate that erasure structs require re-initialization

  return 0;
}

// Seek to the start of each block file.
// return -1 on fatal error (seek failed that was expected to succeed)
// return 1 on non-fatal error (seek failed, but may still be recoverable).
// return 0 on success.
static int reset_blocks(ne_handle handle, rebuild_err epat) {
  int block_index;
  for(block_index = 0; block_index < handle->status->N + handle->status->E; block_index++) {
    //seek all non-errored files and all rebuild output files
    if(handle->mode != NE_STAT  ||  handle->src_in_err[block_index] == 0) {
      PRINTdbg( "ne_rebuild: performing seek to offset 0 for file %d\n",
                block_index);
      if (HNDLOP(lseek, handle->FDArray[block_index], 0, SEEK_SET) == -1) {
        if(handle->src_in_err[block_index]) {
          PRINTerr( "ne_rebuild: failed to seek ouput file %d (critical error)\n", block_index );
          handle->e_ready = 0;
          return -1;
        }
        else {
          PRINTerr( stderr, "ne_rebuild: encountered error while seeking data/erasure file %d\n", block_index );
          reopen_for_rebuild(handle, block_index,epat);
          epat->per_rebuild_err[ block_index ] = 1;
          return 1;
        }
      }
      
    }
    if ( handle->src_in_err[block_index]  &&  epat->FDArray[block_index] != -1 ) {
      DBG_FPRINTF(stdout,
                  "ne_rebuild: performing seek to offset 0 for in-error file %d\n",
                  block_index);
      // always reattempt a seek of the original, so long as we have a FD
      if ( lseek(epat->FDArray[block_index], 0, SEEK_SET) == -1 ) {
        DBG_FPRINTF(stderr, "ne_rebuild: failed to seek in-error file %d\n", block_index );
        // we skip updating the per-stripe errors here, as that will always be handled later on
        epat->per_rebuild_err[ block_index ] = 1;
      }
    }
  }
  return 0;
}

static int fill_buffers(ne_handle handle, u64 *csum, rebuild_err epat) {
  int          block_index;
  u32          crc;
  const int    ERASURE_WIDTH = handle->status->N + handle->status->E;
#ifdef INT_CRC
  const size_t BUFFER_SIZE   = handle->status->bsz + sizeof(crc);
#else
  const size_t BUFFER_SIZE   = handle->status->bsz;
#endif

  for(block_index = 0; block_index < ERASURE_WIDTH; block_index++) {
    int readFD = handle->FDArray[block_index];
    if ( handle->src_in_err[ block_index ] ) {
      readFD = epat->FDArray[ block_index ];
      if( epat->src_in_err[ block_index ] == 0  &&  readFD == -1 ) {
        epat->permanent_err[ block_index ] = 1;
        epat->per_rebuild_err[ block_index ] = 1;
        update_rebuild_err( epat, block_index );
        DBG_FPRINTF( stderr, "ne_rebuild: encountered -1 FD for in-error file %d\n", block_index );
      }
    }
    if(!epat->src_in_err[block_index]) {
      size_t read_size = read(readFD, handle->buffs[block_index],
                              BUFFER_SIZE);
      if(read_size < BUFFER_SIZE) {
        PRINTerr(
                    "ne_rebuild: encountered error while reading file %d\n",
                    block_index);
        epat->per_rebuild_err[ block_index ] = 1;
        if ( handle->src_in_err[ block_index ] == 0 ) {
          reopen_for_rebuild(handle, block_index,epat);
          return -1;
        }
        update_rebuild_err( epat, block_index );
        handle->e_ready = 0; // force reinit of erasure structs
        continue; //added here to avoid writing to rebuild file
      }
      crc = crc32_ieee( TEST_SEED, handle->buffs[block_index], handle->status->bsz);
      csum[block_index] += crc;

#ifdef INT_CRC
      // verify the stored crc
      u32 *buff_crc = (u32*)(handle->buffs[block_index] + (handle->status->bsz));
      if(*buff_crc != crc) {
        PRINTerr( "ne_rebuild: mismatch of int-crc for file %d\n",
                    block_index);
        if ( handle->src_in_err[ block_index ] == 0 ) {
          reopen_for_rebuild(handle, block_index,epat);
          return -1;
        }
        update_rebuild_err( epat, block_index );
        handle->e_ready = 0; // force reinit of erasure structs
        continue; //added here to avoid writing to rebuild file
      }
#endif
      if( handle->src_in_err[ block_index ]  &&  handle->mode != NE_STAT ) {
        // this is ugly, but due to the structure of the handle buffers, we have to write out this good data block/crc before reading another
        size_t written = write(handle->FDArray[block_index],
                               handle->buffs[block_index], BUFFER_SIZE);
        if( written != BUFFER_SIZE ) {
          DBG_FPRINTF( stderr, "ne_rebuild: failed to write valid buffer to rebuilt file %d (critical error)\n", block_index );
          handle->e_ready = 0;
          epat->nerr = handle->N + handle->E;
          return -1;
        }
        DBG_FPRINTF( stderr, "ne_rebuild: successfully wrote valid buffer out to rebuilt file %d\n", block_index );
        // update manifest values appropriately
        handle->csum[block_index]      += crc;
        handle->nsz[block_index]       += handle->bsz;
        handle->ncompsz[block_index]   += handle->bsz;
      }
    }
  }
  return 0;
}

static int write_buffers(ne_handle handle, unsigned char *rebuild_buffs[], rebuild_err epat) {
  u32 crc;
  int i;
  int written, total_written = 0;
#ifdef INT_CRC
  const size_t BUFFER_SIZE = handle->status->bsz + sizeof(crc);
#else
  const size_t BUFFER_SIZE = handle->status->bsz;
#endif

  for(i = 0; i < epat->nerr; i++) {
    // if we hit an error for this stripe, use the rebuilt buffer to generate a crc
    crc = crc32_ieee(TEST_SEED, rebuild_buffs[handle->status->N+i], handle->status->bsz);
    if(handle->mode != NE_STAT) {
#ifdef INT_CRC
      u32 *buf_crc = (u32*)(rebuild_buffs[handle->status->N+i] + (handle->status->bsz));
      *buf_crc = crc;
#endif
      // written = HNDLOP(write, handle->FDArray[handle->src_err_list[i]],
      //                  rebuild_buffs[handle->status->N+i], BUFFER_SIZE);
      written = write_all(&handle->FDArray[epat->src_err_list[i]],
                      rebuild_buffs[handle->status->N+i], BUFFER_SIZE);
      if(written < BUFFER_SIZE) {
         PRINTerr("failed to write %llu bytes to fd %d\n", BUFFER_SIZE, FD_NUM(handle->FDArray[handle->src_err_list[i]]));
        return -1;
      }
      PRINTdbg("wrote %llu bytes to fd %d\n", BUFFER_SIZE, FD_NUM(handle->FDArray[handle->src_err_list[i]]));
    }
    handle->csum[epat->src_err_list[i]]      += crc;
    handle->nsz[epat->src_err_list[i]]       += handle->status->bsz;
    handle->ncompsz[epat->src_err_list[i]]   += handle->status->bsz;
    total_written                            += handle->status->bsz;
  }
  // have to be careful that this return value does not over-inflate the rebuilt total
  return total_written;
}

// free an array of pointers.
static inline void free_buffers(unsigned char *buffs[], int size) {
  int i;
  for(i = 0; i < size; i++) {
    free(buffs[i]);
  }
}

int do_rebuild(ne_handle handle, rebuild_err epat) {
  int            block_index;
  int            nsrcerr       = 0;
  size_t         rebuilt_size  = 0;
  unsigned char *rebuild_buffs[ MAXPARTS ];
  unsigned int   decode_index[ MAXPARTS ];
  u64            csum[ MAXPARTS ];
  u32            crc;

  const int      ERASURE_WIDTH = handle->status->N + handle->status->E;
#ifdef INT_CRC
  const size_t   BUFFER_SIZE = handle->status->bsz + sizeof(crc);
#else
  const size_t   BUFFER_SIZE = handle->status->bsz;
#endif

  int tmp;
  char alloc_flag = 0;
  if( epat == NULL ) {
    alloc_flag = 1;
    epat = malloc( sizeof( struct rebuild_err_struct ) );
    if ( epat == NULL ) {
      errno = ENOMEM;
      return -1;
    }
  }

  for ( block_index = 0; block_index < ERASURE_WIDTH; block_index++ ) {
    tmp = posix_memalign((void **)&(rebuild_buffs[block_index]),
                         64, BUFFER_SIZE);
    if ( tmp != 0 ) {
      PRINTerr("ne_rebuild: failed to allocate temporary data buffer\n" );
      errno = tmp;
      return -1;
    }
    // clean up epat structures
    if( alloc_flag ) {
      // init the in-error FD array to -1 to avoid confusion
      epat->FDArray[ block_index  ] = -1;
      // clear all permanent errors
      epat->permanent_err[ block_index ] = 0;
    }
    // rebuild now handles opening all output files
    if( handle->src_in_err[ block_index ] ) {
      epat->FDArray[ block_index ] = handle->FDArray[ block_index ];
      if( handle->mode == NE_STAT ) {
        handle->FDArray[ block_index ] = -1;
      }
      else {
        reopen_for_rebuild( handle, block_index, epat );
      }
    }
  }

  PRINTdbg( "ne_rebuild: initiating rebuild operation...\n" );

  // loop over all the data to complete the rebuild.
  while(rebuilt_size < handle->status->totsz) {

    // (re)starting the rebuild. reset checksums. reset position in
    // blocks.
    if(rebuilt_size == 0) {
      epat->nerr = 0;
      for(block_index = 0; block_index < ERASURE_WIDTH; block_index++) {
        epat->src_in_err[ block_index ] = 0;
        epat->src_err_list[ block_index ] = 0;
        epat->per_rebuild_err[ block_index ] = 0;

        if( handle->src_in_err[block_index] == 0 ) {
          csum[block_index] = 0;
        }
        else {
          handle->csum[ block_index ] =    0;
          handle->nsz[ block_index ] =     0;
          handle->ncompsz[ block_index ] = 0;
        }
      }

      int reset_result = reset_blocks(handle,epat);
      if(reset_result == -1) {
        handle->e_ready = 0;
        free_buffers(rebuild_buffs, ERASURE_WIDTH);
        return -1; // fail the rebuild. could not seek.
      }
      else if(reset_result == 1) {
        PRINTerr( "ne_rebuild: restarting rebuild due to seek error");
        rebuilt_size = 0; // restart.
        continue;
      }
    }
    // always reset the error pattern for a new stripe, this will
    // update the stripe to reflect only permanent/per-rebuild errors
    nsrcerr = 0;
    tmp = rebuild_err_reset( epat, ERASURE_WIDTH );
    if( handle->e_ready )
      handle->e_ready = tmp;

    // try to read data from the non-corrupted files, verifies
    // checksums while reading.
    if(fill_buffers(handle, csum, epat) != 0) {
      // failed to read something. Fill_buffers took care of
      // reopening the necessary files.
      if ( epat->nerr == (handle->N + handle->E) ) {
        DBG_FPRINTF( stderr, "ne_rebuild: detected a failure to write to an output file\n" );
        return -1;
      }
      rebuilt_size = 0;
      continue;
    }

    // zero out any errors
    for(block_index = 0; block_index < ERASURE_WIDTH; block_index++) {
      if(epat->src_in_err[block_index]) {
        // Zero buffers for faulty blocks
        PRINTdbg( "ne_rebuild: zeroing data for faulty_file %d\n",
                   block_index);
        if(block_index < handle->status->N) { nsrcerr++; }
        // We don't actually care about int-crcs at this point,
        // those were verified when read.  The erasure will only
        // take place over the data blocks.
        bzero(handle->buffs[block_index], handle->status->bsz);
        bzero(rebuild_buffs[block_index], handle->status->bsz);
      }
    }

    /* Check that errors are still recoverable */
    if(epat->nerr > handle->status->E) {
      PRINTerr( "ne_rebuild: errors exceed regeneration "
                  "capacity of erasure\n");
      errno = ENODATA;
      handle->e_ready = 0;
      free_buffers(rebuild_buffs, ERASURE_WIDTH);
      return -1;
    }

    /* Regenerate stripe from erasure */
    /* If necessary, initialize the erasure structures */
    if(handle->e_ready == 0) {
      // Generate encode matrix encode_matrix. The matrix generated by
      // gf_gen_rs_matrix is not always invertable.
      PRINTdbg("ne_rebuild: initializing erasure structs...\n");
      gf_gen_rs_matrix(handle->encode_matrix, handle->status->N + handle->status->E,
                       handle->status->N);

      // Generate g_tbls from encode matrix encode_matrix
      ec_init_tables(handle->status->N, handle->status->E,
                     &(handle->encode_matrix[handle->status->N * handle->status->N]),
                     handle->g_tbls);

      int decode_result = gf_gen_decode_matrix( handle->encode_matrix,
                                                handle->decode_matrix,
                                                handle->invert_matrix,
                                                decode_index,
                                                epat->src_err_list,
                                                epat->src_in_err,
                                                epat->nerr,
                                                nsrcerr,
                                                handle->status->N,
                                                handle->status->N + handle->status->E);
      if(decode_result != 0) {
        PRINTerr( "ne_rebuild: failure to generate decode matrix\n");
        errno = ENODATA;
        free_buffers(rebuild_buffs, ERASURE_WIDTH);
        return -1;
      }

      int i;
      for(i = 0; i < handle->status->N; i++) {
        handle->recov[i] = handle->buffs[decode_index[i]];
      }

      PRINTdbg( "ne_rebuild: init erasure tables nsrcerr = %d...\n", nsrcerr );
      ec_init_tables(handle->status->N, epat->nerr,
                     handle->decode_matrix, handle->g_tbls);
      handle->e_ready = 1; // indicate that rebuild structures are initialized
    }

    PRINTdbg("ne_rebuild: performing regeneration from erasure...\n" );

    ec_encode_data(handle->status->bsz, handle->status->N, epat->nerr,
                   handle->g_tbls, handle->recov, &rebuild_buffs[handle->status->N]);
    size_t size_written;
    if((size_written = write_buffers(handle, rebuild_buffs, epat)) < 0) {
      free_buffers(rebuild_buffs, ERASURE_WIDTH);
      return -1; // fail the rebuild. something went seriously wrong.
    }

    PRINTdbg( "ne_rebuild: stripe regeneration complete\n" );
    rebuilt_size += handle->status->N * handle->status->bsz;
  }

  // verify block-level crcs
  int retry = 0;
  for (block_index = 0; block_index < ERASURE_WIDTH; block_index++) {
    if(handle->src_in_err[block_index] == 0
       && handle->csum[block_index] != csum[block_index]) {
      PRINTerr( "ne_rebuild: mismatch of crc sum for file %d, "
                  "handle:%llu data:%llu\n", block_index,
                  (unsigned long long)handle->csum[block_index],
                  (unsigned long long)csum[block_index]);
      reopen_for_rebuild(handle, block_index,epat);
      // if we've hit a block-level crc error, we never want to trust that file again
      epat->permanent_err[ block_index ] = 1;
      epat->per_rebuild_err[ block_index ] = 1;
      update_rebuild_err( epat, block_index ); // update this, just to make the next 'early failure' check work
      retry = 1;
    }
  }

  if(retry && handle->mode != NE_STAT) {
    // protect from an infinite recursion
    if( epat->nerr > handle->status->E ) {
      PRINTerr( "ne_rebuild: errors exceed regeneration "
                   "capacity of erasure\n");
      free_buffers(rebuild_buffs, ERASURE_WIDTH);
      errno = ENODATA;
      return -1;
    }
    else {
      int i;
      free_buffers(rebuild_buffs, ERASURE_WIDTH);
      return do_rebuild(handle,epat);
    }
  }

  for ( tmp = 0; tmp < handle->nerr; tmp++ ) {
    int block = handle->src_err_list[ tmp ];
    if( epat->FDArray[ block ] != -1 ) {
      close( epat->FDArray[ block ] ); // we don't really care if this fails
    }
  }
  free( epat );
  free_buffers(rebuild_buffs, ERASURE_WIDTH);
  return 0;
}

/**
 * Performs a rebuild operation on the erasure striping indicated by
 * the given handle.
 *
 * @param ne_handle handle : The handle for the erasure striping to be repaired
 * @return int : Status code.  0 indicates that the object was intact,
 * -1 indicates failure to rebuild, > 0 indicates that the object was
 * degraded and has been rebuilt successfully.
 */
int ne_rebuild( ne_handle handle ) {

   if ( handle == NULL ) {
      PRINTerr( "ne_rebuild: received NULL handle\n" );
      errno = EINVAL;
      return -1;
   }

   if ( handle->mode != NE_REBUILD  &&  handle->mode != NE_STAT ){
      PRINTerr( "ne_rebuild: handle is in improper mode for rebuild operation" );
      errno = EPERM;
      return -1;
   }

   //   init = 0; init should be set to 0 before entering rebuild/retry loop.
   mode_t mask = umask(0000);
   int rebuild_result = do_rebuild(handle,NULL);
   umask(mask);

   return (rebuild_result == 0) ?
     handle->nerr : -1;
}


/**
 * Flushes the handle buffer of the given striping, zero filling the remainder of the stripe data.
 *
 *     Note, at present and paradoxically, this SHOULD NOT be called before
 *     the completion of a series of reads to a file.  Performing a write
 *     after a call to ne_flush WILL result in zero fill remaining within
 *     the erasure striping.
 *
 * @param ne_handle handle : Handle for the erasure striping to be flushed
 * @return int : 0 on success and -1 on failure
 */
//int ne_flush( ne_handle handle ) {
//   int N;
//   int E;
//   unsigned int bsz;
//   int ret = 0;
//   int tmp;
////   int counter;
////   int rem_back;
//   off_t pos[ MAXPARTS ];
//   unsigned char *zero_buff;
//
//   if ( handle == NULL ) {
//      PRINTerr( "ne_flush: received a NULL handle\n" );
//      errno = EINVAL;
//      return -1;
//   }
//
//   if ( handle->mode != NE_WRONLY ) {
//      PRINTerr( "ne_flush: handle is in improper mode for writing\n" );
//      errno = EINVAL;
//   }
//
//   N = handle->N;
//   E = handle->E;
//   bsz = handle->bsz;
//
//   if ( handle->buff_rem == 0 ) {
//      PRINTdbg( "ne_flush: handle buffer is empty, nothing to be done.\n" );
//      return ret;
//   }
//
////   rem_back = handle->buff_rem;
////
////   // store the seek positions for each file
////   for ( counter = 0; counter < (handle->N + handle->E); counter++ ) {
////      pos[counter] = HNDLOP(lseek, handle->FDArray[counter], 0, SEEK_CUR);
////      if ( pos[counter] == -1 ) {
////         PRINTerr( "ne_flush: failed to obtain current seek position for file %d\n", counter );
////         return -1;
////      }
////      if ( (rem_back/(handle->bsz)) == counter ) {
////         pos[counter] += rem_back % handle->bsz;
////      }
////      else if ( (rem_back/(handle->bsz)) > counter ) {
////         pos[counter] += handle->bsz;
////      }
////      fprintf(stdout, "    got seek pos for file %d as %zd ( rem = %d )\n", counter, pos[counter], rem_back );//REMOVE
////   }
//
//
//   PRINTdbg( "ne_flush: flusing handle buffer...\n" );
//   //zero the buffer to the end of the stripe
//   tmp = (N*bsz) - handle->buff_rem;
//   zero_buff = malloc(sizeof(char) * tmp);
//   bzero(zero_buff, tmp );
//
//   if ( tmp != ne_write( handle, zero_buff, tmp ) ) { //make ne_write do all the work
//      PRINTerr( "ne_flush: failed to flush handle buffer\n" );
//      ret = -1;
//   }
//
////   // reset the seek positions for each file
////   for ( counter = 0; counter < (handle->N + handle->E); counter++ ) {
////      if ( HNDLOP(lseek, handle->FDArray[counter], pos[counter], SEEK_SET ) == -1 ) {
////         PRINTerr( "ne_flush: failed to reset seek position for file %d\n", counter );
////         return -1;
////      }
////      fprintf(stdout, "    set seek pos for file %d as %zd\n", counter, pos[counter] ); //REMOVE
////   }
////   handle->buff_rem = rem_back;
//
//   //reset various handle properties
//   handle->totsz -= tmp;
//   free( zero_buff );
//
//   return ret;
//}


#ifndef HAVE_LIBISAL
// This replicates the function defined in libisal.  If we define it here,
// and do static linking with libisal, the linker will complain.

void ec_init_tables(int k, int rows, unsigned char *a, unsigned char *g_tbls)
{
        int i, j;

        for (i = 0; i < rows; i++) {
                for (j = 0; j < k; j++) {
                        gf_vect_mul_init(*a++, g_tbls);
                        g_tbls += 32;
                }
        }
}
#endif

//void dump(unsigned char *buf, int len)
//{
//        int i;
//        for (i = 0; i < len;) {
//                printf(" %2x", 0xff & buf[i++]);
//                if (i % 32 == 0)
//                        printf("\n");
//        }
//        printf("\n");
//}

// Generate decode matrix from encode matrix
static int gf_gen_decode_matrix(unsigned char *encode_matrix,
                                unsigned char *decode_matrix,
                                unsigned char *invert_matrix,
                                unsigned int *decode_index,
                                unsigned char *src_err_list,
                                unsigned char *src_in_err,
                                int nerrs, int nsrcerrs, int k, int m)
{
        int i, j, p;
        int r;
        unsigned char *backup, *b, s;
        int incr = 0;

        b = malloc(MAXPARTS * MAXPARTS);
        backup = malloc(MAXPARTS * MAXPARTS);

        if (b == NULL || backup == NULL) {
           PRINTerr("gf_gen_decode_matrix: failure of malloc\n");
           free(b);
           free(backup);
           errno = ENOMEM;
           return -1;
        }
        // Construct matrix b by removing error rows
        for (i = 0, r = 0; i < k; i++, r++) {
                while (src_in_err[r])
                        r++;
                for (j = 0; j < k; j++) {
                        b[k * i + j] = encode_matrix[k * r + j];
                        backup[k * i + j] = encode_matrix[k * r + j];
                }
                decode_index[i] = r;
        }
        incr = 0;
        while (gf_invert_matrix(b, invert_matrix, k) < 0) {
                if (nerrs == (m - k)) {
                        free(b);
                        free(backup);
                        PRINTerr("gf_gen_decode_matrix: BAD MATRIX\n");
                        return NO_INVERT_MATRIX;
                }
                incr++;
                memcpy(b, backup, MAXPARTS * MAXPARTS);
                for (i = nsrcerrs; i < nerrs - nsrcerrs; i++) {
                        if (src_err_list[i] == (decode_index[k - 1] + incr)) {
                                // skip the erased parity line
                                incr++;
                                continue;
                        }
                }
                if (decode_index[k - 1] + incr >= m) {
                        free(b);
                        free(backup);
                        PRINTerr("gf_gen_decode_matrix: BAD MATRIX\n");
                        return NO_INVERT_MATRIX;
                }
                decode_index[k - 1] += incr;
                for (j = 0; j < k; j++)
                        b[k * (k - 1) + j] = encode_matrix[k * decode_index[k - 1] + j];

        };

        if (b == NULL || backup == NULL) {
           PRINTerr("gf_gen_decode_matrix: failure of malloc\n");
           free(b);
           free(backup);
           errno = ENOMEM;
           return -1;
        }
        // Construct matrix b by removing error rows
        for (i = 0, r = 0; i < k; i++, r++) {
                while (src_in_err[r])
                        r++;
                for (j = 0; j < k; j++) {
                        b[k * i + j] = encode_matrix[k * r + j];
                        backup[k * i + j] = encode_matrix[k * r + j];
                }
                decode_index[i] = r;
        }
        incr = 0;
        while (gf_invert_matrix(b, invert_matrix, k) < 0) {
                if (nerrs == (m - k)) {
                        free(b);
                        free(backup);
                        PRINTerr("gf_gen_decode_matrix: BAD MATRIX\n");
                        return NO_INVERT_MATRIX;
                }
                incr++;
                memcpy(b, backup, MAXPARTS * MAXPARTS);
                for (i = nsrcerrs; i < nerrs - nsrcerrs; i++) {
                        if (src_err_list[i] == (decode_index[k - 1] + incr)) {
                                // skip the erased parity line
                                incr++;
                                continue;
                        }
                }
                if (decode_index[k - 1] + incr >= m) {
                        free(b);
                        free(backup);
                        PRINTerr("gf_gen_decode_matrix: BAD MATRIX\n");
                        return NO_INVERT_MATRIX;
                }
                decode_index[k - 1] += incr;
                for (j = 0; j < k; j++)
                        b[k * (k - 1) + j] = encode_matrix[k * decode_index[k - 1] + j];

        };

        for (i = 0; i < nsrcerrs; i++) {
                for (j = 0; j < k; j++) {
                        decode_matrix[k * i + j] = invert_matrix[k * src_err_list[i] + j];
                }
        }
        /* src_err_list from encode_matrix * invert of b for parity decoding */
        for (p = nsrcerrs; p < nerrs; p++) {
                for (i = 0; i < k; i++) {
                        s = 0;
                        for (j = 0; j < k; j++)
                                s ^= gf_mul(invert_matrix[j * k + i],
                                            encode_matrix[k * src_err_list[p] + j]);

                        decode_matrix[k * p + i] = s;
                }
        }
        free(b);
        free(backup);
        return 0;
}


/**
 * Performs a rebuild operation on the erasure striping indicated by the given handle, but ignores faulty xattr values.
 * @param ne_handle handle : The handle for the erasure striping to be repaired
 * @return int : Status code.  Success is indicated by 0 and failure by -1
 */
int ne_noxattr_rebuild(ne_handle handle) {
   while ( handle->nerr > 0 ) {
      handle->nerr--;
      handle->src_in_err[handle->src_err_list[handle->nerr]] = 0;
      handle->src_err_list[handle->nerr] = 0;
   }
   return ne_rebuild( handle ); 
}


/**
 * Retrieves the health and parameters for the erasure striping
 * indicated by the provided path and offset
 *
 * ne_status(path) calls this with fn=ne_default_snprintf, and state=NULL
 *
 * @param SnprintfFunc fn : function takes block-number and <state> and produces per-block path from template.
 * @param void* state : optional state to be used by SnprintfFunc (e.g. configuration details)
 * @param SktAuth auth : authentication may be required for RDMA uDALTypes
 * @param StatFlagsValue flags : flags control the collection of *statistics*.  (Confusing, in this particular function.)
 * @param uDALType itype : select the underlying file-system implementation (RDMA versus POSIX).
 * @param char* path : sprintf format-template for individual files of in each stripe.
 *
 * @return nestat : Status structure containing the encoded error
 *                  pattern of the stripe (as with ne_close) as well
 *                  as the number of data parts (N), number of erasure
 *                  parts (E), and blocksize (bsz) for the stripe.
 */

ne_stat ne_status1( SnprintfFunc fn, void* state,
                    uDALType itype, SktAuth auth, StatFlagsValue timer_flags,
                    char *path )
{
   char file[MAXNAME];       /* array name of files */
   int counter;
   int ret;
#ifdef INT_CRC
   int crccount;
   unsigned int bsz = BLKSZ - sizeof( u32 );
#else
   unsigned int bsz = BLKSZ;
#endif

   ne_stat   stat   = malloc( sizeof( struct ne_stat_struct ) );
   ne_handle handle = malloc( sizeof( struct handle ) );
   if ( stat == NULL  ||  handle == NULL ) {
      PRINTerr( "ne_status: failed to allocate stat/handle structures!\n" );
      return NULL;
   }
   memset(handle, 0, sizeof(struct handle));

   handle->impl = get_impl(itype);

   // flags control collection of timing stats
   handle->stat_flags = timer_flags;
   if (timer_flags) {
      fast_timer_inits();

      // // redundant with memset() on handle
      // init_bench_stats(&handle->agg_stats);
   }
   if (handle->stat_flags & SF_HANDLE)
      fast_timer_start(&handle->handle_timer); /* start overall timer for handle */


   /* initialize stored info */
   for ( counter=0; counter < MAXPARTS; counter++ ) {
      handle->csum[counter] = 0;
      handle->nsz[counter] = 0;
      handle->ncompsz[counter] = 0;
      handle->src_in_err[counter] = 0;
      handle->src_err_list[counter] = 0;
      stat->data_status[counter] = 0;
      stat->xattr_status[counter] = 0;
   }
   handle->nerr           = 0;
   handle->status->totsz          = 0;
   handle->status->N              = 0;
   handle->status->E              = 0;
   handle->status->bsz            = 0;
   handle->status->O = 0;
   handle->mode           = NE_STAT;
   handle->e_ready        = 0;
   handle->buff_offset    = 0;
   handle->buff_rem       = 0;

   handle->snprintf = fn;
   handle->state    = state;
   handle->auth     = auth;

   char* nfile = malloc( strlen(path) + 1 );
   strncpy( nfile, path, strlen(path) + 1 );
   handle->path = nfile;

   ret = xattr_check(handle, path); // identify total data size of stripe
   if( ret == -1 ) {
      PRINTerr( "ne_status: extended attribute check has failed\n" );
      free( handle );
      return NULL;
   }

   while ( handle->nerr > 0 ) {
      handle->nerr--;
      handle->src_in_err[handle->src_err_list[handle->nerr]] = 0;
      handle->src_err_list[handle->nerr] = 0;
   }

   handle->mode = NE_REBUILD;
   ret = xattr_check(handle, path); //verify the stripe, now that values have been established
   if ( ret == -1 ) {
      PRINTerr( "ne_status: extended attribute check has failed\n" );
      free( handle );
      return NULL;
   }
   handle->mode = NE_STAT;

   PRINTdbg( "ne_status: Post xattr_check() -- NERR = %d, N = %d, E = %d, Start = %d, TotSz = %llu\n",
             handle->nerr, handle->status->N, handle->status->E, handle->status->O, handle->status->totsz );

   stat->N = handle->status->N;
   stat->E = handle->status->E;
   stat->bsz = handle->status->bsz;
   stat->totsz = handle->status->totsz;
   stat->start = handle->status->O;

   // store xattr failures to stat struct and reset error data
   for ( counter = 0; counter < ( handle->status->N + handle->status->E ); counter++ ) {
      if ( counter < handle->nerr ) {
         stat->xattr_status[handle->src_err_list[counter]] = 1;
         handle->src_err_list[counter] = 0;
      }
      handle->src_in_err[counter] = 0;
   }
   handle->nerr = 0;

   /* allocate a big buffer for all the N chunks plus a bit extra for reading in crcs */
#ifdef INT_CRC
   crccount = 1;
   if ( handle->status->E > 0 )
      crccount = handle->status->E;

   // add space for intermediate checksum
   ret = posix_memalign( &(handle->buffer), 64,
                         ((handle->status->N+handle->status->E)*bsz) + (sizeof(u32)*crccount) );
   PRINTdbg("ne_stat: Allocated handle buffer of size %zd for bsz=%d, N=%d, E=%d\n",
            ((handle->status->N+handle->status->E)*bsz) + (sizeof(u32)*crccount),
            handle->status->bsz, handle->status->N, handle->status->E);
#else
   ret = posix_memalign( &(handle->buffer), 64,
                         ((handle->status->N+handle->status->E)*bsz) );
   PRINTdbg("ne_stat: Allocated handle buffer of size %d for bsz=%d, N=%d, E=%d\n",
            (handle->status->N+handle->status->E)*bsz, handle->status->bsz, handle->status->N, handle->status->E);
#endif
   if ( ret != 0 ) {
      PRINTerr( "ne_status: failed to allocate handle buffer\n" );
      errno = ret;
      return NULL;
   }

   /* allocate matrices */
   handle->encode_matrix = malloc(MAXPARTS * MAXPARTS);
   handle->decode_matrix = malloc(MAXPARTS * MAXPARTS);
   handle->invert_matrix = malloc(MAXPARTS * MAXPARTS);
   handle->g_tbls = malloc(MAXPARTS * MAXPARTS * 32);


   /* loop through and open up all the output files, initilize per part info, and allocate buffers */
   counter = 0;
   PRINTdbg( "ne_status: opening file descriptors...\n" );
   while ( counter < (handle->status->N+handle->status->E) ) {
      bzero( file, MAXNAME );
      handle->snprintf( file, MAXNAME, path, (counter+handle->status->O)%(handle->status->N+handle->status->E), handle->state );

#ifdef INT_CRC
      if ( counter > handle->status->N ) {
         crccount = counter - handle->status->N;
         handle->buffs[counter] = handle->buffer + ( counter*bsz ) + ( crccount * sizeof(u32) ); //make space for block and erasure crc
      }
      else {
         handle->buffs[counter] = handle->buffer + ( counter*bsz ); //make space for block
      }
#else
      handle->buffs[counter] = handle->buffer + ( counter*bsz ); //make space for block
#endif

      PRINTdbg( "ne_status:    opening %s for read\n", file );
      OPEN(handle->FDArray[counter], handle, file, O_RDONLY);

      if ( FD_ERR(handle->FDArray[counter])  &&  handle->src_in_err[counter] == 0 ) {
         PRINTerr( "ne_status:    failed to open file %s!!!!\n", file );
         handle->src_err_list[handle->nerr] = counter;
         handle->nerr++;
         handle->src_in_err[counter] = 1;
         counter++;

         continue;
      }

      counter++;
   }

   if ( ne_rebuild( handle ) < 0 ) {
      PRINTerr( "ne_status: rebuild indicates that data is unrecoverable\n" );
   }

   // store data failures to stat struct
   for ( counter = 0; counter < handle->nerr; counter++ ) {
      stat->data_status[handle->src_err_list[counter]] = 1;
   }


   /* Close file descriptors and free bufs */
   counter = 0;
   while (counter < (handle->status->N+handle->status->E) ) {

      if ( handle->src_in_err[counter] == 0  &&  FD_ERR(handle->FDArray[counter]) ) {
        HNDLOP(close, handle->FDArray[counter]);
      }

      counter++;
   }
   free(handle->buffer);
   free(handle->encode_matrix);
   free(handle->decode_matrix);
   free(handle->invert_matrix);
   free(handle->g_tbls);

   if (timer_flags & SF_HANDLE) {
      fast_timer_stop(&handle->handle_timer);
      show_handle_stats(handle);
   }
   free(handle->path);
   free(handle);

   return stat;

}

ne_stat ne_status(char *path) {

   // this is safe for builds with/without sockets and/or socket-authentication enabled
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return NULL;
   }

   return ne_status1(ne_default_snprintf, NULL, UDAL_POSIX, auth, 0, path);
}




// ---------------------------------------------------------------------------
// per-block functions
//
// These functions operate on a single block-file.  They are called both
// from (a) administrative applications which run on the server-side, and
// (b) from client-side applications (like the MarFS run-time).  With the
// advent of the uDAL, the second case requires wrapping in
// uDAL-implementation sensitive code.  See comments above
// ne_delete_block() for details.
// ---------------------------------------------------------------------------




int ne_set_xattr1(const uDAL* impl, SktAuth auth,
                  const char *path, const char *xattrval, size_t len) {
   int ret = -1;

   // see comments above OPEN() defn
   GenericFD      fd   = {0};
   struct handle  hndl = {0};
   ne_handle      handle = &hndl;

   handle->impl = impl;
   handle->auth = auth;
   if (! handle->impl) {
      PRINTerr( "ne_set_xattr1: implementation is NULL\n");
      errno = EINVAL;
      return -1;
   }


#ifdef META_FILES
   char meta_file[2048];
   strcpy( meta_file, path );
   strncat( meta_file, META_SFX, strlen(META_SFX) + 1 );

   mode_t mask = umask(0000);
   OPEN( fd, handle, meta_file, O_WRONLY | O_CREAT, 0666 );
   umask(mask);

   if ( FD_ERR(fd) < 0 ) { 
      PRINTerr( "ne_close: failed to open file %s\n", meta_file);
      ret = -1;
   }
   else {
      // int val = HNDLOP( write, fd, xattrval, strlen(xattrval) + 1 );
      int val = write_all(&fd, xattrval, strlen(xattrval) + 1 );
      if ( val != strlen(xattrval) + 1 ) {
         PRINTerr( "ne_close: failed to write to file %s\n", meta_file);
         ret = -1;
         HNDLOP(close, fd);
      }
      else {
         ret = HNDLOP(close, fd);
      }
   }

   // PATHOP(chown, handle->impl, handle->auth, meta_file, handle->status->owner, handle->status->group);

#else
   // looks like the stuff below might conceivably work with threads.
   // The problem is that fgetxattr/fsetxattr are not yet implemented.
#   error "xattr metadata is not functional with new thread model"

   OPEN( fd, handle, path, O_RDONLY );
   if ( FD_ERR(fd) ) { 
      PRINTerr("ne_set_xattr: failed to open file %s\n", path);
      ret = -1;
   }
   else {

#   if (AXATTR_SET_FUNC == 5) // XXX: not functional with threads!!!
      ret = HNDLOP(fsetxattr, fd, XATTRKEY, xattrval, strlen(xattrval), 0);
#   else
      ret = HNDLOP(fsetxattr, fd, XATTRKEY, xattrval, strlen(xattrval), 0, 0);
#   endif
   }

   if (HNDLOP(close, fd) < 0) {
      PRINTerr("ne_set_xattr: failed to close file %s\n", path);
      ret = -1;
   }
#endif //META_FILES

   return ret;
}

int ne_set_xattr( const char *path, const char *xattrval, size_t len) {

   // this is safe for builds with/without sockets enabled
   // and with/without socket-authentication enabled
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return -1;
   }

   return ne_set_xattr1(get_impl(UDAL_POSIX), auth, path, xattrval, len);
}



int ne_get_xattr1( const uDAL* impl, SktAuth auth,
                   const char *path, char *xattrval, size_t len) {
   int ret = 0;

   // see comments above OPEN() defn
   GenericFD      fd   = {0};
   struct handle  hndl = {0};
   ne_handle      handle = &hndl;

   handle->impl = impl;
   handle->auth = auth;
   if (! handle->impl) {
      PRINTerr( "ne_get_xattr1: implementation is NULL\n" );
      errno = EINVAL;
      return -1;
   }


#ifdef META_FILES
   char meta_file_path[2048];
   strncpy(meta_file_path, path, 2048);
   strncat(meta_file_path, META_SFX, strlen(META_SFX)+1);

   OPEN( fd, handle, meta_file_path, O_RDONLY );
   if (FD_ERR(fd)) {
      ret = -1;
      PRINTerr("ne_get_xattr: failed to open file %s\n", meta_file_path);
   }
   else {
      // ssize_t size = HNDLOP( read, fd, xattrval, len );
      ssize_t size = read_all(&fd, xattrval, len);
      if ( size < 0 ) {
         PRINTerr("ne_get_xattr: failed to read from file %s\n", meta_file_path);
         ret = -1;
      }
      else if(size == 0) {
         PRINTerr( "ne_get_xattr: read 0 bytes from metadata file %s\n", meta_file_path);
         ret = -1;
      }
      else if (size == len) {
         // This might mean that the read truncated results to fit into our buffer.
         // Caller should give us a buffer that has more-than-enough room.
         PRINTerr( "ne_get_xattr: read %d bytes from metadata file %s\n", size, meta_file_path);
         ret = -1;
      }

      if (HNDLOP(close, fd) < 0) {
         PRINTerr("ne_get_xattr: failed to close file %s\n", meta_file_path);
         ret = -1;
      }

      ret = size;
   }

#else
   // looks like the stuff below might conceivably work with threads.
   // The problem is that fgetxattr/fsetxattr are not yet implemented.
#   error "xattr metadata is not functional with new thread model"

   OPEN( fd, handle, path, O_RDONLY );
   if ( FD_ERR(fd) ) { 
      PRINTerr("ne_get_xattr: failed to open file %s\n", path);
      ret = -1;
   }
   else {

#   if (AXATTR_GET_FUNC == 4)
      ret = HNDLOP(fgetxattr, fd, XATTRKEY, &xattrval[0], len);
#   else
      ret = HNDLOP(fgetxattr, fd, XATTRKEY, &xattrval[0], len, 0, 0);
#   endif
   }

   if (HNDLOP(close, fd) < 0) {
      PRINTerr("ne_get_xattr: failed to close file %s\n", meta_file_path);
      ret = -1;
   }
#endif

   return ret;
}

int ne_get_xattr( const char *path, char *xattrval, size_t len) {

   // this is safe for builds with/without sockets enabled
   // and with/without socket-authentication enabled
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return -1;
   }

   return ne_get_xattr1(get_impl(UDAL_POSIX), auth, path, xattrval, len);
}

static int set_block_xattr(ne_handle handle, int block) {
  int tmp = 0;
  char xattrval[1024];
  sprintf(xattrval,"%d %d %d %d %lu %lu %llu %llu",
          handle->status->N, handle->status->E, handle->status->O,
          handle->status->bsz, handle->nsz[block],
          handle->ncompsz[block], (unsigned long long)handle->csum[block],
          (unsigned long long)handle->status->totsz);

  PRINTdbg( "ne_close: setting file %d xattr = \"%s\"\n",
               block, xattrval );

  char block_file_path[2048];
  handle->snprintf(block_file_path, MAXNAME, handle->path,
                   (block+handle->status->O)%(handle->status->N+handle->status->E),
                   handle->state);

   if ( handle->mode == NE_REBUILD )
      strncat( block_file_path, REBUILD_SFX, strlen(REBUILD_SFX)+1 );
   else if ( handle->mode == NE_WRONLY )
      strncat( block_file_path, WRITE_SFX, strlen(WRITE_SFX)+1 );
   
   return ne_set_xattr1(handle->impl, handle->auth, block_file_path, xattrval, strlen(xattrval));
}


// unlink a single block (including the manifest file, if
// META_FILES is defined).  This is called from:
//
// (a) a commented-out function in the mc_ring.c MarFS utility ('ch'
//     branch), where I think it would represent a fully-specified
//     block-file on the server-side.  From the server-side, UDAL_POSIX
//     will always work with such paths.
//
// (b) ne_delete1(), where it refers to a fully-specified block-file, but
//     from the client-side.  Therefore, it may potentially need to go
//     through a MarFS RDMA server, so it must acquire uDAL dressing, to
//     allow selection of the appropriate uDAL implementation.


int ne_delete_block1(const uDAL* impl, SktAuth auth, const char *path) {

   int ret = PATHOP(unlink, impl, auth, path);

#ifdef META_FILES
   if(ret == 0) {
      char meta_path[2048];
      strncpy(meta_path, path, 2048);
      strcat(meta_path, META_SFX);

      ret = PATHOP(unlink, impl, auth, meta_path);
   }
#endif

   return ret;
}

int ne_delete_block(const char *path) {

   // this is safe for builds with/without sockets enabled
   // and with/without socket-authentication enabled
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return -1;
   }

   return ne_delete_block1(get_impl(UDAL_POSIX), auth, path);
}




/**
 * Make a symlink to an existing block.
 */
int ne_link_block1(const uDAL* impl, SktAuth auth,
                   const char *link_path, const char *target) {

   struct stat target_stat;
   int         ret;


#ifdef META_FILES
   char meta_path[2048];
   char meta_path_target[2048];

   strcpy(meta_path, link_path);
   strcat(meta_path, META_SFX);

   strcpy(meta_path_target, target);
   strcat(meta_path_target, META_SFX);
#endif // META_FILES

   // stat the target.
   if (PATHOP(stat, impl, auth, target, &target_stat) == -1) {
      return -1;
   }

   // if it is a symlink, then move it,
   if(S_ISLNK(target_stat.st_mode)) {
      // check that the meta file has a symlink here too, If not then
      // abort without doing anything. If it does, then proceed with
      // making symlinks.
      if(PATHOP(stat, impl, auth, meta_path_target, &target_stat) == -1) {
         return -1;
      }
      if(!S_ISLNK(target_stat.st_mode)) {
         return -1;
      }
      char   tp[2048];
      char   tp_meta[2048];
      size_t link_size;
      if((link_size = PATHOP(readlink, impl, auth, target, tp, 2048)) != -1) {
         tp[link_size] = '\0';
      }
      else {
         return -1;
      }
#ifdef META_FILES
      if((link_size = PATHOP(readlink, impl, auth, meta_path_target, tp_meta, 2048)) != -1) {
         tp_meta[link_size] = '\0';
      }
      else {
         return -1;
      }
#endif

      // make the new links.
      ret = PATHOP(symlink, impl, auth, tp, link_path);
#ifdef META_FILES
      if(ret == 0)
         ret = PATHOP(symlink, impl, auth, tp_meta, meta_path);
#endif

      // remove the old links.
      ret = PATHOP(unlink, impl, auth, target);
#ifdef META_FILES
      if(ret == 0)
         PATHOP(unlink, impl, auth, meta_path_target);
#endif
      return ret;
   }

   // if not, then create the link.
   ret = PATHOP(symlink, impl, auth, target, link_path);
#ifdef META_FILES
   if(ret == 0)
      ret = PATHOP(symlink, impl, auth, meta_path_target, meta_path);
#endif
   return ret;
}


int ne_link_block(const char *link_path, const char *target) {

   // this is safe for builds with/without sockets enabled
   // and with/without socket-authentication enabled
   // However, if you do build with socket-authentication, this will require a read
   // from a file (~/.awsAuth) that should probably only be accessible if ~ is /root.
   SktAuth  auth;
   if (DEFAULT_AUTH_INIT(auth)) {
      PRINTerr("failed to initialize default socket-authentication credentials\n");
      return -1;
   }

   return ne_link_block1(get_impl(UDAL_POSIX), auth, link_path, target);
}
