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


#define DEBUG 1
#define USE_STDOUT 1
#include "io/io.h"
#include "dal/dal.h"
#include <unistd.h>
#include <stdio.h>


int main( int argc, char** argv ) {
   // create a meta info struct
   meta_info minfo_ref;
   minfo_ref.N = 10;
   minfo_ref.E = 2;
   minfo_ref.O = 5;
   minfo_ref.partsz = 4096;
   minfo_ref.versz = 1048580;
   minfo_ref.blocksz = 104858000;
   minfo_ref.crcsum = 123456789;
   minfo_ref.totsz = 1048576000;

   meta_info minfo_fill;

   xmlDoc *doc = NULL;
   xmlNode *root_element = NULL;


   /*
   * this initialize the library and check potential ABI mismatches
   * between the version it was compiled for and the actual shared
   * library used.
   */
   LIBXML_TEST_VERSION

   /*parse the file and get the DOM */
   doc = xmlReadFile("./testing/config.xml", NULL, XML_PARSE_NOBLANKS);

   if (doc == NULL) {
     printf("error: could not parse file %s\n", "./dal/testing/config.xml");
     return -1;
   }

   /*Get the root element node */
   root_element = xmlDocGetRootElement(doc);

   // Initialize a posix dal instance
   DAL_location maxloc = { .pod = 1, .block = 1, .cap = 1, .scatter = 1 };
   DAL dal = init_dal( root_element, maxloc );

   /* Free the xml Doc */
   xmlFreeDoc(doc);
   /*
   *Free the global variables that may
   *have been allocated by the parser.
   */
   xmlCleanupParser();

   // check that initialization succeeded
   if ( dal == NULL ) {
      printf( "error: failed to initialize DAL: %s\n", strerror(errno) );
      return -1;
   }

   // get a block context on which to set meta info
   BLOCK_CTXT block = dal->open( dal->ctxt, DAL_WRITE, maxloc, "" );
   if ( block == NULL ) { printf( "error: failed to open block context for write: %s\n", strerror(errno) ); return -1; }

   // attempt to set meta info from our ref struct
   if ( dal_set_minfo( dal, block, &(minfo_ref) ) ) {
      printf( "error: failed to set meta info on block: %s\n", strerror(errno) );
      return -1;
   }

   // close the empty block ref
   if ( dal->close( block ) ) { printf( "error: failed to close block write context: %s\n", strerror(errno) ); return -1; }

   // get a block context on which to get meta info
   block = dal->open( dal->ctxt, DAL_READ, maxloc, "" );
   if ( block == NULL ) { printf( "error: failed to open block context for write: %s\n", strerror(errno) ); return -1; }

   // attempt to retrieve meta info into our fill struct
   if ( dal_get_minfo( dal, block, &(minfo_fill) ) ) {
      printf( "error: failed to get meta info on block: %s\n", strerror(errno) );
      return -1;
   }

   // close the empty block ref
   if ( dal->close( block ) ) { printf( "error: failed to close block read context: %s\n", strerror(errno) ); return -1; }

   // Delete the block we created
   if ( dal->del( dal->ctxt, maxloc, "" ) ) { printf( "warning: del failed!\n" ); }

   // Free the DAL
   if ( dal->cleanup( dal ) ) { printf( "error: failed to cleanup DAL\n" ); return -1; }

   // Finally, compare our structs
   int retval=0;
   if ( minfo_ref.N != minfo_fill.N ) {
      printf( "error: set (%d) and retrieved (%d) meta info 'N' values do not match!\n", minfo_ref.N, minfo_fill.N );
      retval=-1;
   }
   if ( minfo_ref.E != minfo_fill.E ) {
      printf( "error: set (%d) and retrieved (%d) meta info 'E' values do not match!\n", minfo_ref.E, minfo_fill.E );
      retval=-1;
   }
   if ( minfo_ref.O != minfo_fill.O ) {
      printf( "error: set (%d) and retrieved (%d) meta info 'O' values do not match!\n", minfo_ref.O, minfo_fill.O );
      retval=-1;
   }
   if ( minfo_ref.partsz != minfo_fill.partsz ) {
      printf( "error: set (%zd) and retrieved (%zd) meta info 'partsz' values do not match!\n", minfo_ref.partsz, minfo_fill.partsz );
      retval=-1;
   }
   if ( minfo_ref.versz != minfo_fill.versz ) {
      printf( "error: set (%zd) and retrieved (%zd) meta info 'versz' values do not match!\n", minfo_ref.versz, minfo_fill.versz );
      retval=-1;
   }
   if ( minfo_ref.blocksz != minfo_fill.blocksz ) {
      printf( "error: set (%zd) and retrieved (%zd) meta info 'blocksz' values do not match!\n", minfo_ref.blocksz, minfo_fill.blocksz );
      retval=-1;
   }
   if ( minfo_ref.crcsum != minfo_fill.crcsum ) {
      printf( "error: set (%lld) and retrieved (%lld) meta info 'crcsum' values do not match!\n", minfo_ref.crcsum, minfo_fill.crcsum );
      retval=-1;
   }
   if ( minfo_ref.totsz != minfo_fill.totsz ) {
      printf( "error: set (%zd) and retrieved (%zd) meta info 'totsz' values do not match!\n", minfo_ref.totsz, minfo_fill.totsz );
      retval=-1;
   }

   return retval;
}


