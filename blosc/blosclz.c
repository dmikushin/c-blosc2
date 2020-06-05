/*********************************************************************
  Blosc - Blocked Shuffling and Compression Library

  Author: Francesc Alted <francesc@blosc.org>
  Creation date: 2009-05-20

  See LICENSE.txt for details about copyright and rights to use.
**********************************************************************/

/*********************************************************************
  The code in this file is heavily based on FastLZ, a lightning-fast
  lossless compression library.  See LICENSES/FASTLZ.txt for details.
**********************************************************************/


#include <stdio.h>
#include "blosclz.h"
#include "fastcopy.h"
#include "blosc2-common.h"


/*
 * Give hints to the compiler for branch prediction optimization.
 */
#if defined(__GNUC__) && (__GNUC__ > 2)
#define BLOSCLZ_EXPECT_CONDITIONAL(c)    (__builtin_expect((c), 1))
#define BLOSCLZ_UNEXPECT_CONDITIONAL(c)  (__builtin_expect((c), 0))
#else
#define BLOSCLZ_EXPECT_CONDITIONAL(c)    (c)
#define BLOSCLZ_UNEXPECT_CONDITIONAL(c)  (c)
#endif

/*
 * Use inlined functions for supported systems.
 */
#if defined(_MSC_VER) && !defined(__cplusplus)   /* Visual Studio */
#define inline __inline  /* Visual C is not C99, but supports some kind of inline */
#endif

#define MAX_COPY 32U
#define MAX_DISTANCE 8191
#define MAX_FARDISTANCE (65535 + MAX_DISTANCE - 1)

#ifdef BLOSC_STRICT_ALIGN
  #define BLOSCLZ_READU16(p) ((p)[0] | (p)[1]<<8)
  #define BLOSCLZ_READU32(p) ((p)[0] | (p)[1]<<8 | (p)[2]<<16 | (p)[3]<<24)
#else
  #define BLOSCLZ_READU16(p) *((const uint16_t*)(p))
  #define BLOSCLZ_READU32(p) *((const uint32_t*)(p))
#endif

#define HASH_LOG (12)

// This is used in LZ4 and seems to work pretty well here too
#define HASH_FUNCTION(v, s, h) {                          \
  v = (s * 2654435761U) >> (32U - h);  \
}


#define LITERAL(ip, op, op_limit, anchor, copy) {        \
  if (BLOSCLZ_UNEXPECT_CONDITIONAL(op + 2 > op_limit))   \
    goto out;                                            \
  *op++ = *anchor++;                                     \
  ip = anchor;                                           \
  copy++;                                                \
  if (BLOSCLZ_UNEXPECT_CONDITIONAL(copy == MAX_COPY)) {  \
    copy = 0;                                            \
    *op++ = MAX_COPY-1;                                  \
  }                                                      \
}

#define IP_BOUNDARY 2
#define BYTES_IN_CYCLE 512

#if defined(__AVX2__)
static uint8_t *get_run_32(uint8_t *ip, const uint8_t *ip_bound, const uint8_t *ref) {
    uint8_t x = ip[-1];
    /* safe because the outer check against ip limit */
    if (ip < (ip_bound - sizeof(int64_t))) {
        int64_t value, value2;
        /* Broadcast the value for every byte in a 64-bit register */
        memset(&value, x, 8);
#if defined(BLOSC_STRICT_ALIGN)
        memcpy(&value2, ref, 8);
#else
        value2 = ((int64_t*)ref)[0];
#endif
        if (value != value2) {
            /* Return the byte that starts to differ */
            while (*ref++ == x) ip++;
            return ip;
        }
        else {
            ip += 8;
            ref += 8;
        }
    }
    if (ip < (ip_bound - sizeof(__m128i))) {
        __m128i value, value2, cmp;
        /* Broadcast the value for every byte in a 128-bit register */
        memset(&value, x, sizeof(__m128i));
        value2 = _mm_loadu_si128((__m128i *) ref);
        cmp = _mm_cmpeq_epi32(value, value2);
        if (_mm_movemask_epi8(cmp) != 0xFFFF) {
            /* Return the byte that starts to differ */
            while (*ref++ == x) ip++;
            return ip;
        } else {
            ip += sizeof(__m128i);
            ref += sizeof(__m128i);
        }
    }
    while (ip < (ip_bound - (sizeof(__m256i)))) {
        __m256i value, value2, cmp;
        /* Broadcast the value for every byte in a 256-bit register */
        memset(&value, x, sizeof(__m256i));
        value2 = _mm256_loadu_si256((__m256i *)ref);
        cmp = _mm256_cmpeq_epi64(value, value2);
        if ((unsigned)_mm256_movemask_epi8(cmp) != 0xFFFFFFFF) {
            /* Return the byte that starts to differ */
            while (*ref++ == x) ip++;
            return ip;
        }
        else {
            ip += sizeof(__m256i);
            ref += sizeof(__m256i);
        }
    }
    /* Look into the remainder */
    while ((ip < ip_bound) && (*ref++ == x)) ip++;
    return ip;
}

