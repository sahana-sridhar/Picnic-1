/*
 *  This file is part of the optimized implementation of the Picnic signature scheme.
 *  See the accompanying documentation for complete details.
 *
 *  The code is provided under the MIT license, see LICENSE for
 *  more details.
 *  SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "compat.h"
#include "mzd_additional.h"

#if !defined(_MSC_VER)
#include <stdalign.h>
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER) && !defined(static_assert)
#define static_assert _Static_assert
#endif

static const size_t mzd_local_t_size = (sizeof(mzd_local_t) + 0x1f) & ~0x1f;
static_assert(((sizeof(mzd_local_t) + 0x1f) & ~0x1f) == 32, "sizeof mzd_local_t not supported");

#if defined(WITH_OPT)
#include "simd.h"

#if defined(WITH_POPCNT)
#include <nmmintrin.h>

#if !defined(__x86_64__) && !defined(_M_X64)
ATTR_TARGET("popcnt") ATTR_CONST static inline uint64_t parity64_popcnt(uint64_t in) {
  return (_mm_popcnt_u32(in >> 32) ^ _mm_popcnt_u32(in)) & 0x1;
}
#else
ATTR_TARGET("popcnt") ATTR_CONST static inline uint64_t parity64_popcnt(uint64_t in) {
  return _mm_popcnt_u64(in) & 0x1;
}
#endif
#endif
#endif
static const unsigned int align_bound = 128 / (8 * sizeof(word));

static uint32_t calculate_rowstride(uint32_t width) {
  // As soon as we hit the AVX bound, use 32 byte alignment. Otherwise use 16
  // byte alignment for SSE2 and 128 bit vectors.
  if (width > align_bound) {
    return ((width * sizeof(word) + 31) & ~31) / sizeof(word);
  } else {
    return ((width * sizeof(word) + 15) & ~15) / sizeof(word);
  }
}

static uint32_t calculate_width(uint32_t c) {
  return (c + sizeof(word) * 8 - 1) / (sizeof(word) * 8);
}

// Notes on the memory layout: mzd_init allocates multiple memory blocks (one
// for mzd_local_t, one for rows and multiple for the buffers). We use one memory
// block for mzd_local_t, rows and the buffer. This improves memory locality and
// requires less calls to malloc.
//
// In mzd_local_init_multiple we do the same, but store n mzd_local_t instances in one
// memory block.

mzd_local_t* mzd_local_init_ex(uint32_t r, uint32_t c, bool clear) {
  const uint32_t width     = calculate_width(c);
  const uint32_t rowstride = calculate_rowstride(width);

  const size_t buffer_size = r * rowstride * sizeof(word);

  /* We always align mzd_local_ts to 32 bytes. Thus the first row is always
   * aligned to 32 bytes as well. For 128 bit and SSE all other rows are then
   * aligned to 16 bytes. */
  unsigned char* buffer = aligned_alloc(32, (mzd_local_t_size + buffer_size + 31) & ~31);

  mzd_local_t* A = (mzd_local_t*)buffer;

  // assign in order
  A->nrows     = r;
  A->ncols     = c;
  A->width     = width;
  A->rowstride = rowstride;

  if (clear) {
    buffer += mzd_local_t_size;
    memset(buffer, 0, buffer_size);
  }

  return A;
}

void mzd_local_free(mzd_local_t* v) {
  aligned_free(v);
}

void mzd_local_init_multiple_ex(mzd_local_t** dst, size_t n, uint32_t r, uint32_t c, bool clear) {
  const uint32_t width     = calculate_width(c);
  const uint32_t rowstride = calculate_rowstride(width);

  const size_t buffer_size   = r * rowstride * sizeof(word);
  const size_t size_per_elem = (mzd_local_t_size + buffer_size + 31) & ~31;

  unsigned char* full_buffer = aligned_alloc(32, size_per_elem * n);

  for (size_t s = 0; s < n; ++s, full_buffer += size_per_elem) {
    unsigned char* buffer = full_buffer;
    mzd_local_t* A        = dst[s] = (mzd_local_t*)buffer;

    // assign in order
    A->nrows     = r;
    A->ncols     = c;
    A->width     = width;
    A->rowstride = rowstride;

    if (clear) {
      buffer += mzd_local_t_size;
      memset(buffer, 0, buffer_size);
    }
  }
}

void mzd_local_free_multiple(mzd_local_t** vs) {
  if (vs) {
    aligned_free(vs[0]);
  }
}

void mzd_local_copy(mzd_local_t* dst, mzd_local_t const* src) {
  memcpy(ASSUME_ALIGNED(FIRST_ROW(dst), 32), ASSUME_ALIGNED(CONST_FIRST_ROW(src), 32),
         src->nrows * sizeof(word) * src->rowstride);
}

void mzd_local_clear(mzd_local_t* c) {
  memset(ASSUME_ALIGNED(FIRST_ROW(c), 32), 0, c->nrows * sizeof(word) * c->rowstride);
}

#if defined(WITH_OPT)
#if defined(WITH_SSE2)
ATTR_TARGET_SSE2
void mzd_xor_sse(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  unsigned int width        = first->rowstride;
  __m128i* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(__m128i));
  __m128i const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(__m128i));
  __m128i const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(__m128i));

  do {
    *mresptr++ = _mm_xor_si128(*mfirstptr++, *msecondptr++);
    width -= sizeof(__m128i) / sizeof(word);
  } while (width);
}

ATTR_TARGET_SSE2
void mzd_xor_sse_128(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  __m128i* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(__m128i));
  __m128i const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(__m128i));
  __m128i const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(__m128i));

  *mresptr = _mm_xor_si128(*mfirstptr, *msecondptr);
}

