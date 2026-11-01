/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7) */
/* LzFindMatchLen.h -- length of a match for the match finders
   Tensor7: internal header of LzFind.c and LzFindOpt.c (docs/design/0004). */

#ifndef ZIP7_INC_LZ_FIND_MATCH_LEN_H
#define ZIP7_INC_LZ_FIND_MATCH_LEN_H

#include "CpuArch.h"

/* Compares 8 bytes at a time: XOR of two 64-bit loads, and the index of the
   lowest non-zero byte of the XOR (count of trailing zero bits / 8) is the
   first difference. Little endian only. */
#if defined(MY_CPU_LE_UNALIGN_64) && (defined(__GNUC__) || defined(__clang__))
  #define Z7_LZ_MATCH_LEN_WORDS
  #define Z7_LZ_CTZ64(x)  ((unsigned)__builtin_ctzll(x))
#elif defined(MY_CPU_LE_UNALIGN_64) && defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
  #include <intrin.h>
  #define Z7_LZ_MATCH_LEN_WORDS
  static Z7_FORCE_INLINE unsigned Z7_LZ_CTZ64(UInt64 x)
  {
    unsigned long index;
    _BitScanForward64(&index, x);
    return (unsigned)index;
  }
#endif

/* Returns the first position (p <= result <= lim) where p[i] != p[i + diff],
   or (lim) if the bytes are equal up to (lim): the same result as the byte loop
   of upstream
     for (; p != lim; p++) if (p[diff] != *p) break;
   It never reads at or beyond (lim) (and (p + diff) .. (lim + diff)): the 8-byte
   loads are done only while 8 bytes remain. Unlike the byte loop, it can read up
   to 7 bytes after the first difference, always before (lim).
   LZMA2 -mx5: 2.6% faster with 1 thread, 7.7% with 20 threads (docs/design/0004). */
static Z7_FORCE_INLINE const Byte *LzFind_MatchEnd(const Byte *p, const Byte *lim, ptrdiff_t diff)
{
#ifdef Z7_LZ_MATCH_LEN_WORDS
  while ((size_t)(lim - p) >= 8)
  {
    const UInt64 x = Z7_LoadNative64(p) ^ Z7_LoadNative64(p + diff);
    if (x != 0)
      return p + (Z7_LZ_CTZ64(x) >> 3);
    p += 8;
  }
#endif
  for (; p != lim; p++)
    if (p[diff] != *p)
      break;
  return p;
}

#endif
