/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)

   lzproto.c -- research prototype (issue #16): the LZMA parse coded with static
   rANS tables per block in separate streams, and a decoder, to measure the
   decompression speed of such a format. Not a file format: the tables stay in
   memory (their cost is estimated), and the decoder gets the per-block tables
   as normalized frequencies and builds its lookup tables itself (timed).

     lzproto <input> <tokens.bin>        (tokens from lztrace)

   Streams per block (contexts as lzstatic.py with LZS_PB=2):
     type   7 symbols, ctx = LZMA state * 4 + (pos & 3)        scale 10 bits
     lit    256, ctx = previous byte >> 5                        scale 12
     mlit   256, the literal XOR the byte at rep0, after a match scale 12
     len    272 (len - 2), ctx = pos & 3   (matches)            scale 12
     rlen   272 (len - 2), ctx = pos & 3   (rep matches)        scale 12
     slot   64, ctx = min(len - 2, 3)                           scale 12
     foot   32, ctx = slot - 4 (slots 4..13)                    scale 12
     align  16                                                  scale 12
     raw    the high extra bits of the distances (bit packer)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define BLOCK_LOG 18
#define BLOCK_SIZE ((size_t)1 << BLOCK_LOG)

enum { S_RUN, S_MTYPE, S_LIT, S_MLIT, S_LEN, S_RLEN, S_SLOT, S_FOOT, S_ALIGN, S_NUM };
static const unsigned kNumSym[S_NUM] = { 80, 6, 256, 256, 272, 272, 64, 32, 16 };
static const unsigned kNumCtx[S_NUM] = { 6, 12, 1, 1, 4, 4, 4, 10, 1 };
static const unsigned kScale[S_NUM] = { 11, 9, 11, 11, 11, 11, 11, 11, 11 };
/* interleaved rANS states: the literals are decoded in bulk before the LZ pass */
static const unsigned kNumStates[S_NUM] = { 1, 1, 4, 4, 1, 1, 1, 1, 1 };
#define MAX_STATES 4
#define MAX_SYM 272
#define MAX_CTX 12


/* match types of a sequence */
enum { M_MATCH, M_REP0, M_SHORTREP, M_REP1, M_REP2, M_REP3 };

/* run length code: 0..15 direct, then 2 codes per power of 2 (extra bits) */
static unsigned RunCode(uint32_t n, unsigned *nb, uint32_t *extra)
{
  if (n < 16) { *nb = 0; *extra = 0; return n; }
  const unsigned b = 31 - (unsigned)__builtin_clz(n);
  *nb = b - 1;
  *extra = n & ((1u << (b - 1)) - 1);
  return 16 + 2 * (b - 4) + ((n >> (b - 1)) & 1);
}

/* ---------------- rANS (byte renormalization, 32-bit state) ---------------- */
#define RANS_L (1u << 16)   /* 32-bit state, 16-bit renormalization */

typedef struct { uint16_t freq[MAX_CTX][MAX_SYM]; uint16_t cum[MAX_CTX][MAX_SYM + 1]; } Tables;

/* symbols of one stream in one block, in order */
typedef struct { uint16_t *ctx; uint16_t *sym; size_t n, cap; } SymList;

static void SymList_Add(SymList *l, unsigned ctx, unsigned sym)
{
  if (l->n == l->cap)
  {
    l->cap = l->cap ? l->cap * 2 : 1024;
    l->ctx = (uint16_t *)realloc(l->ctx, l->cap * 2);
    l->sym = (uint16_t *)realloc(l->sym, l->cap * 2);
  }
  l->ctx[l->n] = (uint16_t)ctx;
  l->sym[l->n] = (uint16_t)sym;
  l->n++;
}

/* normalize counts to sum (1 << scale), every used symbol >= 1 */
static void Normalize(const uint32_t *cnt, unsigned n, unsigned scale, uint16_t *freq)
{
  uint64_t tot = 0;
  unsigned i, used = 0;
  for (i = 0; i < n; i++) { tot += cnt[i]; used += cnt[i] != 0; }
  memset(freq, 0, n * 2);
  if (tot == 0)
    return;
  const uint32_t target = 1u << scale;
  int32_t sum = 0;
  unsigned maxi = 0;
  for (i = 0; i < n; i++)
    if (cnt[i])
    {
      uint32_t f = (uint32_t)(((uint64_t)cnt[i] * target) / tot);
      if (f == 0) f = 1;
      freq[i] = (uint16_t)f;
      sum += (int32_t)f;
      if (cnt[i] > cnt[maxi]) maxi = i;
    }
  /* fix the sum on the largest symbols */
  while (sum != (int32_t)target)
  {
    if (sum > (int32_t)target)
    {
      unsigned best = n;
      for (i = 0; i < n; i++)
        if (freq[i] > 1 && (best == n || freq[i] > freq[best])) best = i;
      freq[best]--; sum--;
    }
    else { freq[maxi]++; sum++; }
  }
  (void)used;
}

typedef struct
{
  /* per block */
  uint8_t *buf[S_NUM][MAX_STATES];  /* encoded substreams (forward order for the decoder) */
  size_t size[S_NUM][MAX_STATES];
  size_t num[S_NUM];        /* symbols in the block */
  uint16_t (*freq)[MAX_CTX][MAX_SYM];   /* [S_NUM] */
  uint32_t *raw;            /* raw bits, packed LSB first in 32-bit words */
  size_t rawWords;
  size_t outStart, outEnd;  /* bytes of the output in this block */
  size_t tokStart, tokEnd;
} Block;

static size_t EncodeStream(const SymList *l, unsigned s, uint16_t (*freq)[MAX_SYM], uint8_t **out)
{
  /* cumulative frequencies */
  static uint16_t cum[MAX_CTX][MAX_SYM + 1];
  const unsigned nsym = kNumSym[s], nctx = kNumCtx[s], scale = kScale[s];
  for (unsigned c = 0; c < nctx; c++)
  {
    cum[c][0] = 0;
    for (unsigned i = 0; i < nsym; i++)
      cum[c][i + 1] = (uint16_t)(cum[c][i] + freq[c][i]);
  }
  size_t cap = l->n * 4 + 16;
  uint8_t *buf = (uint8_t *)malloc(cap);
  uint8_t *p = buf + cap;
  uint32_t x = RANS_L;
  for (size_t k = l->n; k-- > 0;)
  {
    const unsigned c = l->ctx[k], sy = l->sym[k];
    const uint32_t f = freq[c][sy], start = cum[c][sy];
    const uint64_t xmax = ((uint64_t)(RANS_L >> scale) << 16) * f;
    if (x >= xmax) { p -= 2; p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); x >>= 16; }
    x = ((x / f) << scale) + (x % f) + start;
  }
  p -= 4;
  p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
  const size_t size = (size_t)(buf + cap - p);
  *out = (uint8_t *)malloc(size + 8);
  memcpy(*out, p, size);
  free(buf);
  return size;
}