#elif defined(__SSE2__)

static uint8_t *get_run_16(uint8_t *ip, const uint8_t *ip_bound, const uint8_t *ref) {
  uint8_t x = ip[-1];

  if (ip < (ip_bound - sizeof(int64_t))) {
    int64_t value, value2;
    /* Broadcast the value for every byte in a 64-bit register */
    memset(&value, x, 8);
#if defined(BLOSC_STRICT_ALIGN)
    memcpy(&value2, ref, 8);
#else
    value2 = ((int64_t*)ref)[0];
#endif
    if (value != value2) {
      /* Return the byte that starts to differ */
      while (*ref++ == x) ip++;
      return ip;
    }
    else {
      ip += 8;
      ref += 8;
    }
  }
  /* safe because the outer check against ip limit */
  while (ip < (ip_bound - sizeof(__m128i))) {
    __m128i value, value2, cmp;
    /* Broadcast the value for every byte in a 128-bit register */
    memset(&value, x, sizeof(__m128i));
    value2 = _mm_loadu_si128((__m128i *)ref);
    cmp = _mm_cmpeq_epi32(value, value2);
    if (_mm_movemask_epi8(cmp) != 0xFFFF) {
      /* Return the byte that starts to differ */
      while (*ref++ == x) ip++;
      return ip;
    }
    else {
      ip += sizeof(__m128i);
      ref += sizeof(__m128i);
    }
  }
  /* Look into the remainder */
  while ((ip < ip_bound) && (*ref++ == x)) ip++;
  return ip;
}

#else
static uint8_t *get_run(uint8_t *ip, const uint8_t *ip_bound, const uint8_t *ref) {
  uint8_t x = ip[-1];
  int64_t value, value2;
  /* Broadcast the value for every byte in a 64-bit register */
  memset(&value, x, 8);
  /* safe because the outer check against ip limit */
  while (ip < (ip_bound - sizeof(int64_t))) {
#if defined(BLOSC_STRICT_ALIGN)
    memcpy(&value2, ref, 8);
#else
    value2 = ((int64_t*)ref)[0];
#endif
    if (value != value2) {
      /* Return the byte that starts to differ */
      while (*ref++ == x) ip++;
      return ip;
    }
    else {
      ip += 8;
      ref += 8;
    }
  }
  /* Look into the remainder */
  while ((ip < ip_bound) && (*ref++ == x)) ip++;
  return ip;
}
#endif


/* Return the byte that starts to differ */
static uint8_t *get_match(uint8_t *ip, const uint8_t *ip_bound, const uint8_t *ref) {
#if !defined(BLOSC_STRICT_ALIGN)
  while (ip < (ip_bound - sizeof(int64_t))) {
    if (*(int64_t*)ref != *(int64_t*)ip) {
      /* Return the byte that starts to differ */
      while (*ref++ == *ip++) {}
      return ip;
    }
    else {
      ip += sizeof(int64_t);
      ref += sizeof(int64_t);
    }
  }
#endif
  /* Look into the remainder */
  while ((ip < ip_bound) && (*ref++ == *ip++)) {}
  return ip;
}