ATTR_TARGET_SSE2
void mzd_xor_sse_256(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  __m128i* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(__m128i));
  __m128i const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(__m128i));
  __m128i const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(__m128i));

  mresptr[0] = _mm_xor_si128(mfirstptr[0], msecondptr[0]);
  mresptr[1] = _mm_xor_si128(mfirstptr[1], msecondptr[1]);
}
#endif

#if defined(WITH_AVX2)
ATTR_TARGET_AVX2
void mzd_xor_avx(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  unsigned int width        = first->rowstride;
  __m256i* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(__m256i));
  __m256i const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(__m256i));
  __m256i const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(__m256i));
  do {
    *mresptr++ = _mm256_xor_si256(*mfirstptr++, *msecondptr++);
    width -= sizeof(__m256i) / sizeof(word);
  } while (width);
}

ATTR_TARGET_AVX2
void mzd_xor_avx_128(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  __m128i* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(__m128i));
  __m128i const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(__m128i));
  __m128i const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(__m128i));

  *mresptr = _mm_xor_si128(*mfirstptr, *msecondptr);
}

ATTR_TARGET_AVX2
void mzd_xor_avx_256(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  __m256i* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(__m256i));
  __m256i const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(__m256i));
  __m256i const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(__m256i));

  *mresptr = _mm256_xor_si256(*mfirstptr, *msecondptr);
}
#endif

#if defined(WITH_NEON)
void mzd_xor_neon(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  unsigned int width           = first->rowstride;
  uint64x2_t* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(uint64x2_t));
  uint64x2_t const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(uint64x2_t));
  uint64x2_t const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(uint64x2_t));

  do {
    *mresptr++ = veorq_u64(*mfirstptr++, *msecondptr++);
    width -= sizeof(uint64x2_t) / sizeof(word);
  } while (width);
}

void mzd_xor_neon_128(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  uint64x2_t* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(uint64x2_t));
  uint64x2_t const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(uint64x2_t));
  uint64x2_t const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(uint64x2_t));

  *mresptr = veorq_u64(*mfirstptr, *msecondptr);
}

void mzd_xor_neon_256(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  uint64x2_t* mresptr          = ASSUME_ALIGNED(FIRST_ROW(res), alignof(uint64x2_t));
  uint64x2_t const* mfirstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), alignof(uint64x2_t));
  uint64x2_t const* msecondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), alignof(uint64x2_t));

  mresptr[0] = veorq_u64(mfirstptr[0], msecondptr[0]);
  mresptr[1] = veorq_u64(mfirstptr[1], msecondptr[1]);
}
#endif
#endif

void mzd_xor_uint64(mzd_local_t* res, mzd_local_t const* first, mzd_local_t const* second) {
  unsigned int width    = first->width;
  word* resptr          = ASSUME_ALIGNED(FIRST_ROW(res), 32);
  word const* firstptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(first), 32);
  word const* secondptr = ASSUME_ALIGNED(CONST_FIRST_ROW(second), 32);

  while (width--) {
    *resptr++ = *firstptr++ ^ *secondptr++;
  }
}

void mzd_mul_v_parity_uint64_128_30(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  cptr[0] = 0;

  word res = 0;
  for(unsigned i = 30; i; --i) {
    word const* A     = CONST_ROW(At, 30 - i);
    word const popcnt = parity64_uint64((vptr[0] & A[0]) ^ (vptr[1] & A[1]));
    res |= popcnt << (64-i);
  }
  cptr[1] = res;
}

void mzd_mul_v_parity_uint64_192_30(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 2; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for (unsigned i = 30; i; --i) {
    word const* A     = CONST_ROW(At, 30 - i);
    const word popcnt = parity64_uint64((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]));
    res |= popcnt << (64 - i);
  }

  cptr[2] = res;
}

void mzd_mul_v_parity_uint64_256_30(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 3; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for(unsigned i = 30; i; --i) {
    word const* A     = CONST_ROW(At, 30 - i);
    word const popcnt = parity64_uint64((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]) ^ (vptr[3] & A[3]));
    res |= popcnt << (64-i);
  }
  cptr[3] = res;
}

void mzd_mul_v_parity_uint64_128_3(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  cptr[0] = 0;

  word res = 0;
  for (unsigned i = 3; i; --i) {
    word const* A     = CONST_ROW(At, 3 - i);
    word const popcnt = parity64_uint64((vptr[0] & A[0]) ^ (vptr[1] & A[1]));
    res |= popcnt << (64 - i);
  }
  cptr[1] = res;
}

void mzd_mul_v_parity_uint64_192_3(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 2; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for(unsigned i = 3; i; --i) {
    word const* A     = CONST_ROW(At, 3 - i);
    word const popcnt = parity64_uint64((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]));
    res |= popcnt << (64-i);
  }

  cptr[2] = res;
}

void mzd_mul_v_parity_uint64_256_3(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 3; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for(unsigned i = 3; i; --i) {
    word const* A     = CONST_ROW(At, 3 - i);
    word const popcnt = parity64_uint64((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]) ^ (vptr[3] & A[3]));
    res |= popcnt << (64-i);
  }
  cptr[3] = res;
}

#if defined(WITH_OPT)
#if defined(WITH_POPCNT)
ATTR_TARGET("popcnt")
void mzd_mul_v_parity_popcnt_128_30(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  cptr[0] = 0;

  word res = 0;
  for(unsigned i = 30; i; --i) {
    word const* A     = CONST_ROW(At, 30 - i);
    word const popcnt = parity64_popcnt((vptr[0] & A[0]) ^ (vptr[1] & A[1]));
    res |= popcnt << (64-i);
  }
  cptr[1] = res;
}

