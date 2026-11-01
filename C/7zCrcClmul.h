/* 7zCrcClmul.h -- CRC32 with PCLMULQDQ folding (x86-64)
Tensor7.

Implementation header: it's included by 7zCrc.c only (all functions are
static), so no build file of 7-Zip needs changes. tests/unit_crc_clmul.c
includes it too, to test exactly this code.

Algorithm: "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ
Instruction" (Intel, V. Gopal et al., 2009), for the bit-reflected CRC-32
polynomial 0x04C11DB7 (reflected 0xEDB88320) used by 7z, zip, gzip.
Constants (x^n mod P(x), bit-reflected, and Barrett mu) are the ones used by
the zlib / Chromium implementation of the same algorithm.

The state is the internal CRC state, as in CrcUpdate(): initial value
CRC_INIT_VAL, final XOR with CRC_INIT_VAL.

The folding function has a target attribute, so the binary runs on any
x86-64 CPU: the caller checks CrcClmul_IsSupported() first. */

#ifndef ZIP7_INC_7Z_CRC_CLMUL_H
#define ZIP7_INC_7Z_CRC_CLMUL_H

#include "7zTypes.h"
#include "CpuArch.h"

/* Z7_CRC_CLMUL is defined, if the compiler can build the PCLMULQDQ code
   (GCC >= 4.9, Clang >= 3.8, MSVC >= 2010) for x86-64. */
#if defined(MY_CPU_AMD64) && !defined(Z7_CRC_CLMUL_DISABLE) && \
    ( (defined(__GNUC__) && !defined(__clang__) && (__GNUC__ * 100 + __GNUC_MINOR__ >= 409)) \
    || (defined(__clang__) && (__clang_major__ * 100 + __clang_minor__ >= 308)) \
    || (defined(_MSC_VER) && _MSC_VER >= 1600) )
#define Z7_CRC_CLMUL
#endif

#ifdef Z7_CRC_CLMUL

#if defined(_MSC_VER)
#include <intrin.h>
#define Z7_CLMUL_ATTRIB
#else
#include <wmmintrin.h>
#include <smmintrin.h>
#define Z7_CLMUL_ATTRIB __attribute__((target("pclmul,sse4.1")))
#endif

/* Returns non-zero, if the CPU supports PCLMULQDQ and SSE4.1. */
static int CrcClmul_IsSupported(void)
{
  UInt32 a[4];
  z7_x86_cpuid(a, 0);
  if (a[0] < 1)
    return 0;
  z7_x86_cpuid(a, 1);
  /* CPUID.1:ECX bit 1 = PCLMULQDQ, bit 19 = SSE4.1 (_mm_extract_epi32) */
  return ((a[2] >> 1) & 1) && ((a[2] >> 19) & 1);
}

/* x^(4*128+32), x^(4*128-32) mod P : fold by 4 x 128 bits */
MY_ALIGN(16) static const UInt64 k_CrcClmul_Fold4[2] = { 0x0154442bd4, 0x01c6e41596 };
/* x^(128+32), x^(128-32) mod P : fold by 128 bits */
MY_ALIGN(16) static const UInt64 k_CrcClmul_Fold1[2] = { 0x01751997d0, 0x00ccaa009e };
/* x^64 mod P : 64 -> 32 bits */
MY_ALIGN(16) static const UInt64 k_CrcClmul_Fold64[2] = { 0x0163cd6124, 0 };
/* P(x) and Barrett constant mu = x^64 / P(x) */
MY_ALIGN(16) static const UInt64 k_CrcClmul_Poly[2] = { 0x01db710641, 0x01f7011641 };

#define Z7_CLMUL(a, b, imm)  _mm_clmulepi64_si128(a, b, imm)
#define Z7_CLMUL_LOADU(p)    _mm_loadu_si128((const __m128i *)(const void *)(p))
#define Z7_CLMUL_LOAD(p)     _mm_load_si128((const __m128i *)(const void *)(p))

