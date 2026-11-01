/* unit_float_plane.c -- test del formato a blocchi FLOAT (C/FloatPlane.c, design 0008)
 *
 * - round-trip su dati sintetici: pesi gaussiani in BF16, FP16, FP32, con fasi 0..3
 *   (byte prima del primo elemento), valori costanti, byte casuali, dimensioni da 1 byte
 *   al blocco massimo;
 * - il tipo e la fase scelti dall'encoder sono quelli giusti, e il blocco si riduce
 *   (BF16 gaussiano: sotto il 70%);
 * - blocchi non float (maschere, costanti, testo): zstd (mode 2), con il frame verificato;
 * - ogni blocco troncato da' SZ_ERROR_INPUT_EOF; ogni byte corrotto (uno alla volta, su
 *   blocchi piccoli) da' un errore oppure un blocco decodificato, mai un accesso fuori
 *   dai limiti (da eseguire con ASan).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../C/CpuArch.h"
#include "../C/FloatPlaneLz.h"

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

enum { kBf16, kFp16, kFp32, kConst, kRandom, kBf16Flat };

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
    else if (kind == kBf16Flat && i + 2 <= size)
    {
      /* esponente gaussiano, mantissa uniforme: i bit alti della mantissa non dicono
         nulla, l'encoder deve scegliere k = 0 */
      const UInt16 v = (UInt16)((ToBf16(Gauss(0.02f)) & 0xFF80) | (Rnd32() & 0x7F));
      p[i] = (Byte)v; p[i + 1] = (Byte)(v >> 8); i += 2;
    }
    else if (kind == kConst)
      p[i++] = 0x3C;
    else
      p[i++] = (Byte)Rnd32();
  }
}

static Byte *g_Src, *g_Enc, *g_Dec;
static CFloatPlaneZstd g_Zstd;   /* zstd reale (mode 2), se l'encoder e' compilato */
#define LZ (&g_Zstd.Lz)

/* encode + decode di un blocco; restituisce la dimensione codificata (0 se errore) */
static size_t RoundTrip(size_t size)
{
  size_t encLen, used = 0, outLen = 0;
  SRes res;
  encLen = FloatPlane_EncodeBlock(g_Src, size, g_Enc, LZ);
  CHECK(encLen <= FLOAT_PLANE_ENCODED_MAX(size));
  CHECK(FloatPlane_GetBlockOutLen(g_Enc, encLen, &outLen) == SZ_OK && outLen == size);
  memset(g_Dec, 0xAA, size);
  res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, size, &used, LZ);
  CHECK(res == SZ_OK);
  CHECK(used == encLen);
  CHECK(memcmp(g_Src, g_Dec, size) == 0);
  /* buffer di uscita troppo piccolo: errore, non overflow */
  if (size > 1)
  {
    res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, size - 1, &used, LZ);
    CHECK(res == SZ_ERROR_DATA);
  }
  return res == SZ_OK || size > 1 ? encLen : 0;
}

static void Test_RoundTrips(void)
{
  static const size_t sizes[] = { 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 100, 1001, 4096,
      65535, 65536, 65537, 300001, FLOAT_PLANE_BLOCK_DEFAULT, FLOAT_PLANE_BLOCK_MAX };
  int kind;
  for (kind = kBf16; kind <= kBf16Flat; kind++)
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
  printf("  dati %s fase %u: tipo %u, k %u, %.2f%%\n",
      kind == kBf16 ? "BF16" : kind == kFp16 ? "FP16" : kind == kFp32 ? "FP32" : "BF16 mantissa uniforme",
      phase, g_Enc[5], g_Enc[7], 100.0 * (double)encLen / (double)size);
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
    encLen = FloatPlane_EncodeBlock(g_Src, size, g_Enc, LZ);
    for (pos = 0; pos < encLen; pos++)
    {
      static const Byte deltas[] = { 1, 0x80, 0xFF };
      unsigned d;
      for (d = 0; d < 3; d++)
      {
        size_t used;
        SRes res;
        g_Enc[pos] ^= deltas[d];
        res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used, LZ);
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
      const SRes res = FloatPlane_DecodeBlock(g_Enc, pos, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used, LZ);
      CHECK(res == SZ_ERROR_INPUT_EOF || res == SZ_ERROR_DATA);
    }
  }
}