ATTR_TARGET("popcnt")
void mzd_mul_v_parity_popcnt_192_30(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 2; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for(unsigned i = 30; i; --i) {
    word const* A     = CONST_ROW(At, 30 - i);
    const word popcnt = parity64_popcnt((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]));
    res |= popcnt << (64-i);
  }
  cptr[2] = res;
}

ATTR_TARGET("popcnt")
void mzd_mul_v_parity_popcnt_256_30(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 3; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for(unsigned i = 30; i; --i) {
    word const* A     = CONST_ROW(At, 30 - i);
    word const popcnt = parity64_popcnt((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]) ^ (vptr[3] & A[3]));
    res |= popcnt << (64-i);
  }
  cptr[3] = res;
}

ATTR_TARGET("popcnt")
void mzd_mul_v_parity_popcnt_128_3(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  cptr[0] = 0;

  word res = 0;
  for (unsigned i = 3; i; --i) {
    word const* A     = CONST_ROW(At, 3 - i);
    word const popcnt = parity64_popcnt((vptr[0] & A[0]) ^ (vptr[1] & A[1]));
    res |= popcnt << (64 - i);
  }
  cptr[1] = res;
}

ATTR_TARGET("popcnt")
void mzd_mul_v_parity_popcnt_192_3(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 2; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for (unsigned i = 3; i; --i) {
    word const* A     = CONST_ROW(At, 3 - i);
    word const popcnt = parity64_popcnt((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]));
    res |= popcnt << (64-i);
  }
  cptr[2] = res;
}

ATTR_TARGET("popcnt")
void mzd_mul_v_parity_popcnt_256_3(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  for (unsigned int j = 0; j < 3; j++) {
    cptr[j] = 0;
  }

  word res = 0;
  for(unsigned i = 3; i; --i) {
    word const* A     = CONST_ROW(At, 3 - i);
    word const popcnt = parity64_popcnt((vptr[0] & A[0]) ^ (vptr[1] & A[1]) ^ (vptr[2] & A[2]) ^ (vptr[3] & A[3]));
    res |= popcnt << (64-i);
  }
  cptr[3] = res;
}
#endif

#if defined(WITH_SSE2)
ATTR_TARGET_SSE2 ATTR_CONST
static inline __m128i mm128_compute_mask(const word idx, const size_t bit) {
  return _mm_set1_epi64x(-((idx >> bit) & 1));
}

ATTR_TARGET_SSE2
static void mzd_addmul_v_sse(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr              = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  const unsigned int width      = v->width;
  const unsigned int rowstride  = A->rowstride;
  const unsigned int mrowstride = rowstride * sizeof(word) / sizeof(__m128i);
  const unsigned int len        = mrowstride;

  __m128i* mcptr = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));

  for (unsigned int w = 0; w < width; ++w, ++vptr) {
    word idx             = *vptr;
    __m128i const* mAptr = ASSUME_ALIGNED(CONST_ROW(A, w * sizeof(word) * 8), alignof(__m128i));

    for (unsigned int i = sizeof(word) * 8; i; --i, idx >>= 1, mAptr += mrowstride) {
      mm128_xor_mask_region(mcptr, mAptr, mm128_compute_mask(idx, 0), len);
    }
  }
}

ATTR_TARGET_SSE2
void mzd_mul_v_sse(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  mzd_local_clear(c);
  mzd_addmul_v_sse(c, v, A);
}

ATTR_TARGET_SSE2
void mzd_mul_v_sse_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[2] ATTR_ALIGNED(alignof(__m128i)) = {_mm_setzero_si128(), _mm_setzero_si128()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
      cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
      cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
      cval[1] = mm128_xor_mask(cval[1], mAptr[3], mm128_compute_mask(idx, 3));
    }
  }
  *mcptr = _mm_xor_si128(cval[0], cval[1]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[2] ATTR_ALIGNED(alignof(__m128i)) = {*mcptr, _mm_setzero_si128()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
      cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
      cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
      cval[1] = mm128_xor_mask(cval[1], mAptr[3], mm128_compute_mask(idx, 3));
    }
  }
  *mcptr = _mm_xor_si128(cval[0], cval[1]);
}

ATTR_TARGET_SSE2
void mzd_mul_v_sse_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {_mm_setzero_si128(), _mm_setzero_si128(),
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1], _mm_setzero_si128(),
                                                    _mm_setzero_si128()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_mul_v_sse_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {_mm_setzero_si128(), _mm_setzero_si128(),
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1], _mm_setzero_si128(),
                                                    _mm_setzero_si128()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}
#endif

#if defined(WITH_AVX2)
ATTR_TARGET_AVX2
ATTR_CONST static inline __m256i mm256_compute_mask(const word idx, const size_t bit) {
  return _mm256_set1_epi64x(-((idx >> bit) & 1));
}

ATTR_TARGET_AVX2
ATTR_CONST static inline __m256i mm256_compute_mask_2(const word idx, const size_t bit) {
  const uint64_t m1 = -((idx >> bit) & 1);
  const uint64_t m2 = -((idx >> (bit + 1)) & 1);
  return _mm256_set_epi64x(m2, m2, m1, m1);
}