static unsigned PosSlot(uint32_t d)
{
  if (d < 4) return d;
  unsigned n = 31 - (unsigned)__builtin_clz(d);
  return 2 * n + ((d >> (n - 1)) & 1);
}

/* ---------------- decoder ---------------- */
typedef struct { uint32_t x; const uint8_t *p; } RansDec;

static inline void RansDec_Init(RansDec *d, const uint8_t *p)
{
  d->x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  d->p = p + 4;
}

typedef uint32_t DEntry;   /* sym | freq << 9 | (index - start) << 21 */

/* decode tables of one stream: (1 << scale) entries per context */
static DEntry *g_Lut[S_NUM];
static uint8_t g_LitBuf[BLOCK_SIZE * 2], g_MLitBuf[BLOCK_SIZE * 2];

static void BuildLut(unsigned s, uint16_t (*freq)[MAX_SYM])
{
  const unsigned nsym = kNumSym[s], nctx = kNumCtx[s], scale = kScale[s];
  for (unsigned c = 0; c < nctx; c++)
  {
    DEntry *t = g_Lut[s] + ((size_t)c << scale);
    unsigned pos = 0;
    for (unsigned i = 0; i < nsym; i++)
    {
      const unsigned f = freq[c][i];
      for (unsigned j = 0; j < f; j++)
        t[pos + j] = (uint32_t)i | ((uint32_t)f << 9) | ((uint32_t)j << 21);
      pos += f;
    }
  }
}