/* blocchi che non sono float: zstd (mode 2) se l'encoder zstd c'e' */
static void Test_Lz(void)
{
  size_t size = (size_t)1 << 20, encLen, used, i, j;
  SRes res;
  if (!g_Zstd.Lz.Compress)
  {
    printf("  encoder zstd non compilato: test del mode 2 saltati\n");
    return;
  }
  /* maschera triangolare 1024 x 1024 di 0/1, come nei modelli */
  for (i = 0; i < 1024; i++)
    for (j = 0; j < 1024; j++)
      g_Src[i * 1024 + j] = (Byte)(j <= i);
  encLen = RoundTrip(size);
  CHECK(g_Enc[0] == 2);
  CHECK(encLen < size / 100);
  printf("  maschera triangolare: mode %u, %.3f%%\n", g_Enc[0], 100.0 * (double)encLen / (double)size);
  /* senza decoder zstd: non supportato */
  res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, size, &used, NULL);
  CHECK(res == SZ_ERROR_UNSUPPORTED);
  /* il frame con un byte in piu' (lzSize + 1) non e' accettato */
  {
    const UInt32 lzSize = GetUi32(g_Enc + 5);
    g_Enc[encLen] = 0;
    SetUi32(g_Enc + 5, lzSize + 1)
    res = FloatPlane_DecodeBlock(g_Enc, encLen + 1, g_Dec, size, &used, LZ);
    CHECK(res == SZ_ERROR_DATA);
    SetUi32(g_Enc + 5, lzSize)
  }
  /* costante */
  memset(g_Src, 0, size);
  RoundTrip(size);
  CHECK(g_Enc[0] == 2);
  /* pesi gaussiani: resta split anche con zstd disponibile */
  Fill(g_Src, size, kBf16, 0);
  RoundTrip(size);
  CHECK(g_Enc[0] == 1);
  /* testo ripetitivo dopo un'intestazione (metadati GGUF, JSON) */
  {
    static const char kText[] = "\"dtype\":\"BF16\",\"shape\":[1024,896]},";
    for (i = 0; i < size; i++)
      g_Src[i] = (Byte)kText[i % (sizeof(kText) - 1)];
  }
  RoundTrip(size);
  CHECK(g_Enc[0] == 2);
  /* corruzione di blocchi mode 2 piccoli: ogni byte */
  for (i = 0; i < 3000; i++)
    g_Src[i] = (Byte)(i % 7 == 0 ? i : 0);
  encLen = FloatPlane_EncodeBlock(g_Src, 3000, g_Enc, LZ);
  CHECK(g_Enc[0] == 2);
  for (i = 0; i < encLen; i++)
  {
    static const Byte deltas[] = { 1, 0x80, 0xFF };
    unsigned d;
    for (d = 0; d < 3; d++)
    {
      g_Enc[i] ^= deltas[d];
      res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used, LZ);
      CHECK(res == SZ_OK || res == SZ_ERROR_DATA || res == SZ_ERROR_INPUT_EOF);
      g_Enc[i] ^= deltas[d];
    }
  }
  for (i = 0; i < encLen; i++)
  {
    res = FloatPlane_DecodeBlock(g_Enc, i, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used, LZ);
    CHECK(res == SZ_ERROR_INPUT_EOF || res == SZ_ERROR_DATA);
  }
}

/* ---------- blocchi quantizzati (mode 3) ---------- */

/* valore quantizzato a "campana" in 0 .. max */
static unsigned QValue(unsigned max)
{
  const int v = (int)(max / 2) + (int)(Gauss((float)max / 6.0f) + 0.5f);
  return v < 0 ? 0 : v > (int)max ? max : (unsigned)v;
}

static void PutFp16(Byte *p, float f)
{
  const UInt16 v = ToFp16(f);
  p[0] = (Byte)v;
  p[1] = (Byte)(v >> 8);
}