/* Folds (size) bytes: size >= 64 and (size % 16 == 0). Returns the CRC state. */
Z7_CLMUL_ATTRIB
static UInt32 CrcClmul_Fold(UInt32 crc, const Byte *buf, size_t size)
{
  __m128i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;

  x1 = Z7_CLMUL_LOADU(buf + 0x00);
  x2 = Z7_CLMUL_LOADU(buf + 0x10);
  x3 = Z7_CLMUL_LOADU(buf + 0x20);
  x4 = Z7_CLMUL_LOADU(buf + 0x30);
  x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128((int)crc));
  x0 = Z7_CLMUL_LOAD(k_CrcClmul_Fold4);
  buf += 64;
  size -= 64;

  /* parallel folding of 4 x 128 bits */
  while (size >= 64)
  {
    x5 = Z7_CLMUL(x1, x0, 0x00);
    x6 = Z7_CLMUL(x2, x0, 0x00);
    x7 = Z7_CLMUL(x3, x0, 0x00);
    x8 = Z7_CLMUL(x4, x0, 0x00);
    x1 = Z7_CLMUL(x1, x0, 0x11);
    x2 = Z7_CLMUL(x2, x0, 0x11);
    x3 = Z7_CLMUL(x3, x0, 0x11);
    x4 = Z7_CLMUL(x4, x0, 0x11);
    y5 = Z7_CLMUL_LOADU(buf + 0x00);
    y6 = Z7_CLMUL_LOADU(buf + 0x10);
    y7 = Z7_CLMUL_LOADU(buf + 0x20);
    y8 = Z7_CLMUL_LOADU(buf + 0x30);
    x1 = _mm_xor_si128(_mm_xor_si128(x1, x5), y5);
    x2 = _mm_xor_si128(_mm_xor_si128(x2, x6), y6);
    x3 = _mm_xor_si128(_mm_xor_si128(x3, x7), y7);
    x4 = _mm_xor_si128(_mm_xor_si128(x4, x8), y8);
    buf += 64;
    size -= 64;
  }

  /* fold the 4 lanes into one 128-bit value */
  x0 = Z7_CLMUL_LOAD(k_CrcClmul_Fold1);
  x5 = Z7_CLMUL(x1, x0, 0x00);  x1 = Z7_CLMUL(x1, x0, 0x11);  x1 = _mm_xor_si128(_mm_xor_si128(x1, x2), x5);
  x5 = Z7_CLMUL(x1, x0, 0x00);  x1 = Z7_CLMUL(x1, x0, 0x11);  x1 = _mm_xor_si128(_mm_xor_si128(x1, x3), x5);
  x5 = Z7_CLMUL(x1, x0, 0x00);  x1 = Z7_CLMUL(x1, x0, 0x11);  x1 = _mm_xor_si128(_mm_xor_si128(x1, x4), x5);

  /* remaining 16-byte blocks */
  while (size >= 16)
  {
    x2 = Z7_CLMUL_LOADU(buf);
    x5 = Z7_CLMUL(x1, x0, 0x00);
    x1 = Z7_CLMUL(x1, x0, 0x11);
    x1 = _mm_xor_si128(_mm_xor_si128(x1, x2), x5);
    buf += 16;
    size -= 16;
  }

  /* 128 -> 64 bits */
  x2 = Z7_CLMUL(x1, x0, 0x10);
  x3 = _mm_setr_epi32(~0, 0, ~0, 0);
  x1 = _mm_srli_si128(x1, 8);
  x1 = _mm_xor_si128(x1, x2);

  x0 = _mm_loadl_epi64((const __m128i *)(const void *)k_CrcClmul_Fold64);
  x2 = _mm_srli_si128(x1, 4);
  x1 = _mm_and_si128(x1, x3);
  x1 = Z7_CLMUL(x1, x0, 0x00);
  x1 = _mm_xor_si128(x1, x2);

  /* Barrett reduction: 64 -> 32 bits */
  x0 = Z7_CLMUL_LOAD(k_CrcClmul_Poly);
  x2 = _mm_and_si128(x1, x3);
  x2 = Z7_CLMUL(x2, x0, 0x10);
  x2 = _mm_and_si128(x2, x3);
  x2 = Z7_CLMUL(x2, x0, 0x00);
  x1 = _mm_xor_si128(x1, x2);

  return (UInt32)_mm_extract_epi32(x1, 1);
}

/* Updates the CRC state. (tableFunc) processes the bytes that the folding
   code doesn't process: data shorter than 64 bytes and the tail (size % 16). */
typedef UInt32 (Z7_FASTCALL *Z7_CRC_CLMUL_TAIL_FUNC)(UInt32 v, const void *data, size_t size);

static UInt32 CrcClmul_Update(UInt32 v, const void *data, size_t size, Z7_CRC_CLMUL_TAIL_FUNC tableFunc)
{
  const Byte *p = (const Byte *)data;
  if (size >= 64)
  {
    const size_t foldSize = size & ~(size_t)15;
    v = CrcClmul_Fold(v, p, foldSize);
    p += foldSize;
    size -= foldSize;
  }
  if (size != 0)
    v = tableFunc(v, p, size);
  return v;
}

#endif /* Z7_CRC_CLMUL */

#endif