#if defined(__SSE2__)
static uint8_t *get_match_16(uint8_t *ip, const uint8_t *ip_bound, const uint8_t *ref) {
  __m128i value, value2, cmp;

  if (ip < (ip_bound - sizeof(int64_t))) {
    if (*(int64_t *) ref != *(int64_t *) ip) {
      /* Return the byte that starts to differ */
      while (*ref++ == *ip++) {}
      return ip;
    } else {
      ip += sizeof(int64_t);
      ref += sizeof(int64_t);
    }
  }
  while (ip < (ip_bound - sizeof(__m128i))) {
    value = _mm_loadu_si128((__m128i *) ip);
    value2 = _mm_loadu_si128((__m128i *) ref);
    cmp = _mm_cmpeq_epi32(value, value2);
    if (_mm_movemask_epi8(cmp) != 0xFFFF) {
      /* Return the byte that starts to differ */
      return get_match(ip, ip_bound, ref);
    }
    else {
      ip += sizeof(__m128i);
      ref += sizeof(__m128i);
    }
  }
  /* Look into the remainder */
  while ((ip < ip_bound) && (*ref++ == *ip++)) {}
  return ip;
}
#endif


#if defined(__AVX2__)
static uint8_t *get_match_32(uint8_t *ip, const uint8_t *ip_bound, const uint8_t *ref) {

  if (ip < (ip_bound - sizeof(int64_t))) {
    if (*(int64_t *) ref != *(int64_t *) ip) {
      /* Return the byte that starts to differ */
      while (*ref++ == *ip++) {}
      return ip;
    } else {
      ip += sizeof(int64_t);
      ref += sizeof(int64_t);
    }
  }
  if (ip < (ip_bound - sizeof(__m128i))) {
    __m128i value, value2, cmp;
    value = _mm_loadu_si128((__m128i *) ip);
    value2 = _mm_loadu_si128((__m128i *) ref);
    cmp = _mm_cmpeq_epi32(value, value2);
    if (_mm_movemask_epi8(cmp) != 0xFFFF) {
      /* Return the byte that starts to differ */
      return get_match_16(ip, ip_bound, ref);
    }
    else {
      ip += sizeof(__m128i);
      ref += sizeof(__m128i);
    }
  }
  while (ip < (ip_bound - sizeof(__m256i))) {
    __m256i value, value2, cmp;
    value = _mm256_loadu_si256((__m256i *) ip);
    value2 = _mm256_loadu_si256((__m256i *)ref);
    cmp = _mm256_cmpeq_epi64(value, value2);
    if ((unsigned)_mm256_movemask_epi8(cmp) != 0xFFFFFFFF) {
      /* Return the byte that starts to differ */
      while (*ref++ == *ip++) {}
      return ip;
    }
    else {
      ip += sizeof(__m256i);
      ref += sizeof(__m256i);
    }
  }
  /* Look into the remainder */
  while ((ip < ip_bound) && (*ref++ == *ip++)) {}
  return ip;
}
#endif