/* una struttura GGUF del tipo dato, con la disposizione di ggml-common.h */
enum { kQ4_0, kQ8_0, kQ4_K, kQ5_K, kQ6_K };
static const unsigned kQSize[] = { 18, 34, 144, 176, 210 };

static void FillQStruct(Byte *p, unsigned qtype)
{
  unsigned v;
  memset(p, 0, kQSize[qtype]);
  switch (qtype)
  {
    case kQ4_0:
      PutFp16(p, 0.001f + 0.002f * (float)(Rnd32() & 255) / 256.0f);
      for (v = 0; v < 32; v++)
        p[2 + (v & 15)] |= (Byte)(QValue(15) << (4 * (v >> 4)));
      break;
    case kQ8_0:
      PutFp16(p, 0.0002f + 0.0004f * (float)(Rnd32() & 255) / 256.0f);
      for (v = 0; v < 32; v++)
        p[2 + v] = (Byte)(Int32)(Gauss(30.0f));
      break;
    case kQ4_K:
    case kQ5_K:
      PutFp16(p, 0.001f + 0.002f * (float)(Rnd32() & 255) / 256.0f);
      PutFp16(p + 2, 0.002f + 0.002f * (float)(Rnd32() & 255) / 256.0f);
      for (v = 0; v < 12; v++)
        p[4 + v] = (Byte)(QValue(63) | (Rnd32() & 3) << 6);
      for (v = 0; v < 256; v++)
      {
        const unsigned c = v >> 6, h = (v >> 5) & 1, l = v & 31;
        if (qtype == kQ4_K)   /* qs[32c + l]: valori 64c + l e 64c + 32 + l */
          p[16 + 32 * c + l] |= (Byte)(QValue(15) << (4 * h));
        else
        {
          const unsigned q = QValue(31);
          p[48 + 32 * c + l] |= (Byte)((q & 15) << (4 * h));
          p[16 + l] |= (Byte)((q >> 4) << (2 * c + h));
        }
      }
      break;
    default:   /* Q6_K */
      for (v = 0; v < 256; v++)
      {
        const unsigned h = v >> 7, c = (v >> 5) & 3, l = v & 31;
        const unsigned q = QValue(63);
        p[64 * h + 32 * (c & 1) + l] |= (Byte)((q & 15) << (4 * (c >> 1)));
        p[128 + 32 * h + l] |= (Byte)((q >> 4) << (2 * c));
      }
      for (v = 0; v < 16; v++)
        p[192 + v] = (Byte)(Int32)Gauss(20.0f);
      PutFp16(p + 208, 0.0005f + 0.001f * (float)(Rnd32() & 255) / 256.0f);
      break;
  }
}

/* phase byte casuali, strutture, coda casuale (meno di una struttura) */
static size_t FillQuant(Byte *p, size_t numStructs, unsigned qtype, unsigned phase, unsigned tail)
{
  const unsigned S = kQSize[qtype];
  size_t i, size = phase;
  for (i = 0; i < phase; i++)
    p[i] = (Byte)Rnd32();
  for (i = 0; i < numStructs; i++, size += S)
    FillQStruct(p + size, qtype);
  for (i = 0; i < tail; i++)
    p[size++] = (Byte)Rnd32();
  return size;
}