ATTR_TARGET_AVX2
static void mzd_addmul_v_avx(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr              = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  const unsigned int width      = v->width;
  const unsigned int rowstride  = A->rowstride;
  const unsigned int mrowstride = rowstride * sizeof(word) / sizeof(__m256i);
  const unsigned int len        = mrowstride;

  __m256i* mcptr = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));

  for (unsigned int w = 0; w < width; ++w, ++vptr) {
    word idx             = *vptr;
    __m256i const* mAptr = ASSUME_ALIGNED(CONST_ROW(A, w * sizeof(word) * 8), alignof(__m256i));

    for (unsigned int i = sizeof(word) * 8; i; --i, idx >>= 1, mAptr += mrowstride) {
      mm256_xor_mask_region(mcptr, mAptr, mm256_compute_mask(idx, 0), len);
    }
  }
}

ATTR_TARGET_AVX2
void mzd_mul_v_avx(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  mzd_local_clear(c);
  mzd_addmul_v_avx(c, v, A);
}

ATTR_TARGET_AVX2
void mzd_addmul_v_avx_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_castsi128_si256(*mcptr),
                                                    _mm256_setzero_si256()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 8, idx >>= 8, mAptr += 4) {
      cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask_2(idx, 0));
      cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask_2(idx, 2));
      cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask_2(idx, 4));
      cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask_2(idx, 6));
    }
  }
  cval[0] = _mm256_xor_si256(cval[0], cval[1]);
  *mcptr =
      _mm_xor_si128(_mm256_extractf128_si256(cval[0], 0), _mm256_extractf128_si256(cval[0], 1));
}

ATTR_TARGET_AVX2
void mzd_mul_v_avx_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_setzero_si256(), _mm256_setzero_si256()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 8, idx >>= 8, mAptr += 4) {
      cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask_2(idx, 0));
      cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask_2(idx, 2));
      cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask_2(idx, 4));
      cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask_2(idx, 6));
    }
  }
  cval[0] = _mm256_xor_si256(cval[0], cval[1]);
  *mcptr =
      _mm_xor_si128(_mm256_extractf128_si256(cval[0], 0), _mm256_extractf128_si256(cval[0], 1));
}

ATTR_TARGET_AVX2
void mzd_addmul_v_avx_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
      cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
      cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
      cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask(idx, 3));
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_mul_v_avx_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_setzero_si256(), _mm256_setzero_si256()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
      cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
      cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
      cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask(idx, 3));
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_addmul_v_avx_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
      cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
      cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
      cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask(idx, 3));
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_mul_v_avx_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_setzero_si256(), _mm256_setzero_si256()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
      cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
      cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
      cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask(idx, 3));
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

#endif

#if defined(WITH_NEON)
ATTR_CONST
static inline uint64x2_t mm128_compute_mask(const word idx, size_t bit) {
  return vdupq_n_u64(-((idx >> bit) & 1));
}

static void mzd_addmul_v_neon(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  const unsigned int width      = v->width;
  const unsigned int rowstride  = A->rowstride;
  const unsigned int mrowstride = rowstride * sizeof(word) / sizeof(uint64x2_t);
  const unsigned int len        = mrowstride;

  word const* vptr  = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));

  for (unsigned int w = 0; w < width; ++w, ++vptr) {
    word idx = *vptr;
    uint64x2_t const* mAptr =
        ASSUME_ALIGNED(CONST_ROW(A, w * sizeof(word) * 8), alignof(uint64x2_t));

    for (unsigned int i = sizeof(word) * 8; i; --i, idx >>= 1, mAptr += mrowstride) {
      mm128_xor_mask_region(mcptr, mAptr, mm128_compute_mask(idx, 0), len);
    }
  }
}

void mzd_mul_v_neon(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  mzd_local_clear(c);
  mzd_addmul_v_neon(c, v, A);
}

void mzd_mul_v_neon_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[2] ATTR_ALIGNED(alignof(uint64x2_t)) = {vmovq_n_u64(0), vmovq_n_u64(0)};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
      cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
      cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
      cval[1] = mm128_xor_mask(cval[1], mAptr[3], mm128_compute_mask(idx, 3));
    }
  }
  *mcptr = veorq_u64(cval[0], cval[1]);
}

void mzd_addmul_v_neon_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[2] ATTR_ALIGNED(alignof(uint64x2_t)) = {*mcptr, vmovq_n_u64(0)};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 4, idx >>= 4, mAptr += 4) {
      cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
      cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
      cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
      cval[1] = mm128_xor_mask(cval[1], mAptr[3], mm128_compute_mask(idx, 3));
    }
  }
  *mcptr = veorq_u64(cval[0], cval[1]);
}

void mzd_mul_v_neon_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {vmovq_n_u64(0), vmovq_n_u64(0),
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_addmul_v_neon_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {mcptr[0], mcptr[1], vmovq_n_u64(0),
                                                          vmovq_n_u64(0)};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_mul_v_neon_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {vmovq_n_u64(0), vmovq_n_u64(0),
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_addmul_v_neon_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);

  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t ATTR_ALIGNED(alignof(uint64x2_t))
      cval[4] = {mcptr[0], mcptr[1], vmovq_n_u64(0), vmovq_n_u64(0)};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int i = sizeof(word) * 8; i; i -= 2, idx >>= 2, mAptr += 4) {
      mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
      mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}
#endif
#endif

void mzd_addmul_v_uint64(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  const unsigned int len       = A->width;
  const unsigned int rowstride = A->rowstride;
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  const unsigned int width     = v->width;
  word const* Aptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(A), 32);

  for (unsigned int w = width; w; --w, ++vptr) {
    word idx = *vptr;

    for (unsigned int i = sizeof(word) * 8; i; --i, idx >>= 1, Aptr += rowstride) {
      const uint64_t mask = -(idx & 1);
      for (unsigned int j = 0; j < len; ++j) {
        cptr[j] ^= (Aptr[j] & mask);
      }
    }
  }
}

