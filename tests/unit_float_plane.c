/* unit_float_plane.c -- test del formato a blocchi FLOAT (C/FloatPlane.c, design 0008)
 *
 * - round-trip su dati sintetici: pesi gaussiani in BF16, FP16, FP32, con fasi 0..3
 *   (byte prima del primo elemento), valori costanti, byte casuali, dimensioni da 1 byte
 *   al blocco massimo;
 * - il tipo e la fase scelti dall'encoder sono quelli giusti, e il blocco si riduce
 *   (BF16 gaussiano: sotto il 70%);
 * - ogni blocco troncato da' SZ_ERROR_INPUT_EOF; ogni byte corrotto (uno alla volta, su
 *   blocchi piccoli) da' un errore oppure un blocco decodificato, mai un accesso fuori
 *   dai limiti (da eseguire con ASan).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../C/FloatPlane.h"

static unsigned g_NumChecks, g_NumFailures;

#define CHECK(cond) \
  do { g_NumChecks++; if (!(cond)) { g_NumFailures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static UInt64 g_Rnd = 0x9E3779B97F4A7C15;
static UInt32 Rnd32(void)
{
  g_Rnd = g_Rnd * 6364136223846793005ULL + 1442695040888963407ULL;
  return (UInt32)(g_Rnd >> 32);
}

/* normale (Box-Muller), deviazione standard sd */
static float Gauss(float sd)
{
  const double u1 = ((double)Rnd32() + 1.0) / 4294967297.0;
  const double u2 = (double)Rnd32() / 4294967296.0;
  return (float)(sd * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2));
}

static UInt32 FloatBits(float f) { UInt32 v; memcpy(&v, &f, 4); return v; }

static UInt16 ToBf16(float f)
{
  const UInt32 v = FloatBits(f);
  return (UInt16)((v + 0x7FFF + ((v >> 16) & 1)) >> 16);   /* round to nearest even */
}

static UInt16 ToFp16(float f)   /* semplice: troncamento, niente subnormali */
{
  const UInt32 v = FloatBits(f);
  const UInt32 sign = (v >> 16) & 0x8000;
  const int exp = (int)((v >> 23) & 0xFF) - 127 + 15;
  if (exp <= 0)
    return (UInt16)sign;
  if (exp >= 31)
    return (UInt16)(sign | 0x7C00);
  return (UInt16)(sign | ((UInt32)exp << 10) | ((v >> 13) & 0x3FF));
}

enum { kBf16, kFp16, kFp32, kConst, kRandom };

/* size byte: phase byte casuali, poi elementi del tipo, poi coda casuale */
static void Fill(Byte *p, size_t size, int kind, unsigned phase)
{
  size_t i = 0;
  for (; i < phase && i < size; i++)
    p[i] = (Byte)Rnd32();
  while (i < size)
  {
    if (kind == kBf16 && i + 2 <= size)
    {
      const UInt16 v = ToBf16(Gauss(0.02f));
      p[i] = (Byte)v; p[i + 1] = (Byte)(v >> 8); i += 2;
    }
    else if (kind == kFp16 && i + 2 <= size)
    {
      const UInt16 v = ToFp16(Gauss(0.02f));
      p[i] = (Byte)v; p[i + 1] = (Byte)(v >> 8); i += 2;
    }
    else if (kind == kFp32 && i + 4 <= size)
    {
      const UInt32 v = FloatBits(Gauss(0.02f));
      memcpy(p + i, &v, 4); i += 4;
    }
    else if (kind == kConst)
      p[i++] = 0x3C;
    else
      p[i++] = (Byte)Rnd32();
  }
}

static Byte *g_Src, *g_Enc, *g_Dec;

/* encode + decode di un blocco; restituisce la dimensione codificata (0 se errore) */
static size_t RoundTrip(size_t size)
{
  size_t encLen, used = 0, outLen = 0;
  SRes res;
  encLen = FloatPlane_EncodeBlock(g_Src, size, g_Enc);
  CHECK(encLen <= FLOAT_PLANE_ENCODED_MAX(size));
  CHECK(FloatPlane_GetBlockOutLen(g_Enc, encLen, &outLen) == SZ_OK && outLen == size);
  memset(g_Dec, 0xAA, size);
  res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, size, &used);
  CHECK(res == SZ_OK);
  CHECK(used == encLen);
  CHECK(memcmp(g_Src, g_Dec, size) == 0);
  /* buffer di uscita troppo piccolo: errore, non overflow */
  if (size > 1)
  {
    res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, size - 1, &used);
    CHECK(res == SZ_ERROR_DATA);
  }
  return res == SZ_OK || size > 1 ? encLen : 0;
}

