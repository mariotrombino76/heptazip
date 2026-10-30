/* unit_crc_clmul.c -- test di C/7zCrcClmul.h (CRC32 con PCLMULQDQ)

Confronta il CRC32 calcolato con il folding PCLMULQDQ con un CRC32 di
riferimento a tabella semplice, scritto qui e indipendente da 7-Zip, su:
- tutte le lunghezze 0..4096 con 16 disallineamenti diversi;
- aggiornamenti concatenati spezzati in punti arbitrari;
- un buffer grande (anche misura della velocita').
Exit code: 0 = ok (o CPU senza PCLMULQDQ: SKIP), 1 = errore. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../C/7zCrcClmul.h"

#ifndef Z7_CRC_CLMUL

int main(void)
{
  printf("SKIP: compilatore o architettura senza supporto PCLMULQDQ\n");
  return 0;
}

#else

static UInt32 g_RefTable[256];

static void RefInit(void)
{
  UInt32 i;
  for (i = 0; i < 256; i++)
  {
    UInt32 r = i;
    int j;
    for (j = 0; j < 8; j++)
      r = (r >> 1) ^ (0xEDB88320 & (0u - (r & 1)));
    g_RefTable[i] = r;
  }
}

static UInt32 Z7_FASTCALL RefUpdate(UInt32 v, const void *data, size_t size)
{
  const Byte *p = (const Byte *)data;
  for (; size != 0; size--)
    v = g_RefTable[(v ^ *p++) & 0xFF] ^ (v >> 8);
  return v;
}

static UInt64 g_Rand = 0x9E3779B97F4A7C15ull;
static Byte RandByte(void)
{
  g_Rand = g_Rand * 6364136223846793005ull + 1442695040888963407ull;
  return (Byte)(g_Rand >> 56);
}

static double Now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
  unsigned numErrors = 0, numChecks = 0;
  size_t len, offset, i;
  const size_t kMaxLen = 4096;
  Byte *buf;

  if (!CrcClmul_IsSupported())
  {
    printf("SKIP: CPU senza PCLMULQDQ/SSE4.1\n");
    return 0;
  }
  RefInit();

  /* valore noto: CRC32("123456789") = 0xCBF43926 (con lunghezza >= 64 per il folding) */
  {
    const char *s = "123456789";
    Byte data[9 * 8];
    for (i = 0; i < 8; i++)
      memcpy(data + i * 9, s, 9);
    numChecks++;
    if (RefUpdate(0xFFFFFFFF, s, 9) != (0xCBF43926 ^ 0xFFFFFFFF))
    {
      printf("FAIL: il riferimento non da' il valore noto\n");
      numErrors++;
    }
    numChecks++;
    if (CrcClmul_Update(0xFFFFFFFF, data, sizeof(data), RefUpdate) != RefUpdate(0xFFFFFFFF, data, sizeof(data)))
    {
      printf("FAIL: valore noto 8 x \"123456789\"\n");
      numErrors++;
    }
  }

  buf = (Byte *)malloc(kMaxLen + 64);
  for (i = 0; i < kMaxLen + 64; i++)
    buf[i] = RandByte();

  /* tutte le lunghezze e 16 disallineamenti, con stato iniziale variabile */
  for (offset = 0; offset < 16; offset++)
    for (len = 0; len <= kMaxLen; len++)
    {
      const UInt32 init = (len & 1) ? 0xFFFFFFFF : (UInt32)(len * 2654435761u);
      const UInt32 ref = RefUpdate(init, buf + offset, len);
      const UInt32 got = CrcClmul_Update(init, buf + offset, len, RefUpdate);
      numChecks++;
      if (ref != got)
      {
        if (numErrors < 10)
          printf("FAIL: len=%u offset=%u ref=%08X clmul=%08X\n", (unsigned)len, (unsigned)offset, (unsigned)ref, (unsigned)got);
        numErrors++;
      }
    }

  /* aggiornamenti concatenati: il risultato non deve dipendere dai punti di divisione */
  for (i = 0; i < 2000; i++)
  {
    const size_t total = 1 + (RandByte() | ((size_t)RandByte() << 8)) % kMaxLen;
    const size_t cut = (RandByte() | ((size_t)RandByte() << 8)) % (total + 1);
    UInt32 v = CrcClmul_Update(0xFFFFFFFF, buf, cut, RefUpdate);
    v = CrcClmul_Update(v, buf + cut, total - cut, RefUpdate);
    numChecks++;
    if (v != RefUpdate(0xFFFFFFFF, buf, total))
    {
      if (numErrors < 10)
        printf("FAIL: concatenato total=%u cut=%u\n", (unsigned)total, (unsigned)cut);
      numErrors++;
    }
  }
  free(buf);

  /* buffer grande + velocita' */
  {
    const size_t kBig = (size_t)64 << 20;
    Byte *big = (Byte *)malloc(kBig);
    double t0, t1, t2;
    UInt32 a, b;
    for (i = 0; i < kBig; i++)
      big[i] = RandByte();
    t0 = Now();
    a = RefUpdate(0xFFFFFFFF, big, kBig);
    t1 = Now();
    b = CrcClmul_Update(0xFFFFFFFF, big, kBig, RefUpdate);
    t2 = Now();
    numChecks++;
    if (a != b)
    {
      printf("FAIL: buffer da 64 MiB\n");
      numErrors++;
    }
    printf("velocita': riferimento byte-wise %.0f MiB/s, PCLMULQDQ %.0f MiB/s\n",
        64.0 / (t1 - t0), 64.0 / (t2 - t1));
    free(big);
  }

  printf("%u controlli, %u falliti\n", numChecks, numErrors);
  return numErrors == 0 ? 0 : 1;
}

#endif