void mzd_mul_v_uint64(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* At) {
  mzd_local_clear(c);
  mzd_addmul_v_uint64(c, v, At);
}

bool mzd_local_equal(mzd_local_t const* first, mzd_local_t const* second) {
  if (first == second) {
    return true;
  }
  if (first->ncols != second->ncols || first->nrows != second->nrows) {
    return false;
  }

  const unsigned int rows  = first->nrows;
  const unsigned int width = first->width;

  for (unsigned int r = 0; r < rows; ++r) {
    if (memcmp(ASSUME_ALIGNED(CONST_ROW(first, r), 32), ASSUME_ALIGNED(CONST_ROW(second, r), 32),
               sizeof(word) * width) != 0) {
      return false;
    }
  }

  return true;
}

#if defined(MUL_M4RI)
static void xor_comb(const unsigned int len, word* Brow, mzd_local_t const* A,
                     unsigned int r_offset, unsigned comb) {
  while (comb) {
    const word* Arow = CONST_ROW(A, r_offset);
    if (comb & 0x1) {
      for (unsigned int i = 0; i < len; ++i) {
        Brow[i] ^= Arow[i];
      }
    }

    comb >>= 1;
    ++r_offset;
  }
}

/**
 * Pre-compute matrices for faster mzd_addmul_v computions.
 */
mzd_local_t* mzd_precompute_matrix_lookup(mzd_local_t const* A) {
  mzd_local_t* B = mzd_local_init_ex(32 * A->nrows, A->ncols, true);

  const unsigned int len = A->width;
  for (unsigned int r = 0; r < B->nrows; ++r) {
    const unsigned int comb     = r & 0xff;
    const unsigned int r_offset = (r >> 8) << 3;
    if (!comb) {
      continue;
    }

    xor_comb(len, ROW(B, r), A, r_offset, comb);
  }

  return B;
}

#if defined(WITH_OPT)
#if defined(WITH_SSE2)
ATTR_TARGET_SSE2
static void mzd_addmul_vl_sse(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr              = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  const unsigned int width      = v->width;
  const unsigned int rowstride  = A->rowstride;
  const unsigned int mrowstride = rowstride * sizeof(word) / sizeof(__m128i);
  const unsigned int len        = mrowstride;
  const unsigned int moff2      = 256 * mrowstride;

  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  for (unsigned int w = width; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; --s, idx >>= 8, mAptr += moff2) {
      const word comb = idx & 0xff;
      mm128_xor_region(mcptr, mAptr + comb * mrowstride, len);
    }
  }
}

ATTR_TARGET_SSE2
void mzd_mul_vl_sse(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  mzd_local_clear(c);
  mzd_addmul_vl_sse(c, v, A);
}

ATTR_TARGET_SSE2
void mzd_addmul_vl_sse_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 256;

  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[2] ATTR_ALIGNED(alignof(__m128i)) = {*mcptr, _mm_setzero_si128()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm128_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = _mm_xor_si128(cval[0], cval[1]);
}

ATTR_TARGET_SSE2
void mzd_mul_vl_sse_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 256;

  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[2] ATTR_ALIGNED(alignof(__m128i)) = {_mm_setzero_si128(), _mm_setzero_si128()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm128_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = _mm_xor_si128(cval[0], cval[1]);
}

ATTR_TARGET_SSE2
void mzd_addmul_vl_sse_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1], _mm_setzero_si128(),
                                                    _mm_setzero_si128()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_mul_vl_sse_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {_mm_setzero_si128(), _mm_setzero_si128(),
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_addmul_vl_sse_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1], _mm_setzero_si128(),
                                                    _mm_setzero_si128()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_mul_vl_sse_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {_mm_setzero_si128(), _mm_setzero_si128(),
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}
#endif

#if defined(WITH_AVX2)
ATTR_TARGET_AVX2
void mzd_mul_vl_avx_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr                = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  static const unsigned int moff2 = 256;

  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_setzero_si256(), _mm256_setzero_si256()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm256_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm256_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_addmul_vl_avx_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr                = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  static const unsigned int moff2 = 256;

  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm256_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm256_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_mul_vl_avx_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr                = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  static const unsigned int moff2 = 256;

  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_setzero_si256(), _mm256_setzero_si256()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm256_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm256_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_addmul_vl_avx_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr                = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  static const unsigned int moff2 = 256;

  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm256_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm256_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_mul_vl_avx_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr                = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  static const unsigned int moff2 = 256;

  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_setzero_si256(), _mm256_setzero_si256()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 4, idx >>= 32) {
      const __m256i t1 =
          _mm256_set_m128i(mAptr[(idx >> 0) & 0xff], mAptr[((idx >> 8) & 0xff) + moff2]);
      mm256_xor_region(&cval[0], &t1, 1);
      mAptr += 2 * moff2;

      const __m256i t2 =
          _mm256_set_m128i(mAptr[(idx >> 16) & 0xff], mAptr[((idx >> 24) & 0xff) + moff2]);
      mm256_xor_region(&cval[1], &t2, 1);
      mAptr += 2 * moff2;
    }
  }
  cval[0] = _mm256_xor_si256(cval[0], cval[1]);
  *mcptr =
      _mm_xor_si128(_mm256_extractf128_si256(cval[0], 0), _mm256_extractf128_si256(cval[0], 1));
}

