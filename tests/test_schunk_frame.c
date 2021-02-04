/*
  Copyright (C) 2020 The Blosc Developers
  http://blosc.org
  License: BSD (see LICENSE.txt)

  Creation date: 2020-10-22

  See LICENSE.txt for details about copyright and rights to use.
*/

#include <stdio.h>
#include "test_common.h"

#define CHUNKSIZE (200 * 1000)
#define NTHREADS (2)

/* Global vars */
int tests_run = 0;
int nchunks;
int sequential = false;


static char* test_schunk_framebuf(void) {
  int32_t isize = CHUNKSIZE * sizeof(int32_t);
  int32_t *data = malloc(isize);
  int32_t *data_dest = malloc(isize);
  int dsize;
  blosc2_schunk* schunk;

  /* Initialize the Blosc compressor */
  blosc_init();

  /* Create a super-chunk container */
  blosc2_storage storage = {.sequential=sequential};
  schunk = blosc2_schunk_new(storage);

  // Feed it with data
  for (int nchunk = 0; nchunk < nchunks; nchunk++) {
    for (int i = 0; i < CHUNKSIZE; i++) {
      data[i] = i + nchunk * CHUNKSIZE;
    }
    int nchunks_ = blosc2_schunk_append_buffer(schunk, data, isize);
    mu_assert("ERROR: bad append in frame", nchunks_ > 0);
  }

  // Get a memory frame out of the schunk
  uint8_t* framebuf;
  bool framebuf_needs_free;
  int64_t len = blosc2_schunk_to_buffer(schunk, &framebuf, &framebuf_needs_free);
  mu_assert("Error in getting a frame buffer", len > 0);

  // ...and another schunk backed by the frame buffer
  blosc2_schunk* schunk2 = blosc2_schunk_from_buffer(framebuf, len, false);

  // Now store frame in a file
  len = blosc2_schunk_to_file(schunk2, "test_file.b2frame");
  mu_assert("Error in storing a frame buffer", len > 0);

  // Free completely all the schunks
  blosc2_schunk_free(schunk);
  blosc2_schunk_free(schunk2);

  // ...and open a new one back
  schunk = blosc2_schunk_open("test_file.b2frame");

  // Check that the chunks have been decompressed correctly
  for (int nchunk = 0; nchunk < nchunks; nchunk++) {
    dsize = blosc2_schunk_decompress_chunk(schunk, nchunk, (void *) data_dest, isize);
    mu_assert("ERROR: chunk cannot be decompressed correctly.", dsize >= 0);
    for (int i = 0; i < CHUNKSIZE; i++) {
      mu_assert("ERROR: bad roundtrip",data_dest[i] == i + nchunk * CHUNKSIZE);
    }
  }

  /* Free resources */
  free(data);
  free(data_dest);
  blosc2_schunk_free(schunk);
  if (framebuf_needs_free) {
    free(framebuf);
  }
  /* Destroy the Blosc environment */
  blosc_destroy();

  return EXIT_SUCCESS;
}

static char *all_tests(void) {
  nchunks = 0;
  sequential = true;
  mu_run_test(test_schunk_framebuf);

  nchunks = 0;
  sequential = false;
  mu_run_test(test_schunk_framebuf);

  nchunks = 1;
  sequential = false;
  mu_run_test(test_schunk_framebuf);

  nchunks = 10;
  sequential = true;
  mu_run_test(test_schunk_framebuf);

  return EXIT_SUCCESS;
}


int main(void) {
  char *result;

  install_blosc_callback_test(); /* optionally install callback test */
  blosc_init();

  /* Run all the suite */
  result = all_tests();
  if (result != EXIT_SUCCESS) {
    printf(" (%s)\n", result);
  }
  else {
    printf(" ALL TESTS PASSED");
  }
  printf("\tTests run: %d\n", tests_run);

  blosc_destroy();

  return result != EXIT_SUCCESS;
}