int blosclz_compress(const int clevel, const void* input, int length,
                     void* output, int maxout) {
  uint8_t* ibase = (uint8_t*)input;
  uint8_t* ip = ibase;
  uint8_t* icycle = ibase;
  uint8_t* ip_bound = ibase + length - IP_BOUNDARY;
  uint8_t* ip_limit = ibase + length - 12;
  uint8_t* op = (uint8_t*)output;
  uint8_t* ocycle = op;
  uint8_t* op_limit;
  uint32_t htab[1U << (uint8_t)HASH_LOG];
  uint32_t hval;
  uint32_t seq;
  uint8_t copy;
  long skip_cycle = 0;
  double cratio;

  // Minimum cratios before issuing and _early giveup_
  // Remind that blosclz is not meant for cratios <= 2 (too costly to decompress)
  double maxlength_[10] = {-1, .07, .1, .2, .4, .5, .5, .5, .5, .6};
  int32_t maxlength = (int32_t)(length * maxlength_[clevel]);
  if (maxlength > (int32_t)maxout) {
    maxlength = (int32_t)maxout;
  }
  op_limit = op + maxlength;

  // The maximum amount of cycles to skip match lookups
  // A 0 means just _early giveup_ whereas > 0 use _entropy sensing_ too
  long max_skip_cycles_[10] = {255, 0, 0, 0, 3, 2, 2, 1, 1, 0};
  long max_skip_cycles = max_skip_cycles_[clevel];
  // The minimum compression ratio before skipping a number of cycles
  double min_cratio_[10] = {-1, 0., 0., 0., 5., 4., 4., 3., 2., 1.};
  double min_cratio = min_cratio_[clevel];

  uint8_t hashlog_[10] = {0, HASH_LOG - 2, HASH_LOG - 1, HASH_LOG, HASH_LOG,
                           HASH_LOG, HASH_LOG, HASH_LOG, HASH_LOG, HASH_LOG};
  uint8_t hashlog = hashlog_[clevel];
  // Initialize the hash table to distances of 0
  for (unsigned i = 0; i < (1U << hashlog); i++) {
    htab[i] = 0;
  }

  /* input and output buffer cannot be less than 16 and 66 bytes or we can get into trouble */
  if (length < 16 || maxout < 66) {
    return 0;
  }

  /* we start with literal copy */
  copy = 2;
  *op++ = MAX_COPY - 1;
  *op++ = *ip++;
  *op++ = *ip++;

  /* main loop */
  while (BLOSCLZ_EXPECT_CONDITIONAL(ip < ip_limit)) {
    const uint8_t* ref;
    uint32_t distance;
    uint32_t len = 4;         /* minimum match length */
    uint8_t* anchor = ip;    /* comparison starting-point */

    if (BLOSCLZ_EXPECT_CONDITIONAL(max_skip_cycles)) {
      // Enter the entropy probing mode
      if (skip_cycle) {
        LITERAL(ip, op, op_limit, anchor, copy)
        // Start a new cycle every 256 bytes
        if (BLOSCLZ_UNEXPECT_CONDITIONAL(ip - icycle) >= BYTES_IN_CYCLE) {
          skip_cycle--;
          icycle = ip;
          ocycle = op;
        }
        continue;
      }
      // Check whether we are doing well with compression ratios
      if (BLOSCLZ_UNEXPECT_CONDITIONAL((op - ocycle) >= BYTES_IN_CYCLE)) {
        cratio = (double) (ip - icycle) / (double) (op - ocycle);
        if (cratio < min_cratio) {
          skip_cycle = max_skip_cycles;
          icycle = ip;
          ocycle = op;
          continue;
        }
      }
    }

    /* find potential match */
    seq = BLOSCLZ_READU32(ip);
    HASH_FUNCTION(hval, seq, hashlog)
    ref = ibase + htab[hval];

    /* calculate distance to the match */
    distance = (int32_t)(anchor - ref);

    /* update hash table */
    htab[hval] = (uint32_t) (anchor - ibase);

    if (distance == 0 || (distance >= MAX_FARDISTANCE)) {
      LITERAL(ip, op, op_limit, anchor, copy)
      continue;
    }

    /* is this a match? check the first 4 bytes */
    if (BLOSCLZ_UNEXPECT_CONDITIONAL(BLOSCLZ_READU32(ref) == BLOSCLZ_READU32(ip))) {
      ref += 4;
    }
    else {
      /* no luck, copy as a literal */
      LITERAL(ip, op, op_limit, anchor, copy)
      continue;
    }

    /* last matched byte */
    ip = anchor + len;

    /* distance is biased */
    distance--;

    if (BLOSCLZ_UNEXPECT_CONDITIONAL(!distance)) {
      /* zero distance means a run */
#if defined(__AVX2__)
      ip = get_run_32(ip, ip_bound, ref);
#elif defined(__SSE2__)
      ip = get_run_16(ip, ip_bound, ref);
#else
      ip = get_run(ip, ip_bound, ref);
#endif
    }
    else {
#if defined(__AVX2__)
      ip = get_match_32(ip, ip_bound + IP_BOUNDARY, ref);
#elif defined(__SSE2__)
      ip = get_match_16(ip, ip_bound + IP_BOUNDARY, ref);
#else
      ip = get_match(ip, ip_bound + IP_BOUNDARY, ref);
#endif
    }

    /* if we have copied something, adjust the copy count */
    if (copy)
      /* copy is biased, '0' means 1 byte copy */
      *(op - copy - 1) = (uint8_t)(copy - 1);
    else
      /* back, to overwrite the copy count */
      op--;

    /* reset literal counter */
    copy = 0;

    /* length is biased, '1' means a match of 3 bytes */
    ip -= 3;
    len = (int32_t)(ip - anchor);

    /* encode the match */
    if (distance < MAX_DISTANCE) {
      if (len < 7) {
        *op++ = (uint8_t)((len << 5U) + (distance >> 8U));
        *op++ = (uint8_t)((distance & 255U));
      }
      else {
        *op++ = (uint8_t)((7U << 5U) + (distance >> 8U));
        for (len -= 7; len >= 255; len -= 255)
          *op++ = 255;
        *op++ = (uint8_t)len;
        *op++ = (uint8_t)((distance & 255U));
      }
    }
    else {
      /* far away, but not yet in the another galaxy... */
      if (len < 7) {
        distance -= MAX_DISTANCE;
        *op++ = (uint8_t)((len << 5U) + 31);
        *op++ = 255;
        *op++ = (uint8_t)(distance >> 8U);
        *op++ = (uint8_t)(distance & 255U);
      }
      else {
        distance -= MAX_DISTANCE;
        *op++ = (7U << 5U) + 31;
        for (len -= 7; len >= 255; len -= 255)
          *op++ = 255;
        *op++ = (uint8_t)len;
        *op++ = 255;
        *op++ = (uint8_t)(distance >> 8U);
        *op++ = (uint8_t)(distance & 255U);
      }
    }

    /* update the hash at match boundary */
    seq = BLOSCLZ_READU32(ip);
    HASH_FUNCTION(hval, seq, hashlog)
    htab[hval] = (uint32_t) (ip++ - ibase);
    seq >>= 8U;
    HASH_FUNCTION(hval, seq, hashlog)
    htab[hval] = (uint32_t) (ip++ - ibase);
    /* assuming literal copy */
    *op++ = MAX_COPY - 1;

  }

  /* left-over as literal copy */
  ip_bound++;
  while (BLOSCLZ_UNEXPECT_CONDITIONAL(ip <= ip_bound)) {
    if (BLOSCLZ_UNEXPECT_CONDITIONAL(op + 2 > op_limit)) goto out;
    *op++ = *ip++;
    copy++;
    if (BLOSCLZ_UNEXPECT_CONDITIONAL(copy == MAX_COPY)) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* if we have copied something, adjust the copy length */
  if (copy)
    *(op - copy - 1) = (uint8_t)(copy - 1);
  else
    op--;

  /* marker for blosclz */
  *(uint8_t*)output |= (1U << 5U);

  return (int)(op - (uint8_t*)output);

  out:
  return 0;

}

// See https://habr.com/en/company/yandex/blog/457612/
#ifdef __AVX2__

#if defined(_MSC_VER)
#define ALIGNED_(x) __declspec(align(x))
#else
#if defined(__GNUC__)
#define ALIGNED_(x) __attribute__ ((aligned(x)))
#endif
#endif
#define ALIGNED_TYPE_(t, x) t ALIGNED_(x)

static unsigned char* copy_match_16(unsigned char *op, const unsigned char *match, int32_t len)
{
  size_t offset = op - match;
  while (len >= 16) {

    static const ALIGNED_TYPE_(uint8_t, 16) masks[] =
      {
                0,  1,  2,  1,  4,  1,  4,  2,  8,  7,  6,  5,  4,  3,  2,  1, // offset = 0, not used as mask, but for shift
                0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, // offset = 1
                0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,
                0,  1,  2,  0,  1,  2,  0,  1,  2,  0,  1,  2,  0,  1,  2,  0,
                0,  1,  2,  3,  0,  1,  2,  3,  0,  1,  2,  3,  0,  1,  2,  3,
                0,  1,  2,  3,  4,  0,  1,  2,  3,  4,  0,  1,  2,  3,  4,  0,
                0,  1,  2,  3,  4,  5,  0,  1,  2,  3,  4,  5,  0,  1,  2,  3,
                0,  1,  2,  3,  4,  5,  6,  0,  1,  2,  3,  4,  5,  6,  0,  1,
                0,  1,  2,  3,  4,  5,  6,  7,  0,  1,  2,  3,  4,  5,  6,  7,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  0,  1,  2,  3,  4,  5,  6,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0,  1,  2,  3,  4,  5,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,  0,  1,  2,  3,  4,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,  0,  1,  2,  3,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12,  0,  1,  2,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,  0,  1,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,  0,
                0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,  15, // offset = 16
      };

    _mm_storeu_si128((__m128i *)(op),
                     _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(match)),
                                      _mm_load_si128((const __m128i *)(masks) + offset)));

    match += masks[offset];

    op += 16;
    len -= 16;
  }
  // Deal with remainders
  for (; len > 0; len--) {
    *op++ = *match++;
  }
  return op;
}
#endif