ATTR_TARGET_AVX2
void mzd_addmul_vl_avx_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr                = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  static const unsigned int moff2 = 256;

  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_castsi128_si256(*mcptr),
                                                    _mm256_setzero_si256()};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 4, idx >>= 32) {
      const __m256i t1 =
          _mm256_set_m128i(mAptr[(idx >> 0) & 0xff], mAptr[((idx >> 8) & 0xff) + moff2]);
      mm256_xor_region(&cval[0], &t1, 1);
      mAptr += 2 * moff2;

      const __m256i t2 =
          _mm256_set_m128i(mAptr[(idx >> 16) & 0xff], mAptr[((idx >> 24) & 0xff) + moff2]);
      mm256_xor_region(&cval[1], &t2, 1);
      mAptr += 2 * moff2;
    }
  }
  cval[0] = _mm256_xor_si256(cval[0], cval[1]);
  *mcptr =
      _mm_xor_si128(_mm256_extractf128_si256(cval[0], 0), _mm256_extractf128_si256(cval[0], 1));
}

ATTR_TARGET_AVX2
static void mzd_addmul_vl_avx(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  const unsigned int width      = v->width;
  const unsigned int rowstride  = A->rowstride;
  const unsigned int mrowstride = rowstride * sizeof(word) / sizeof(__m256i);
  const unsigned int moff2      = 256 * mrowstride;
  const unsigned int len        = mrowstride;

  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  for (unsigned int w = width; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; --s, idx >>= 8, mAptr += moff2) {
      const word comb = idx & 0xff;
      mm256_xor_region(mcptr, mAptr + comb * mrowstride, len);
    }
  }
}

ATTR_TARGET_AVX2
void mzd_mul_vl_avx(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  mzd_local_clear(c);
  mzd_addmul_vl_avx(c, v, A);
}
#endif

#if defined(WITH_NEON)
void mzd_mul_vl_neon_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 256;

  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[2] ATTR_ALIGNED(alignof(uint64x2_t)) = {vmovq_n_u64(0), vmovq_n_u64(0)};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm128_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = veorq_u64(cval[0], cval[1]);
}

void mzd_mul_vl_neon_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {vmovq_n_u64(0), vmovq_n_u64(0),
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_mul_vl_neon_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {vmovq_n_u64(0), vmovq_n_u64(0),
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_addmul_vl_neon_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 256;

  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[2] ATTR_ALIGNED(alignof(uint64x2_t)) = {*mcptr, vmovq_n_u64(0)};
  for (unsigned int w = 2; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + ((idx >> 0) & 0xff), 1);
      mAptr += moff2;
      mm128_xor_region(&cval[1], mAptr + ((idx >> 8) & 0xff), 1);
      mAptr += moff2;
    }
  }
  *mcptr = veorq_u64(cval[0], cval[1]);
}

void mzd_addmul_vl_neon_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {mcptr[0], mcptr[1], vmovq_n_u64(0),
                                                          vmovq_n_u64(0)};
  for (unsigned int w = 3; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_addmul_vl_neon_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  static const unsigned int moff2 = 512;

  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {mcptr[0], mcptr[1], vmovq_n_u64(0),
                                                          vmovq_n_u64(0)};
  for (unsigned int w = 4; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; s -= 2, idx >>= 16) {
      mm128_xor_region(&cval[0], mAptr + 2 * ((idx >> 0) & 0xff), 2);
      mAptr += moff2;
      mm128_xor_region(&cval[2], mAptr + 2 * ((idx >> 8) & 0xff), 2);
      mAptr += moff2;
    }
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

static void mzd_addmul_vl_neon(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr              = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  const unsigned int width      = v->width;
  const unsigned int rowstride  = A->rowstride;
  const unsigned int mrowstride = rowstride * sizeof(word) / sizeof(uint64x2_t);
  const unsigned int len        = mrowstride;
  const unsigned int moff2      = 256 * mrowstride;

  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  for (unsigned int w = width; w; --w, ++vptr) {
    word idx = *vptr;
    for (unsigned int s = sizeof(word); s; --s, idx >>= 8, mAptr += moff2) {
      const word comb = idx & 0xff;
      mm128_xor_region(mcptr, mAptr + comb * mrowstride, len);
    }
  }
}

void mzd_mul_vl_neon(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  mzd_local_clear(c);
  mzd_addmul_vl_neon(c, v, A);
}
#endif
#endif

void mzd_addmul_vl_uint64(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  const unsigned int len   = A->width;
  word* cptr               = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr         = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  const unsigned int width = v->width;

  for (unsigned int w = 0; w < width; ++w, ++vptr) {
    word idx         = *vptr;
    unsigned int add = 0;

    while (idx) {
      const word comb = idx & 0xff;

      word const* Aptr = CONST_ROW(A, w * sizeof(word) * 8 * 32 + add + comb);
      for (unsigned int i = 0; i < len; ++i) {
        cptr[i] ^= Aptr[i];
      }

      idx >>= 8;
      add += 256;
    }
  }
}

void mzd_mul_vl_uint64(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  mzd_local_clear(c);
  mzd_addmul_vl_uint64(c, v, A);
}
#endif

// bit extract, non-constant time for mask, but mask is public in our calls
static word extract_bits(word in, word mask) {
  word res = 0;
  for (word bb = 1; mask != 0; bb <<= 1, mask &= (mask - 1)) {
    res |= bb & (-((word) !!(in & mask & -mask)));
  }
  return res;
}

void mzd_shuffle_30(mzd_local_t* x, const word mask) {
  word a                     = extract_bits(CONST_FIRST_ROW(x)[x->width - 1], mask) << (34);
  FIRST_ROW(x)[x->width - 1] = a | extract_bits(CONST_FIRST_ROW(x)[x->width - 1], ~(mask));
}

void mzd_shuffle_3(mzd_local_t* x, const word mask) {
  word a                     = extract_bits(CONST_FIRST_ROW(x)[x->width - 1], mask) << (61);
  FIRST_ROW(x)[x->width - 1] = a | extract_bits(CONST_FIRST_ROW(x)[x->width - 1], ~(mask));
}

// specific instances
#if defined(OPTIMIZED_LINEAR_LAYER_EVALUATION)
//no simd
void mzd_addmul_v_uint64_30(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
    const unsigned int rowstride = A->rowstride;
    word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
    word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
    word const* Aptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(A), 32);
    const unsigned int width     = v->width;

    word idx = vptr[width-1] >> 34;
    for (unsigned int i = 30; i; --i, idx >>= 1, Aptr += rowstride) {
      const uint64_t mask = -(idx & 1);
      for(unsigned int j = 0; j < width; j++) {
        cptr[j] ^= (Aptr[j] & mask);
      }
    }
}

