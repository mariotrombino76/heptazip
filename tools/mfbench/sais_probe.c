/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7) */
/* sais_probe.c -- costo di costruzione di un suffix array (docs/design/0005, candidato A)

   Implementazione diretta di SA-IS (Nong, Zhang, Chan 2009, induced sorting), senza
   ottimizzazioni particolari, per stimare il costo minimo di un match finder basato su
   suffix array rispetto ai ~240 ns/posizione di bt4.

     sais_probe <file> [maxBytes]

   Verifica anche che l'array sia ordinato (confronto dei suffissi adiacenti su un
   campione di posizioni).
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int32_t sidx;

static double Now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* text: s[0..n) with values in [0, k); s[n-1] must be the unique smallest (sentinel 0) */
#define GET_S(i) (cs == 1 ? (sidx)((const uint8_t *)s)[i] : ((const sidx *)s)[i])
#define TGET(i) ((t[(i) >> 3] >> ((i) & 7)) & 1)
#define TSET(i, b) { if (b) t[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)); else t[(i) >> 3] &= (uint8_t)~(1u << ((i) & 7)); }
#define IS_LMS(i) ((i) > 0 && TGET(i) && !TGET((i) - 1))

static void GetBuckets(const void *s, sidx *bkt, sidx n, sidx k, int cs, int end)
{
  sidx i, sum = 0;
  memset(bkt, 0, (size_t)k * sizeof(sidx));
  for (i = 0; i < n; i++)
    bkt[GET_S(i)]++;
  for (i = 0; i < k; i++)
  {
    sum += bkt[i];
    bkt[i] = end ? sum : sum - bkt[i];
  }
}

static void InduceL(const uint8_t *t, sidx *sa, const void *s, sidx *bkt, sidx n, sidx k, int cs)
{
  sidx i;
  GetBuckets(s, bkt, n, k, cs, 0);
  for (i = 0; i < n; i++)
  {
    const sidx j = sa[i] - 1;
    if (sa[i] > 0 && !TGET(j))
      sa[bkt[GET_S(j)]++] = j;
  }
}

static void InduceS(const uint8_t *t, sidx *sa, const void *s, sidx *bkt, sidx n, sidx k, int cs)
{
  sidx i;
  GetBuckets(s, bkt, n, k, cs, 1);
  for (i = n - 1; i >= 0; i--)
  {
    const sidx j = sa[i] - 1;
    if (sa[i] > 0 && TGET(j))
      sa[--bkt[GET_S(j)]] = j;
  }
}

static void Sais(const void *s, sidx *sa, sidx n, sidx k, int cs)
{
  uint8_t *t = (uint8_t *)calloc((size_t)n / 8 + 1, 1);
  sidx *bkt = (sidx *)malloc((size_t)k * sizeof(sidx));
  sidx i, j, n1, name, prev;
  sidx *s1, *sa1;

  // types: 1 = S, 0 = L
  TSET(n - 1, 1)
  TSET(n - 2, 0)
  for (i = n - 3; i >= 0; i--)
    TSET(i, (GET_S(i) < GET_S(i + 1) || (GET_S(i) == GET_S(i + 1) && TGET(i + 1))) ? 1 : 0)

  // stage 1: sort the LMS substrings
  GetBuckets(s, bkt, n, k, cs, 1);
  for (i = 0; i < n; i++)
    sa[i] = -1;
  for (i = 1; i < n; i++)
    if (IS_LMS(i))
      sa[--bkt[GET_S(i)]] = i;
  InduceL(t, sa, s, bkt, n, k, cs);
  InduceS(t, sa, s, bkt, n, k, cs);

  // compact the sorted LMS substrings into the first n1 items
  n1 = 0;
  for (i = 0; i < n; i++)
    if (IS_LMS(sa[i]))
      sa[n1++] = sa[i];

  // name the LMS substrings
  for (i = n1; i < n; i++)
    sa[i] = -1;
  name = 0;
  prev = -1;
  for (i = 0; i < n1; i++)
  {
    const sidx pos = sa[i];
    int diff = 0;
    sidx d;
    for (d = 0; d < n; d++)
    {
      if (prev == -1 || GET_S(pos + d) != GET_S(prev + d) || TGET(pos + d) != TGET(prev + d))
      {
        diff = 1;
        break;
      }
      if (d > 0 && (IS_LMS(pos + d) || IS_LMS(prev + d)))
        break;
    }
    if (diff)
    {
      name++;
      prev = pos;
    }
    sa[n1 + pos / 2] = name - 1;
  }
  for (i = n - 1, j = n - 1; i >= n1; i--)
    if (sa[i] >= 0)
      sa[j--] = sa[i];

  // stage 2: sort the reduced problem (recursion if names are not unique)
  s1 = sa + n - n1;
  sa1 = sa;
  if (name < n1)
    Sais(s1, sa1, n1, name, 4);
  else
    for (i = 0; i < n1; i++)
      sa1[s1[i]] = i;

  // stage 3: induce the final order from the sorted LMS suffixes
  GetBuckets(s, bkt, n, k, cs, 1);
  for (i = 1, j = 0; i < n; i++)
    if (IS_LMS(i))
      s1[j++] = i;
  for (i = 0; i < n1; i++)
    sa1[i] = s1[sa1[i]];
  for (i = n1; i < n; i++)
    sa[i] = -1;
  for (i = n1 - 1; i >= 0; i--)
  {
    j = sa[i];
    sa[i] = -1;
    sa[--bkt[GET_S(j)]] = j;
  }
  InduceL(t, sa, s, bkt, n, k, cs);
  InduceS(t, sa, s, bkt, n, k, cs);
  free(bkt);
  free(t);
}

int main(int argc, char **argv)
{
  FILE *f;
  size_t size, maxBytes = (size_t)-1;
  uint8_t *data;
  sidx *sa;
  double t0, dt;
  size_t i, bad = 0;

  if (argc < 2) { fprintf(stderr, "uso: sais_probe <file> [maxBytes]\n"); return 2; }
  if (argc > 2) maxBytes = (size_t)strtoull(argv[2], NULL, 10);
  f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  size = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size > maxBytes) size = maxBytes;
  data = (uint8_t *)malloc(size + 1);
  if (fread(data, 1, size, f) != size) return 1;
  fclose(f);

  /* alphabet 1..256 for the bytes, 0 for the sentinel */
  {
    sidx *text = (sidx *)malloc((size + 1) * sizeof(sidx));
    for (i = 0; i < size; i++) text[i] = (sidx)data[i] + 1;
    text[size] = 0;
    sa = (sidx *)malloc((size + 1) * sizeof(sidx));
    t0 = Now();
    Sais(text, sa, (sidx)(size + 1), 257, 4);
    dt = Now() - t0;
    free(text);
  }

  /* check the order on a sample of adjacent suffixes */
  for (i = 2; i <= size; i += 997)
  {
    const sidx a = sa[i - 1], b = sa[i];
    const size_t la = size - (size_t)a, lb = size - (size_t)b;
    const size_t m = la < lb ? la : lb;
    const int c = memcmp(data + a, data + b, m);
    if (c > 0 || (c == 0 && la > lb))
      bad++;
  }
  printf("size=%lu sais=%.3f s %.1f ns/byte %.1f MB/s  check: %lu errori\n",
      (unsigned long)size, dt, dt * 1e9 / (double)size, (double)size / dt / 1e6, (unsigned long)bad);
  free(sa);
  free(data);
  return bad != 0;
}