static void Test_Quant(void)
{
  static const char * const names[] = { "Q4_0", "Q8_0", "Q4_K", "Q5_K", "Q6_K" };
  unsigned qtype;
  for (qtype = 0; qtype < 5; qtype++)
  {
    const unsigned S = kQSize[qtype];
    const unsigned phases[] = { 0, 1, S / 2, S - 1 };
    const size_t counts[] = { 16 * 1024 / S, 300, FLOAT_PLANE_BLOCK_DEFAULT / S,
        FLOAT_PLANE_BLOCK_MAX / S - 1 };
    unsigned pi, ci;
    for (pi = 0; pi < 4; pi++)
      for (ci = 0; ci < 4; ci++)
      {
        const unsigned tail = (pi * 37 + ci) % S;
        size_t size, encLen, blockSize = 0;
        if (phases[pi] + counts[ci] * S + tail > FLOAT_PLANE_BLOCK_MAX)
          continue;
        size = FillQuant(g_Src, counts[ci], qtype, phases[pi], tail);
        encLen = RoundTrip(size);
        CHECK(FloatPlane_GetBlockSize(g_Enc, encLen, &blockSize) == SZ_OK && blockSize == encLen);
        CHECK(encLen < size);
        /* nei blocchi piccoli (qualche KB) le tabelle dei campi (~430-600 byte) possono
           far vincere zstd */
        if (size < ((size_t)1 << 16))
          continue;
        CHECK(g_Enc[0] == 3);
        CHECK(g_Enc[5] == qtype);
        CHECK(g_Enc[6] == phases[pi]);
        if (ci == 2 && pi == 1)
          printf("  %s fase %u: %.2f%%\n", names[qtype], phases[pi], 100.0 * (double)encLen / (double)size);
      }
  }
}

/* blocchi quantizzati piccoli: ogni byte corrotto in 3 modi, tutti i prefissi */
static void Test_QuantCorruption(void)
{
  unsigned qtype;
  for (qtype = 0; qtype < 5; qtype++)
  {
    /* 16 KiB: le tabelle di Huffman dei campi (fino a ~600 byte) devono rendere */
    const size_t size = FillQuant(g_Src, 16 * 1024 / kQSize[qtype], qtype, 3, 5);
    size_t encLen, pos, used;
    SRes res;
    encLen = FloatPlane_EncodeBlock(g_Src, size, g_Enc, LZ);
    CHECK(g_Enc[0] == 3);
    /* ogni byte dell'intestazione, poi un byte ogni 7 dei flussi */
    for (pos = 0; pos < encLen; pos += pos < 1100 ? 1 : 7)
    {
      static const Byte deltas[] = { 1, 0x80, 0xFF };
      unsigned d;
      for (d = 0; d < 3; d++)
      {
        g_Enc[pos] ^= deltas[d];
        res = FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used, LZ);
        CHECK(res == SZ_OK || res == SZ_ERROR_DATA || res == SZ_ERROR_INPUT_EOF);
        if (res == SZ_OK)
          CHECK(used <= encLen);
        g_Enc[pos] ^= deltas[d];
      }
    }
    for (pos = 0; pos < encLen; pos++)
    {
      res = FloatPlane_DecodeBlock(g_Enc, pos, g_Dec, FLOAT_PLANE_BLOCK_MAX, &used, LZ);
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
  FloatPlaneZstd_Construct(&g_Zstd);

  /* tutti i round-trip con il decoder scalare e con quello SIMD (se la CPU lo ha) */
  FloatPlane_SetSimd(False);
  Test_RoundTrips();
  Test_Quant();
  FloatPlane_SetSimd(True);
  Test_RoundTrips();
  printf("blocchi quantizzati (mode 3), 1 MiB:\n");
  Test_Quant();
  Test_QuantCorruption();
  printf("scelta di tipo e fase, blocco da 1 MiB:\n");
  Test_Choice(kBf16, 0, 0, 0.70);
  Test_Choice(kBf16, 1, 0, 0.70);
  Test_Choice(kFp16, 0, 1, 0.85);
  Test_Choice(kFp16, 1, 1, 0.85);
  Test_Choice(kFp32, 0, 2, 0.85);
  Test_Choice(kFp32, 3, 2, 0.85);
  /* k: i bit alti della mantissa nel simbolo solo se servono */
  Test_Choice(kBf16, 0, 0, 0.70);
  CHECK(g_Enc[7] > 0);
  Test_Choice(kBf16Flat, 1, 0, 0.70);
  CHECK(g_Enc[7] == 0);
  Test_Corruption();
  printf("blocchi non float (zstd):\n");
  Test_Lz();
  FloatPlaneZstd_Free(&g_Zstd);

  free(g_Src);
  free(g_Enc);
  free(g_Dec);
  printf("unit_float_plane: %u controlli, %u falliti\n", g_NumChecks, g_NumFailures);
  return g_NumFailures == 0 ? 0 : 1;
}