void mzd_addmul_v_uint64_3(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  const unsigned int rowstride = A->rowstride;
  word* cptr                   = ASSUME_ALIGNED(FIRST_ROW(c), 32);
  word const* vptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  word const* Aptr             = ASSUME_ALIGNED(CONST_FIRST_ROW(A), 32);
  const unsigned int width     = v->width;

  word idx = vptr[width-1] >> 61;
  for (unsigned int i = 3; i; --i, idx >>= 1, Aptr += rowstride) {
    const uint64_t mask = -(idx & 1);
    for(unsigned int j = 0; j < width; j++) {
      cptr[j] ^= (Aptr[j] & mask);
    }
  }
}

#if defined(WITH_SSE2)
ATTR_TARGET_SSE2
void mzd_addmul_v_sse_30_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[2] ATTR_ALIGNED(alignof(__m128i)) = {*mcptr, _mm_setzero_si128()};
  word idx = vptr[1] >> 34;
  for (unsigned int i = 28; i; i -= 4, idx >>= 4, mAptr += 4) {
    cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
    cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
    cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
    cval[1] = mm128_xor_mask(cval[1], mAptr[3], mm128_compute_mask(idx, 3));
  }
  cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
  cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
  *mcptr = _mm_xor_si128(cval[0], cval[1]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_30_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1],
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  word idx = vptr[2] >> 34;
  for (unsigned int i = 30; i; i -= 2, idx >>= 2, mAptr += 4) {
    mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
    mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_30_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1],
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  word idx = vptr[3] >> 34;
  for (unsigned int i = 30; i; i -= 2, idx >>= 2, mAptr += 4) {
    mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
    mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  }
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_3_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[2] ATTR_ALIGNED(alignof(__m128i)) = {*mcptr, _mm_setzero_si128()};
  word idx = vptr[1] >> 61;
  cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
  cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
  cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
  *mcptr = _mm_xor_si128(cval[0], cval[1]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_3_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1],
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  word idx = vptr[2] >> 61;
  mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
  mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  mm128_xor_mask_region(&cval[0], mAptr + 4, mm128_compute_mask(idx, 2), 2);
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}

ATTR_TARGET_SSE2
void mzd_addmul_v_sse_3_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m128i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m128i));

  __m128i cval[4] ATTR_ALIGNED(alignof(__m128i)) = {mcptr[0], mcptr[1],
                                                    _mm_setzero_si128(), _mm_setzero_si128()};
  word idx = vptr[3] >> 61;
  mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
  mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  mm128_xor_mask_region(&cval[0], mAptr + 4, mm128_compute_mask(idx, 2), 2);
  mcptr[0] = _mm_xor_si128(cval[0], cval[2]);
  mcptr[1] = _mm_xor_si128(cval[1], cval[3]);
}
#endif

#if defined(WITH_AVX2)
ATTR_TARGET_AVX2
void mzd_addmul_v_avx_30_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m128i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m128i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {_mm256_castsi128_si256(*mcptr), _mm256_setzero_si256()};
  word idx = vptr[1] >> 34;
  for (unsigned int i = 24; i; i -= 8, idx >>= 8, mAptr += 4) {
    cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask_2(idx, 0));
    cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask_2(idx, 2));
    cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask_2(idx, 4));
    cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask_2(idx, 6));
  }
  cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask_2(idx, 0));
  cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask_2(idx, 2));
  cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask_2(idx, 4));

  cval[0] = _mm256_xor_si256(cval[0], cval[1]);
  *mcptr = _mm_xor_si128(_mm256_extractf128_si256(cval[0], 0), _mm256_extractf128_si256(cval[0], 1));
}

ATTR_TARGET_AVX2
void mzd_addmul_v_avx_30_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  word idx = vptr[2] >> 34;
  // do 7x4 and then 2 extra to get 30
  for (unsigned int i = 28; i; i -= 4, idx >>= 4, mAptr += 4) {
    cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
    cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
    cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
    cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask(idx, 3));
  }
  cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
  cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
  *mcptr  = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_addmul_v_avx_30_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  word idx = vptr[3] >> 34;
  // do 7x4 and then 2 extra to get 30
  for (unsigned int i = 28; i; i -= 4, idx >>= 4, mAptr += 4) {
    cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
    cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
    cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
    cval[1] = mm256_xor_mask(cval[1], mAptr[3], mm256_compute_mask(idx, 3));
  }
  cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
  cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_addmul_v_avx_3_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  word idx = vptr[2] >> 61;
  cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
  cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
  cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