#define DEC(d, s, c, out) \
  { const uint32_t e_ = g_Lut[s][((size_t)(c) << kScale[s]) + ((d).x & ((1u << kScale[s]) - 1))]; \
    (out) = e_ & 511; \
    uint32_t x_ = ((e_ >> 9) & 4095) * ((d).x >> kScale[s]) + (e_ >> 21); \
    const uint32_t y_ = (x_ << 16) | ((uint32_t)(d).p[0] | ((uint32_t)(d).p[1] << 8)); \
    const int r_ = x_ < RANS_L; \
    (d).x = r_ ? y_ : x_; \
    (d).p += r_ * 2; }

static double g_TTables, g_TLit, g_T2a;
static int g_AfterMatch;
static uint32_t g_LastDist;
#define MAX_SEQ (BLOCK_SIZE + 4096)
static uint32_t g_SeqRun[MAX_SEQ], g_SeqLen[MAX_SEQ], g_SeqDist[MAX_SEQ], g_SeqPrevDist[MAX_SEQ];
static double Now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}


typedef struct { uint32_t run; uint8_t type; uint16_t len; uint32_t dist; size_t pos; } Seq;

static void RawPut(uint32_t **raw, size_t *cap, size_t *bits, uint32_t v, unsigned nb)
{
  if ((*bits + nb + 64) / 32 >= *cap)
  {
    *raw = (uint32_t *)realloc(*raw, *cap * 8);
    memset(*raw + *cap, 0, *cap * 4);
    *cap *= 2;
  }
  for (unsigned k = 0; k < nb; k++, (*bits)++)
    (*raw)[*bits >> 5] |= ((v >> k) & 1u) << (*bits & 31);
}

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "lzproto <input> <tokens.bin>\n"); return 2; }
  FILE *f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END); const size_t size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *data = (uint8_t *)malloc(size);
  if (fread(data, 1, size, f) != size) return 1;
  fclose(f);
  f = fopen(argv[2], "rb");
  fseek(f, 0, SEEK_END); const size_t ntok = (size_t)ftell(f) / 6; fseek(f, 0, SEEK_SET);
  uint8_t *tokRaw = (uint8_t *)malloc(ntok * 6);
  if (fread(tokRaw, 1, ntok * 6, f) != ntok * 6) return 1;
  fclose(f);

  /* --- the tokens as sequences: literal run + match (the last one: run only) --- */
  Seq *seqs = (Seq *)malloc(sizeof(Seq) * (ntok + 2));
  size_t nseq = 0;
  {
    size_t pos = 1;
    uint32_t run = 1, runStart = 0;
    uint32_t reps[4] = { 1, 1, 1, 1 };
    for (size_t t = 0; t < ntok; t++)
    {
      const uint8_t *r = tokRaw + t * 6;
      const unsigned len = r[0] | (r[1] << 8);
      const uint32_t back = (uint32_t)r[2] | ((uint32_t)r[3] << 8) | ((uint32_t)r[4] << 16) | ((uint32_t)r[5] << 24);
      if (back == 0xFFFFFFFF) { run++; pos++; continue; }
      Seq *q = &seqs[nseq++];
      q->run = run; q->pos = runStart; q->len = (uint16_t)len;
      if (back < 4)
      {
        if (back == 0) q->type = (len == 1) ? M_SHORTREP : M_REP0;
        else
        {
          q->type = (uint8_t)(M_REP1 + back - 1);   /* back 1..3 */
          const uint32_t d = reps[back];
          for (unsigned k = back; k > 0; k--) reps[k] = reps[k - 1];
          reps[0] = d;
        }
        q->dist = reps[0];
      }
      else
      {
        q->type = M_MATCH;
        q->dist = back - 4 + 1;
        reps[3] = reps[2]; reps[2] = reps[1]; reps[1] = reps[0]; reps[0] = q->dist;
      }
      pos += len;
      run = 0;
      runStart = (uint32_t)pos;
    }
    Seq *q = &seqs[nseq++];
    q->run = run; q->pos = runStart; q->type = 255; q->len = 0; q->dist = 0;
    if (pos != size) { fprintf(stderr, "bad tokens\n"); return 1; }
  }

  /* --- blocks: a block starts with a sequence at or after (blockStart + BLOCK_SIZE) --- */
  const size_t maxBlocks = size / BLOCK_SIZE + 2;
  Block *blocks = (Block *)calloc(maxBlocks, sizeof(Block));
  size_t nblocks = 0;
  SymList lists[S_NUM];
  static uint32_t counts[MAX_CTX][MAX_SYM];
  double tableBits = 0;
  size_t total = 0;
  size_t si = 0, outPos = 0;
  while (si < nseq)
  {
    Block *bl = &blocks[nblocks++];
    const size_t blockEnd = outPos + BLOCK_SIZE;
    memset(lists, 0, sizeof(lists));
    size_t rawCap = 1024, rawBits = 0;
    uint32_t *raw = (uint32_t *)calloc(rawCap, 4);
    bl->outStart = outPos;
    unsigned prevType = 0;
    for (; si < nseq && outPos < blockEnd; si++)
    {
      const Seq *q = &seqs[si];
      unsigned nb; uint32_t extra;
      SymList_Add(&lists[S_RUN], prevType, RunCode(q->run, &nb, &extra));
      RawPut(&raw, &rawCap, &rawBits, extra, nb);
      for (uint32_t k = 0; k < q->run; k++)
      {
        const size_t p = outPos + k;
        if (k == 0 && p != 0 && si != 0)
          SymList_Add(&lists[S_MLIT], 0, data[p] ^ data[p - seqs[si - 1].dist]);
        else
          SymList_Add(&lists[S_LIT], 0, data[p]);
      }
      outPos += q->run;
      if (q->type == 255)
        break;
      SymList_Add(&lists[S_MTYPE], prevType * 2 + (q->run == 0), q->type);
      const unsigned posState = (unsigned)(outPos & 3);
      if (q->type == M_MATCH)
      {
        SymList_Add(&lists[S_LEN], posState, q->len - 2);
        const uint32_t dist = q->dist - 1;
        const unsigned slot = PosSlot(dist);
        SymList_Add(&lists[S_SLOT], q->len - 2 < 3 ? q->len - 2 : 3, slot);
        if (slot >= 4)
        {
          const unsigned fb = (slot >> 1) - 1;
          const uint32_t ex = dist - ((2u | (slot & 1)) << fb);
          if (slot < 14)
            SymList_Add(&lists[S_FOOT], slot - 4, ex);
          else
          {
            RawPut(&raw, &rawCap, &rawBits, ex >> 4, fb - 4);
            SymList_Add(&lists[S_ALIGN], 0, ex & 15);
          }
        }
      }
      else if (q->type != M_SHORTREP)
        SymList_Add(&lists[S_RLEN], posState, q->len - 2);
      outPos += q->len;
      prevType = q->type;
    }
    if (si < nseq && seqs[si].type == 255 && outPos == size) si++;
    bl->outEnd = outPos;
    bl->raw = raw;
    bl->rawWords = (rawBits + 31) / 32 + 2;
    total += (rawBits + 7) / 8;
    bl->freq = (uint16_t (*)[MAX_CTX][MAX_SYM])calloc(S_NUM, sizeof(uint16_t[MAX_CTX][MAX_SYM]));
    for (unsigned s = 0; s < S_NUM; s++)
    {
      memset(counts, 0, sizeof(counts));
      for (size_t k = 0; k < lists[s].n; k++)
        counts[lists[s].ctx[k]][lists[s].sym[k]]++;
      for (unsigned c = 0; c < kNumCtx[s]; c++)
      {
        Normalize(counts[c], kNumSym[s], kScale[s], bl->freq[s][c]);
        unsigned used = 0;
        for (unsigned i = 0; i < kNumSym[s]; i++) used += counts[c][i] != 0;
        if (used) tableBits += 5.0 * used + 8;
      }
      bl->num[s] = lists[s].n;
      for (unsigned j = 0; j < kNumStates[s]; j++)
      {
        SymList sub;
        memset(&sub, 0, sizeof(sub));
        for (size_t k = j; k < lists[s].n; k += kNumStates[s])
          SymList_Add(&sub, lists[s].ctx[k], lists[s].sym[k]);
        bl->size[s][j] = sub.n ? EncodeStream(&sub, s, bl->freq[s], &bl->buf[s][j]) : 0;
        total += bl->size[s][j];
        free(sub.ctx); free(sub.sym);
      }
      free(lists[s].ctx); free(lists[s].sym);
    }
  }
  const size_t totalWithTables = total + (size_t)(tableBits / 8);
  printf("input %zu  sequences %zu  blocks %zu (%zu KiB)  streams %zu + tables ~%zu = %zu (%.3f%%)\n",
      size, nseq, nblocks, BLOCK_SIZE >> 10, total, (size_t)(tableBits / 8), totalWithTables,
      100.0 * (double)totalWithTables / (double)size);

  /* --- decode, several times, keep the best --- */
  uint8_t *out = (uint8_t *)malloc(size + 64 + 16);
  for (unsigned s = 0; s < S_NUM; s++)
    g_Lut[s] = (DEntry *)malloc(sizeof(DEntry) * ((size_t)kNumCtx[s] << kScale[s]));
  double best = 1e9;
  for (int rep = 0; rep < 5; rep++)
  {
    const double t0 = Now();
    size_t op = 0;
    uint32_t rp[4] = { 1, 1, 1, 1 };
    g_AfterMatch = 0;
    for (size_t b = 0; b < nblocks; b++)
    {
      const Block *bl = &blocks[b];
      RansDec d[S_NUM];
      const double tb0 = Now();
      for (unsigned s = 0; s < S_NUM; s++)
        if (bl->num[s])
        {
          BuildLut(s, bl->freq[s]);
          if (kNumStates[s] == 1)
            RansDec_Init(&d[s], bl->buf[s][0]);
        }
      const double tb1 = Now();
      g_TTables += tb1 - tb0;
      /* phase 1: the literals of the block, 4 interleaved states */
      for (unsigned s = S_LIT; s <= S_MLIT; s++)
      {
        const size_t n = bl->num[s];
        uint8_t *dst = (s == S_LIT) ? g_LitBuf : g_MLitBuf;
        RansDec q[MAX_STATES];
        for (unsigned j = 0; j < MAX_STATES; j++)
          if (bl->size[s][j]) RansDec_Init(&q[j], bl->buf[s][j]);
        size_t k = 0;
        for (; k + 4 <= n; k += 4)
        {
          unsigned a0, a1, a2, a3;
          DEC(q[0], s, 0, a0)
          DEC(q[1], s, 0, a1)
          DEC(q[2], s, 0, a2)
          DEC(q[3], s, 0, a3)
          dst[k] = (uint8_t)a0; dst[k + 1] = (uint8_t)a1; dst[k + 2] = (uint8_t)a2; dst[k + 3] = (uint8_t)a3;
        }
        for (; k < n; k++)
        {
          unsigned a;
          DEC(q[k & 3], s, 0, a)
          dst[k] = (uint8_t)a;
        }
      }
      const uint8_t *lit = g_LitBuf, *mlit = g_MLitBuf;
      const double tb2 = Now();
      g_TLit += tb2 - tb1;
      /* phase 2: the sequences */
      const uint32_t *raw = bl->raw;
      size_t rb = 0;
      unsigned prevType = 0;
      const size_t end = bl->outEnd;
      #define RAW_GET(v, nb) { const uint64_t w_ = (uint64_t)raw[rb >> 5] | ((uint64_t)raw[(rb >> 5) + 1] << 32); \
          (v) = (uint32_t)(w_ >> (rb & 31)) & (uint32_t)((1ull << (nb)) - 1); rb += (nb); }
      /* phase 2a: the sequences of the block into arrays (no access to the output) */
      size_t ns = 0;
      {
        size_t opv = op;
        while (opv < end)
        {
          unsigned code;
          DEC(d[S_RUN], S_RUN, prevType, code)
          uint32_t run = code;
          if (code >= 16)
          {
            const unsigned b2 = (code - 16) / 2 + 4;
            uint32_t ex;
            RAW_GET(ex, b2 - 1)
            run = ((2u | ((code - 16) & 1)) << (b2 - 1)) + ex;
          }
          opv += run;
          g_SeqRun[ns] = run;
          if (opv >= size)
          {
            g_SeqLen[ns] = 0;
            g_SeqDist[ns] = rp[0];
            ns++;
            break;
          }
          unsigned type;
          DEC(d[S_MTYPE], S_MTYPE, prevType * 2 + (run == 0), type)
          unsigned len;
          const unsigned posState = (unsigned)(opv & 3);
          if (type == M_MATCH)
          {
            DEC(d[S_LEN], S_LEN, posState, len)
            len += 2;
            unsigned slot;
            DEC(d[S_SLOT], S_SLOT, len - 2 < 3 ? len - 2 : 3, slot)
            uint32_t dd = slot;
            if (slot >= 4)
            {
              const unsigned fb = (slot >> 1) - 1;
              dd = (2u | (slot & 1)) << fb;
              if (slot < 14)
              {
                unsigned e;
                DEC(d[S_FOOT], S_FOOT, slot - 4, e)
                dd += e;
              }
              else
              {
                uint32_t v;
                RAW_GET(v, fb - 4)
                unsigned a;
                DEC(d[S_ALIGN], S_ALIGN, 0, a)
                dd += (v << 4) + a;
              }
            }
            g_SeqPrevDist[ns] = rp[0];
            rp[3] = rp[2]; rp[2] = rp[1]; rp[1] = rp[0]; rp[0] = dd + 1;
          }
          else
          {
            g_SeqPrevDist[ns] = rp[0];
            if (type == M_SHORTREP)
              len = 1;
            else
            {
              if (type != M_REP0)
              {
                const unsigned k = type - M_REP1 + 1;   /* rep index 1..3 */
                const uint32_t dd = rp[k];
                for (unsigned j = k; j > 0; j--) rp[j] = rp[j - 1];
                rp[0] = dd;
              }
              DEC(d[S_RLEN], S_RLEN, posState, len)
              len += 2;
            }
          }
          g_SeqLen[ns] = len;
          g_SeqDist[ns] = rp[0];
          ns++;
          opv += len;
          prevType = type;
        }
      }
      const double tb3 = Now();
      g_T2a += tb3 - tb2;
      /* phase 2b: literals and copies; the source of the match PF sequences ahead is
         prefetched (the far matches wait for the memory) */
      {
        const size_t PF = 8;
        size_t pfPos = op;
        for (size_t i = 0; i < ns && i < PF; i++)
          pfPos += g_SeqRun[i] + g_SeqLen[i];
        for (size_t i = 0; i < ns; i++)
        {
          if (i + PF < ns)
          {
            pfPos += g_SeqRun[i + PF];
            __builtin_prefetch(out + pfPos - g_SeqDist[i + PF]);
            pfPos += g_SeqLen[i + PF];
          }
          const uint32_t run = g_SeqRun[i];
          if (run)
          {
            uint32_t k = 0;
            if (g_AfterMatch)
            {
              /* the first literal after a match: XOR the byte at rep0 before the match */
              out[op] = (uint8_t)(*mlit++ ^ out[op - g_LastDist]);
              op++;
              k = 1;
            }
            if (k < run)
            {
              const uint8_t *src = lit;
              uint8_t *dst = out + op;
              const uint32_t n = run - k;
              uint32_t j = 0;
              do { memcpy(dst + j, src + j, 16); j += 16; } while (j < n);
              lit += n;
              op += n;
            }
          }
          const uint32_t len = g_SeqLen[i];
          if (len == 0)
            break;
          const uint32_t dist = g_SeqDist[i];
          const uint8_t *src = out + op - dist;
          uint8_t *dst = out + op;
          if (dist >= 16)
          {
            size_t k = 0;
            do { memcpy(dst + k, src + k, 16); k += 16; } while (k < len);
          }
          else if (dist >= 8)
          {
            size_t k = 0;
            do { uint64_t v; memcpy(&v, src + k, 8); memcpy(dst + k, &v, 8); k += 8; } while (k < len);
          }
          else
            for (unsigned k = 0; k < len; k++) dst[k] = src[k];
          op += len;
          g_LastDist = dist;
          g_AfterMatch = 1;
        }
      }
    }
    const double dt = Now() - t0;
    if (dt < best) best = dt;
    if (op != size || memcmp(out, data, size) != 0)
    {
      size_t k = 0;
      while (k < size && out[k] == data[k]) k++;
      printf("DECODE MISMATCH at %zu (op %zu)\n", k, op);
      return 1;
    }
  }
  printf("decode ok: %.3f s, %.1f MB/s (one core); of 5 runs: tables %.3f s, literals %.3f s, sequences %.3f s\n",
      best, (double)size / best / 1e6, g_TTables / 5, g_TLit / 5, g_T2a / 5);
  return 0;
}
