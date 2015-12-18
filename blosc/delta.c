/*********************************************************************
  Blosc - Blocked Shuffling and Compression Library

  Author: Francesc Alted <francesc@blosc.org>
  Creation date: 2015-12-18

  See LICENSES/BLOSC.txt for details about copyright and rights to use.
**********************************************************************/


#include "delta.h"

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))


int delta_encoder8(schunk_header* sc_header, int nbytes,
                   unsigned char* src, unsigned char* dest) {
  int i;
  int rbytes = *(int*)(sc_header->filters_chunk + 4);
  int mbytes;
  uint8_t* dref = (uint8_t*)sc_header->filters_chunk + BLOSC_MAX_OVERHEAD;

  mbytes = MIN(nbytes, rbytes);

  /* Encode delta */
  for (i = 0; i < mbytes; i++) {
    dest[i] = src[i] - dref[i];
  }

  /* Copy the leftovers */
  if (nbytes > rbytes) {
    for (i = rbytes; i < nbytes; i++) {
      dest[i] = src[i];
    }
  }

  return nbytes;
}


int delta_decoder8(schunk_header* sc_header, int nbytes, unsigned char* src) {
  int i;
  unsigned char* dref = (uint8_t*)sc_header->filters_chunk + BLOSC_MAX_OVERHEAD;
  int rbytes = *(int*)(sc_header->filters_chunk + 4);
  int mbytes;

  mbytes = MIN(nbytes, rbytes);

  /* Decode delta */
  for (i = 0; i < (mbytes); i++) {
    src[i] += dref[i];
  }

  /* The leftovers are in-place already */

  return nbytes;
}
