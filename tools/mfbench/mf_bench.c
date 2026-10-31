/* mf_bench.c -- banco di prova dei match finder LZMA (docs/design/0005)

   Misura il SOLO match finder, configurato come lo configura LzmaEnc.c, su un file
   letto in memoria (input diretto): tempo per posizione e statistiche dei match.

     mf_bench <file> [mf=bt4] [d=24] [fb=32] [mc=0] [rep=3] [threads=1]

   mf: bt2 bt3 bt4 bt5 hc4 hc5; d: log2 del dizionario; fb: numFastBytes (lenLimit);
   mc: cutValue (0: default di LzmaEnc, 16 + fb/2 per bt, 8 + fb/4 per hc).

   Schema di chiamata come il parser ottimo dell'encoder: GetMatches() a ogni
   posizione; se il match piu' lungo arriva a fb, Skip(len - 1) (in LzmaEnc il parser
   salta il resto di un match lungo). Le statistiche servono a confrontare un match
   finder nuovo con bt4 a parita' di chiamate:
     - pairs/pos: coppie (lunghezza, distanza) per posizione;
     - longest: lunghezza media del match piu' lungo;
     - checksum: somma pesata delle coppie (due match finder che trovano gli stessi
       match danno lo stesso valore).
*/

#include "../../C/Precomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../C/Alloc.h"
#include "../../C/LzFind.h"

#define LZMA_MATCH_LEN_MAX 273
#define kNumOpts (1 << 11)

static double Now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
  const char *mf = "bt4";
  unsigned dictLog = 24, fb = 32, mc = 0, rep = 3;
  int i;
  FILE *f;
  Byte *data;
  size_t size;

  if (argc < 2)
  {
    fprintf(stderr, "uso: mf_bench <file> [mf=bt4] [d=24] [fb=32] [mc=0] [rep=3]\n");
    return 2;
  }
  for (i = 2; i < argc; i++)
  {
    const char *a = argv[i];
    if (strncmp(a, "mf=", 3) == 0) mf = a + 3;
    else if (strncmp(a, "d=", 2) == 0) dictLog = (unsigned)atoi(a + 2);
    else if (strncmp(a, "fb=", 3) == 0) fb = (unsigned)atoi(a + 3);
    else if (strncmp(a, "mc=", 3) == 0) mc = (unsigned)atoi(a + 3);
    else if (strncmp(a, "rep=", 4) == 0) rep = (unsigned)atoi(a + 4);
    else { fprintf(stderr, "opzione sconosciuta: %s\n", a); return 2; }
  }
  if (strlen(mf) != 3 || (mf[0] != 'b' && mf[0] != 'h') || mf[2] < '2' || mf[2] > '5'
      || (mf[0] == 'h' && mf[2] < '4') || fb < 5 || fb > LZMA_MATCH_LEN_MAX || dictLog < 12 || dictLog > 30)
  {
    fprintf(stderr, "parametri non validi\n");
    return 2;
  }

  f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "impossibile aprire %s\n", argv[1]); return 1; }
  fseek(f, 0, SEEK_END);
  size = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  data = (Byte *)malloc(size + 1);
  if (!data || fread(data, 1, size, f) != size) { fprintf(stderr, "lettura fallita\n"); return 1; }
  fclose(f);

  LzFindPrepare();

  {
    const int btMode = (mf[0] == 'b');
    const unsigned numHashBytes = (unsigned)(mf[2] - '0');
    const UInt32 dictSize = (UInt32)1 << dictLog;
    unsigned r;
    double best = 1e30;
    UInt64 numPos = 0, numPairs = 0, sumLongest = 0, checksum = 0;

    if (mc == 0)
      mc = btMode ? 16 + (fb >> 1) : 8 + (fb >> 2);  // LzmaEncProps_Normalize()

    for (r = 0; r < rep; r++)
    {
      CMatchFinder p;
      IMatchFinder2 vt;
      UInt32 *dist = (UInt32 *)malloc((LZMA_MATCH_LEN_MAX * 2 + 2) * sizeof(UInt32));
      double t0, dt;

      MatchFinder_Construct(&p);
      p.btMode = (Byte)(btMode ? 1 : 0);
      p.numHashBytes = numHashBytes;
      p.cutValue = mc;
      p.bigHash = (Byte)(dictSize > ((UInt32)1 << 24) ? 1 : 0);
      MatchFinder_SET_DIRECT_INPUT_BUF(&p, data, size)
      if (!dist || !MatchFinder_Create(&p, dictSize, kNumOpts, fb, LZMA_MATCH_LEN_MAX + 1, &g_BigAlloc))
      {
        fprintf(stderr, "memoria insufficiente\n");
        return 1;
      }
      MatchFinder_CreateVTable(&p, &vt);

      numPos = numPairs = sumLongest = checksum = 0;
      t0 = Now();
      vt.Init(&p);
      for (;;)
      {
        const UInt32 avail = vt.GetNumAvailableBytes(&p);
        const UInt32 *end;
        UInt32 n;
        if (avail == 0)
          break;
        end = vt.GetMatches(&p, dist);
        n = (UInt32)(end - dist) / 2;
        numPos++;
        numPairs += n;
        if (n != 0)
        {
          const UInt32 longest = end[-2];
          UInt32 k;
          sumLongest += longest;
          for (k = 0; k < n; k++)
            checksum = checksum * 31 + dist[k * 2] * 65599u + dist[k * 2 + 1];
          if (longest >= fb && longest > 1)
          {
            UInt32 skip = longest - 1;
            const UInt32 avail2 = vt.GetNumAvailableBytes(&p);
            if (skip > avail2)
              skip = avail2;
            if (skip != 0)
              vt.Skip(&p, skip);
          }
        }
      }
      dt = Now() - t0;
      if (dt < best)
        best = dt;
      MatchFinder_Free(&p, &g_BigAlloc);
      free(dist);
    }

    printf("mf=%s d=%u fb=%u mc=%u size=%lu: %.3f s %.1f MB/s %.1f ns/pos  "
        "pos=%llu pairs/pos=%.3f longest=%.3f checksum=%016llx\n",
        mf, dictLog, fb, mc, (unsigned long)size, best, (double)size / best / 1e6,
        best * 1e9 / (double)numPos, (unsigned long long)numPos,
        (double)numPairs / (double)numPos, (double)sumLongest / (double)numPos,
        (unsigned long long)checksum);
  }
  free(data);
  return 0;
}
