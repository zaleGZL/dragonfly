// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "src/core/detail/bitpacking.h"

#include "base/logging.h"

#if defined(__aarch64__)
#include "base/sse2neon.h"
#else
#include <emmintrin.h>
#endif
#include <absl/base/internal/endian.h>

namespace dfly {

namespace detail {

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("Ofast")
#endif

// Daniel Lemire's function validate_ascii_fast() - under Apache/MIT license.
// See https://github.com/lemire/fastvalidate-utf-8/
// The function returns true (1) if all chars passed in src are
// 7-bit values (0x00..0x7F). Otherwise, it returns false (0).
bool validate_ascii_fast(const char* src, size_t len) {
  size_t i = 0;
  __m128i has_error = _mm_setzero_si128();
  if (len >= 16) {
    for (; i <= len - 16; i += 16) {
      __m128i current_bytes = _mm_loadu_si128((const __m128i*)(src + i));
      has_error = _mm_or_si128(has_error, current_bytes);
    }
  }
  int error_mask = _mm_movemask_epi8(has_error);

  char tail_has_error = 0;
  for (; i < len; i++) {
    tail_has_error |= src[i];
  }
  error_mask |= (tail_has_error & 0x80);

  return !error_mask;
}

// len must be at least 16
void ascii_pack(const char* ascii, size_t len, uint8_t* bin) {
  uint64_t val;
  const char* end = ascii + len;

  while (ascii + 8 <= end) {
    val = absl::little_endian::Load64(ascii);
    uint64_t dest = (val & 0xFF);
    for (unsigned i = 1; i <= 7; ++i) {
      val >>= 1;
      dest |= (val & (0x7FUL << 7 * i));
    }
    memcpy(bin, &dest, 7);
    bin += 7;
    ascii += 8;
  }

  // epilog - we do not pack since we have less than 8 bytes.
  while (ascii < end) {
    *bin++ = *ascii++;
  }
}

// The algo - do in parallel what ascii_pack does on two uint64_t integers
void ascii_pack_simd(const char* ascii, size_t len, uint8_t* bin) {
  __m128i val;

  __m128i mask = _mm_set1_epi64x(0x7FU);  // two uint64_t masks

  // I leave out 16 bytes in addition to 16 that we load in the loop
  // because we store into bin full 16 bytes instead of 14. To prevent data
  // overwrite we finish loop one iteration earlier.
  const char* end = ascii + len - 32;
  while (ascii <= end) {
    val = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ascii));
    __m128i dest = _mm_and_si128(val, mask);

    // Compiler unrolls it
    for (unsigned i = 1; i <= 7; ++i) {
      _mm_srli_epi64(val, 1);                          // shift right both integers
      __m128i shmask = _mm_slli_epi64(mask, 7 * i);    // mask both
      _mm_or_si128(dest, _mm_and_si128(val, shmask));  // add another 7bit part.
    }

    // dest contains two 7 byte blobs. Lets copy them to bin.
    _mm_storeu_si128(reinterpret_cast<__m128i*>(bin), dest);
    memcpy(bin + 7, bin + 8, 7);
    bin += 14;
    ascii += 16;
  }

  end += 32;  // Bring back end.
  DCHECK(ascii < end);
  ascii_pack(ascii, end - ascii, bin);
}

// unpacks 8->7 encoded blob back to ascii.
// generally, we can not unpack inplace because ascii (dest) buffer is 8/7 bigger than
// the source buffer.
// however, if binary data is positioned on the right of the ascii buffer with empty space on the
// left than we can unpack inplace.
void ascii_unpack(const uint8_t* bin, size_t ascii_len, char* ascii) {
  constexpr uint8_t kM = 0x7F;
  uint8_t p = 0;
  unsigned i = 0;

  while (ascii_len >= 8) {
    for (i = 0; i < 7; ++i) {
      uint8_t src = *bin;  // keep on stack in case we unpack inplace.
      *ascii++ = (p >> (8 - i)) | ((src << i) & kM);
      p = src;
      ++bin;
    }

    ascii_len -= 8;
    *ascii++ = p >> 1;
  }

  DCHECK_LT(ascii_len, 8u);
  for (i = 0; i < ascii_len; ++i) {
    *ascii++ = *bin++;
  }
}

// compares packed and unpacked strings. packed must be of length = binpacked_len(ascii_len).
bool compare_packed(const uint8_t* packed, const char* ascii, size_t ascii_len) {
  unsigned i = 0;
  bool res = true;
  const char* end = ascii + ascii_len;

  while (ascii + 8 <= end) {
    for (i = 0; i < 7; ++i) {
      uint8_t conv = (ascii[0] >> i) | (ascii[1] << (7 - i));
      res &= (conv == *packed);
      ++ascii;
      ++packed;
    }

    if (!res)
      return false;

    ++ascii;
  }

  while (ascii < end) {
    if (*ascii++ != *packed++) {
      return false;
    }
  }

  return true;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

}  // namespace detail

}  // namespace dfly