int blosclz_decompress(const void* input, int length, void* output, int maxout) {
  const uint8_t* ip = (const uint8_t*)input;
  const uint8_t* ip_limit = ip + length;
  uint8_t* op = (uint8_t*)output;
  uint32_t ctrl;
  uint8_t* op_limit = op + maxout;
  if (BLOSCLZ_UNEXPECT_CONDITIONAL(length == 0)) {
    return 0;
  }
  ctrl = (*ip++) & 31U;

  while (1) {
    uint8_t* ref = op;
    int32_t len = ctrl >> 5U;
    int32_t ofs = (ctrl & 31U) << 8U;

    if (ctrl >= 32) {
      uint8_t code;
      len--;
      ref -= ofs;
      if (len == 7 - 1) {
        do {
          if (BLOSCLZ_UNEXPECT_CONDITIONAL(ip + 1 >= ip_limit)) {
            return 0;
          }
          code = *ip++;
          len += code;
        } while (code == 255);
      }
      else {
        if (BLOSCLZ_UNEXPECT_CONDITIONAL(ip + 1 >= ip_limit)) {
          return 0;
        }
      }
      code = *ip++;
      ref -= code;

      /* match from 16-bit distance */
      if (BLOSCLZ_UNEXPECT_CONDITIONAL(code == 255)) {
        if (BLOSCLZ_EXPECT_CONDITIONAL(ofs == (31U << 8U))) {
          if (BLOSCLZ_UNEXPECT_CONDITIONAL(ip + 1 >= ip_limit)) {
            return 0;
          }
          ofs = (*ip++) << 8U;
          ofs += *ip++;
          ref = op - ofs - MAX_DISTANCE;
        }
      }

      if (BLOSCLZ_UNEXPECT_CONDITIONAL(op + len + 3 > op_limit)) {
        return 0;
      }

      if (BLOSCLZ_UNEXPECT_CONDITIONAL(ref - 1 < (uint8_t*)output)) {
        return 0;
      }

      if (BLOSCLZ_EXPECT_CONDITIONAL(ip < ip_limit))
        ctrl = *ip++;
      else
        break;

      if (ref == op) {
        /* optimized copy for a run */
        uint8_t b = ref[-1];
        memset(op, b, len + 3);
        op += len + 3;
      }
      else {
        /* copy from reference */
        ref--;
        len += 3;
#ifdef __AVX2__
        if (op - ref <= 16) {
          // This is not faster on a combination of compilers (clang, gcc, icc) or machines, but
          // it is not slower either.  Let's activate here for experimentation.
          op = copy_match_16(op, ref, len);
        }
        else {
#endif
          // We absolutely need a copy_match here
          op = copy_match(op, ref, (unsigned) len);
#ifdef __AVX2__
        }
#endif
      }
    }
    else {
      ctrl++;
      if (BLOSCLZ_UNEXPECT_CONDITIONAL(op + ctrl > op_limit)) {
        return 0;
      }
      if (BLOSCLZ_UNEXPECT_CONDITIONAL(ip + ctrl > ip_limit)) {
        return 0;
      }

      // memcpy(op, ip, ctrl); op += ctrl; ip += ctrl;
      // On GCC-6, fastcopy this is still faster than plain memcpy
      // However, using recent CLANG/LLVM 9.0, there is almost no difference
      // in performance.
      op = fastcopy(op, ip, (unsigned) ctrl); ip += ctrl;

      if (BLOSCLZ_UNEXPECT_CONDITIONAL(ip >= ip_limit)) break;
      ctrl = *ip++;
    }
  }

  return (int)(op - (uint8_t*)output);
}