static void Test_RoundTrips(void)
{
  static const size_t sizes[] = { 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1001, 4096,
      65535, 65536, 65537, 300001, FLOAT_PLANE_BLOCK_DEFAULT, FLOAT_PLANE_BLOCK_MAX };
  int kind;
  for (kind = kBf16; kind <= kRandom; kind++)
  {
    unsigned phase;
    for (phase = 0; phase < 4; phase++)
    {
      unsigned i;
      for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
      {
        Fill(g_Src, sizes[i], kind, phase);
        RoundTrip(sizes[i]);
      }
    }
  }
}

/* il blocco deve essere in modo split, della fase attesa, di un tipo con la dimensione
   dell'elemento attesa, e ridursi. Il tipo esatto non e' imposto: per dati FP16 la
   separazione BF16 (bit 7-14: esponente e 3 bit di mantissa, senza segno) puo' costare
   quanto quella FP16 (bit 8-15) e l'encoder sceglie in base al costo misurato. */
static void Test_Choice(int kind, unsigned phase, unsigned expType, double maxRatio)
{
  const size_t size = FLOAT_PLANE_BLOCK_DEFAULT + phase;
  size_t encLen;
  Fill(g_Src, size, kind, phase);
  encLen = RoundTrip(size);
  CHECK(g_Enc[0] == 1);
  CHECK(expType == 2 ? g_Enc[5] == 2 : g_Enc[5] <= 1);
  CHECK(g_Enc[6] == phase);
  CHECK((double)encLen < maxRatio * (double)size);
  printf("  dati %s fase %u: tipo scelto %u, %.2f%%\n", kind == kBf16 ? "BF16" : kind == kFp16 ? "FP16" : "FP32",
      phase, g_Enc[5], 100.0 * (double)encLen / (double)size);
}

static void Test_Corruption(void)
{
  /* blocchi piccoli: ogni byte corrotto, uno alla volta, in 3 modi */
  static const int kinds[] = { kBf16, kFp32, kRandom };
  unsigned k;
  for (k = 0; k < 3; k++)
  {
    const size_t size = 777;
    size_t encLen, pos;
    Fill(g_Src, size, kinds[k], 1);
    encLen = FloatPlane_EncodeBlock(g_Src, size, g_Enc);
    for (pos = 0; pos < encLen; pos++)
    {
      static const Byte deltas[] = { 1, 0x80, 0xFF };
      unsigned d;
      for (d = 0; d < 3; d++)
      {
        size_t used;
        SRes res;
        g_Enc[pos] ^= deltas[d];
        res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used);
        CHECK(res == SZ_OK || res == SZ_ERROR_DATA || res == SZ_ERROR_INPUT_EOF);
        if (res == SZ_OK)
          CHECK(used <= encLen);
        g_Enc[pos] ^= deltas[d];
      }
    }
    /* troncamenti: tutti i prefissi */
    for (pos = 0; pos < encLen; pos++)
    {
      size_t used;
      const SRes res = FloatPlane_DecodeBlock(g_Enc, pos, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used);
      CHECK(res == SZ_ERROR_INPUT_EOF || res == SZ_ERROR_DATA);
    }
  }
}

int main(void)
{
  g_Src = (Byte *)malloc(FLOAT_PLANE_BLOCK_MAX + 8);
  g_Enc = (Byte *)malloc(FLOAT_PLANE_ENCODED_MAX(FLOAT_PLANE_BLOCK_MAX + 8));
  g_Dec = (Byte *)malloc(FLOAT_PLANE_BLOCK_MAX + 8);
  if (!g_Src || !g_Enc || !g_Dec)
    return 2;

  Test_RoundTrips();
  printf("scelta di tipo e fase, blocco da 1 MiB:\n");
  Test_Choice(kBf16, 0, 0, 0.70);
  Test_Choice(kBf16, 1, 0, 0.70);
  Test_Choice(kFp16, 0, 1, 0.85);
  Test_Choice(kFp16, 1, 1, 0.85);
  Test_Choice(kFp32, 0, 2, 0.85);
  Test_Choice(kFp32, 3, 2, 0.85);
  Test_Corruption();

  free(g_Src);
  free(g_Enc);
  free(g_Dec);
  printf("unit_float_plane: %u controlli, %u falliti\n", g_NumChecks, g_NumFailures);
  return g_NumFailures == 0 ? 0 : 1;
}