ATTR_TARGET_AVX2
void mzd_addmul_v_avx_3_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr     = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  __m256i* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(__m256i));
  __m256i const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(__m256i));

  __m256i cval[2] ATTR_ALIGNED(alignof(__m256i)) = {*mcptr, _mm256_setzero_si256()};
  word idx = vptr[3] >> 61;
  cval[0] = mm256_xor_mask(cval[0], mAptr[0], mm256_compute_mask(idx, 0));
  cval[1] = mm256_xor_mask(cval[1], mAptr[1], mm256_compute_mask(idx, 1));
  cval[0] = mm256_xor_mask(cval[0], mAptr[2], mm256_compute_mask(idx, 2));
  *mcptr = _mm256_xor_si256(cval[0], cval[1]);
}

#if !defined(__x86_64__) && !defined(_M_X64)
ATTR_TARGET_AVX2 ATTR_CONST static uint8_t popcount_32(uint32_t value) {
  uint64_t result = ((value & 0xfff) * UINT64_C(0x1001001001001) & UINT64_C(0x84210842108421)) % 0x1f;
  result += (((value & 0xfff000) >> 12) * UINT64_C(0x1001001001001) & UINT64_C(0x84210842108421)) % 0x1f;
  result += ((value >> 24) * UINT64_C(0x1001001001001) & UINT64_C(0x84210842108421)) % 0x1f;
  return result;
}

ATTR_TARGET_AVX2 ATTR_CONST static uint64_t _pext_u64(uint64_t a, uint64_t mask) {
  const uint32_t low  = _pext_u32(a, mask);
  const uint32_t high = _pext_u32(a >> 32, mask >> 32);

  return (((uint64_t)high) << popcount_32(mask)) | low;
}
#endif

ATTR_TARGET_AVX2
void mzd_shuffle_pext_30(mzd_local_t* x, const word mask) {
  word a                     = _pext_u64(CONST_FIRST_ROW(x)[x->width - 1], mask) << (34);
  FIRST_ROW(x)[x->width - 1] = a | _pext_u64(CONST_FIRST_ROW(x)[x->width - 1], ~(mask));
}

ATTR_TARGET_AVX2
void mzd_shuffle_pext_3(mzd_local_t* x, const word mask) {
  word a                     = _pext_u64(CONST_FIRST_ROW(x)[x->width - 1], mask) << (61);
  FIRST_ROW(x)[x->width - 1] = a | _pext_u64(CONST_FIRST_ROW(x)[x->width - 1], ~(mask));
}
#endif

#if defined(WITH_NEON)
void mzd_addmul_v_neon_30_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[2] ATTR_ALIGNED(alignof(uint64x2_t)) = {*mcptr, vmovq_n_u64(0)};
  word idx = vptr[1] >> 34;
  for (unsigned int i = 28; i; i -= 4, idx >>= 4, mAptr += 4) {
    cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
    cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
    cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
    cval[1] = mm128_xor_mask(cval[1], mAptr[3], mm128_compute_mask(idx, 3));
  }
  cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
  cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
  *mcptr = veorq_u64(cval[0], cval[1]);
}

void mzd_addmul_v_neon_30_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {mcptr[0], mcptr[1],
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  word idx = vptr[2] >> 34;
  for (unsigned int i = 30; i; i -= 2, idx >>= 2, mAptr += 4) {
    mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
    mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_addmul_v_neon_30_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {mcptr[0], mcptr[1],
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  word idx = vptr[3] >> 34;
  for (unsigned int i = 30; i; i -= 2, idx >>= 2, mAptr += 4) {
    mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
    mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  }
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_addmul_v_neon_3_128(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[2] ATTR_ALIGNED(alignof(uint64x2_t)) = {*mcptr, vmovq_n_u64(0)};
  word idx = vptr[1] >> 61;
  cval[0] = mm128_xor_mask(cval[0], mAptr[0], mm128_compute_mask(idx, 0));
  cval[1] = mm128_xor_mask(cval[1], mAptr[1], mm128_compute_mask(idx, 1));
  cval[0] = mm128_xor_mask(cval[0], mAptr[2], mm128_compute_mask(idx, 2));
  *mcptr = veorq_u64(cval[0], cval[1]);
}

void mzd_addmul_v_neon_3_192(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {mcptr[0], mcptr[1],
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  word idx = vptr[2] >> 61;
  mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
  mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  mm128_xor_mask_region(&cval[0], mAptr + 4, mm128_compute_mask(idx, 2), 2);
  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}

void mzd_addmul_v_neon_3_256(mzd_local_t* c, mzd_local_t const* v, mzd_local_t const* A) {
  word const* vptr        = ASSUME_ALIGNED(CONST_FIRST_ROW(v), 32);
  uint64x2_t* mcptr       = ASSUME_ALIGNED(FIRST_ROW(c), alignof(uint64x2_t));
  uint64x2_t const* mAptr = ASSUME_ALIGNED(CONST_FIRST_ROW(A), alignof(uint64x2_t));

  uint64x2_t cval[4] ATTR_ALIGNED(alignof(uint64x2_t)) = {mcptr[0], mcptr[1],
                                                          vmovq_n_u64(0), vmovq_n_u64(0)};
  word idx = vptr[3] >> 61;
  mm128_xor_mask_region(&cval[0], mAptr + 0, mm128_compute_mask(idx, 0), 2);
  mm128_xor_mask_region(&cval[2], mAptr + 2, mm128_compute_mask(idx, 1), 2);
  mm128_xor_mask_region(&cval[0], mAptr + 4, mm128_compute_mask(idx, 2), 2);

  mcptr[0] = veorq_u64(cval[0], cval[2]);
  mcptr[1] = veorq_u64(cval[1], cval[3]);
}
#endif

#endif
