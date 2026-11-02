/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)

   lztrace.c -- research tool (issue #16): compresses a file with LZMA (one thread)
   and writes the decisions of its optimal parser, to study other entropy coders on
   the same parse (lzstatic.py).

     lztrace <input> <tokens.bin> [level] [dictLog]

   tokens.bin: one record per decision, little endian:
     UInt16 len, UInt32 backRes   (0xFFFFFFFF literal, 0..3 rep index, dist + 4 match)
   The first byte of the input is always a literal and has no record.
   Also prints the size of the LZMA stream, for the comparison. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../C/Alloc.h"
#include "../../../C/LzmaEnc.h"

extern void (*g_LzmaTokenTrace)(unsigned len, UInt32 backRes);

static FILE *g_Out;
static unsigned long long g_NumTokens;

static void Trace(unsigned len, UInt32 backRes)
{
  unsigned char r[6];
  r[0] = (unsigned char)len; r[1] = (unsigned char)(len >> 8);
  r[2] = (unsigned char)backRes; r[3] = (unsigned char)(backRes >> 8);
  r[4] = (unsigned char)(backRes >> 16); r[5] = (unsigned char)(backRes >> 24);
  fwrite(r, 1, 6, g_Out);
  g_NumTokens++;
}

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(stderr, "lztrace <input> <tokens.bin> [level] [dictLog]\n");
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  fseek(f, 0, SEEK_END);
  const size_t size = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *in = (unsigned char *)malloc(size + 1);
  if (fread(in, 1, size, f) != size) { perror("read"); return 1; }
  fclose(f);

  g_Out = fopen(argv[2], "wb");
  if (!g_Out) { perror(argv[2]); return 1; }
  g_LzmaTokenTrace = Trace;

  CLzmaEncProps props;
  LzmaEncProps_Init(&props);
  props.level = argc > 3 ? atoi(argv[3]) : 9;
  if (argc > 4)
    props.dictSize = (UInt32)1 << atoi(argv[4]);
  props.numThreads = 1;
  props.reduceSize = size;
  LzmaEncProps_Normalize(&props);

  size_t outSize = size + size / 2 + (1 << 16);
  unsigned char *out = (unsigned char *)malloc(outSize);
  unsigned char propsEnc[LZMA_PROPS_SIZE];
  size_t propsSize = LZMA_PROPS_SIZE;
  const SRes res = LzmaEncode(out, &outSize, in, size, &props, propsEnc, &propsSize,
      0, NULL, &g_Alloc, &g_BigAlloc);
  fclose(g_Out);
  if (res != SZ_OK)
  {
    fprintf(stderr, "LzmaEncode error %d\n", res);
    return 1;
  }
  printf("input %zu  lzma %zu (%.3f%%)  tokens %llu  dict %u  lc %d lp %d pb %d fb %d\n",
      size, outSize, 100.0 * (double)outSize / (double)size, g_NumTokens,
      props.dictSize, props.lc, props.lp, props.pb, props.fb);
  return 0;
}
