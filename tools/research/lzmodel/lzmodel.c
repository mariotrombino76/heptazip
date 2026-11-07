/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
   lzmodel.c -- research tool (strada 2, "LZMA esteso"): replays the decisions of the
   LZMA parser (tokens from tools/research/lzstatic/lztrace.c) through a copy of the
   LZMA probability model and sums the ideal cost (-log2 p) of every coded bit, with
   the model varied:
     - the adaptive counter of every binary context (LZMA: shift 5);
     - the context model of the literals (LZMA: lc high bits of the previous byte).
   The baseline must reproduce the LZMA stream size printed by lztrace (within the
   range coder's rounding): that validates the replay. The decoder cost of a variant is
   estimated separately (design 0018).

     lzmodel <input> <tokens.bin> [-c shift5|dual|cnt[:limit]] [-l lc3|lc8|mix2|mix3]
             [-lc N] [-pb N] [-a]
     -c  counter: shift5 = LZMA; dual = two probabilities (shift 4 and 7) averaged;
         cnt = rate 1/(n+2) until n reaches limit (default 30), then fixed
     -l  literals: lc3 = LZMA tree with lc bits of the previous byte; lc8 = the whole
         previous byte; mix2 = logistic mix of lc3 and lc8 predictions; mix3 = mix2
         plus an order-2 hashed context
     -a  APM (secondary estimation) on the isMatch bits, context = state and the two
         previous bytes' high bits
     -d  distance slot context extended with the slot of the previous match (8 buckets)
   Output: bits per category (literals, match/rep flags, lengths, distances) and the
   total in bytes, next to the size of LZMA's own coding. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t Byte;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;

/* ---------------------------------------------------------------- constants (LzmaEnc.c) */
#define kNumStates 12
#define kNumPosStatesMax 16
#define kNumLenToPosStates 4
#define kNumPosSlotBits 6
#define kNumAlignBits 4
#define kStartPosModelIndex 4
#define kEndPosModelIndex 14
#define kNumFullDistances (1 << (kEndPosModelIndex >> 1))
#define kLenNumLowBits 3
#define kLenNumLowSymbols (1 << kLenNumLowBits)
#define kLenNumHighBits 8
#define kLenNumHighSymbols (1 << kLenNumHighBits)
#define kMatchMinLen 2
#define kNumReps 4
#define MARK_LIT 0xFFFFFFFFu

static const Byte kLiteralNextStates[kNumStates] = {0, 0, 0, 0, 1, 2, 3, 4,  5,  6,   4, 5};
static const Byte kMatchNextStates[kNumStates]   = {7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10};
static const Byte kRepNextStates[kNumStates]     = {8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11};
static const Byte kShortRepNextStates[kNumStates]= {9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11};
#define IsLitState(s) ((s) < 7)
#define GetLenToPosState(len) (((len) < kNumLenToPosStates + 1) ? (len) - 2 : kNumLenToPosStates - 1)

/* ---------------------------------------------------------------- the counter */
enum { C_SHIFT5, C_DUAL, C_CNT };
static int g_counter = C_SHIFT5;
static unsigned g_cntLimit = 30;

/* a binary context: 16-bit probability of bit 0 (LZMA uses 11 bits: the replay uses
   the same shift on 16 bits, which changes the rounding only) */
typedef struct { UInt16 p; UInt16 p2; UInt16 n; UInt16 pad; } CProb;

static double g_cost0[65536]; /* -log2(p / 65536) */
static UInt32 g_rec[256];     /* 65536 / (n + 2) */

static void Prob_Init(CProb *c)
{
  c->p = 1 << 15; c->p2 = 1 << 15; c->n = 0;
}

static inline unsigned Prob_Get(const CProb *c) /* probability of bit 0, 16 bits, never 0 or 65536 */
{
  unsigned p = (g_counter == C_DUAL) ? ((unsigned)c->p + c->p2) >> 1 : c->p;
  if (p < 32) p = 32;
  if (p > 65536 - 32) p = 65536 - 32;
  return p;
}

static inline void Prob_Update(CProb *c, unsigned bit)
{
  const int target = bit ? 0 : 65535;
  switch (g_counter)
  {
    case C_SHIFT5:
      c->p = (UInt16)(c->p + ((target - (int)c->p) >> 5));
      break;
    case C_DUAL:
      c->p  = (UInt16)(c->p  + ((target - (int)c->p)  >> 4));
      c->p2 = (UInt16)(c->p2 + ((target - (int)c->p2) >> 7));
      break;
    case C_CNT:
    {
      const int d = ((target - (int)c->p) * (int)g_rec[c->n]) >> 16;
      c->p = (UInt16)(c->p + d);
      if (c->n < g_cntLimit) c->n++;
      break;
    }
  }
}

/* cost of coding (bit) with the context, then the update */
static double g_bitsLit, g_bitsFlags, g_bitsLen, g_bitsDist;
static UInt64 g_numLit, g_numMatch, g_numRep, g_numShortRep;

static inline double CodeBit(CProb *c, unsigned bit)
{
  const unsigned p0 = Prob_Get(c);
  const double cost = bit ? g_cost0[65536 - p0] : g_cost0[p0];
  Prob_Update(c, bit);
  return cost;
}

/* ---------------------------------------------------------------- the literal models */
enum { L_LC3, L_LC8, L_MIX2, L_MIX3 };
static int g_litModel = L_LC3;
static unsigned g_lc = 3;

static CProb *g_litLc;    /* 0x300 << lc : LZMA's table */
static CProb *g_litO1;    /* 0x300 << 8  : the whole previous byte */
static CProb *g_litO2;    /* 0x300 << 16 : hash of the two previous bytes */
#define kO2Bits 16

/* logistic mixer: one weight set per tree node (0x300) and per model input */
#define kMixInputs 3
static double *g_mixW;    /* 0x300 * kMixInputs * 2 (plain / matched) */
static double g_mixLr = 0.02;

static inline double Stretch(double p) { return log(p / (1.0 - p)); }
static inline double Squash(double x) { return 1.0 / (1.0 + exp(-x)); }

/* the literal tree: LZMA codes the 8 bits with a binary tree of 0x100 nodes (plain) or
   with the "matched" scheme of 0x300 nodes (after a match: the byte at rep0 selects
   the sub-tree until the first mismatch). (ctxLc, ctxO1, ctxO2): the index of the
   table of each model. */
static double CodeLiteral(unsigned sym, unsigned matchByte, int matched,
    unsigned ctxLc, unsigned ctxO1, unsigned ctxO2)
{
  double bits = 0;
  unsigned offs = 0x100;
  unsigned s = sym | 0x100;
  unsigned mb = matchByte;
  CProb *tLc = g_litLc + (size_t)0x300 * ctxLc;
  CProb *tO1 = g_litO1 ? g_litO1 + (size_t)0x300 * ctxO1 : NULL;
  CProb *tO2 = g_litO2 ? g_litO2 + (size_t)0x300 * ctxO2 : NULL;
  do
  {
    unsigned node;
    const unsigned bit = (s >> 7) & 1;
    if (matched)
    {
      mb <<= 1;
      node = offs + (mb & offs) + (s >> 8);
    }
    else
      node = s >> 8;
    s <<= 1;
    if (matched)
      offs &= ~(mb ^ s);

    if (g_litModel == L_LC3)
      bits += CodeBit(tLc + node, bit);
    else if (g_litModel == L_LC8)
      bits += CodeBit(tO1 + node, bit);
    else
    {
      /* the mix: inputs in the logistic domain, weights per node */
      const unsigned nIn = (g_litModel == L_MIX3) ? 3 : 2;
      double st[kMixInputs], dot = 0, p;
      double *w = g_mixW + ((size_t)(matched ? 0x300 : 0) + node) * kMixInputs;
      CProb *in[kMixInputs];
      unsigned i;
      in[0] = tLc + node; in[1] = tO1 + node; in[2] = tO2 ? tO2 + node : NULL;
      for (i = 0; i < nIn; i++)
      {
        st[i] = Stretch((double)Prob_Get(in[i]) / 65536.0); /* of bit 0 */
        dot += w[i] * st[i];
      }
      p = Squash(dot); /* probability of bit 0 */
      if (p < 1e-6) p = 1e-6;
      if (p > 1 - 1e-6) p = 1 - 1e-6;
      bits += bit ? -log2(1 - p) : -log2(p);
      {
        const double err = (bit ? 0.0 : 1.0) - p;
        for (i = 0; i < nIn; i++)
        {
          w[i] += g_mixLr * err * st[i];
          Prob_Update(in[i], bit);
        }
      }
    }
  }
  while (s < 0x10000);
  return bits;
}

/* ---------------------------------------------------------------- APM on isMatch */
static int g_apm = 0;
/* 33 interpolation points per context; context = state * 256 + (b1 >> 4) * 16 + (b2 >> 4) */
#define kApmCtx (kNumStates * 256)
static double *g_apmT; /* kApmCtx * 33 probabilities of bit 0 */
static const double kApmRate = 0.02;

static void Apm_Init(void)
{
  unsigned i, j;
  g_apmT = (double *)malloc(sizeof(double) * kApmCtx * 33);
  for (i = 0; i < kApmCtx; i++)
    for (j = 0; j < 33; j++)
      g_apmT[i * 33 + j] = Squash(((double)j - 16) / 2.0);
}

/* the input probability p0 (bit 0) refined by the APM; returns the cost and updates */
static double CodeBit_Apm(CProb *c, unsigned bit, unsigned ctx)
{
  const unsigned p16 = Prob_Get(c);
  const double st = Stretch((double)p16 / 65536.0); /* -8..8 */
  double x = (st + 8.0) * 2.0;
  int lo;
  double frac, p, pa, cost;
  if (x < 0) x = 0;
  if (x > 31.999) x = 31.999;
  lo = (int)x;
  frac = x - lo;
  {
    double *t = g_apmT + (size_t)ctx * 33;
    pa = t[lo] * (1 - frac) + t[lo + 1] * frac;
    p = (pa + (double)p16 / 65536.0) * 0.5; /* average with the input, as lpaq */
    if (p < 1e-6) p = 1e-6;
    if (p > 1 - 1e-6) p = 1 - 1e-6;
    cost = bit ? -log2(1 - p) : -log2(p);
    {
      const double target = bit ? 0.0 : 1.0;
      t[lo] += kApmRate * (1 - frac) * (target - t[lo]);
      t[lo + 1] += kApmRate * frac * (target - t[lo + 1]);
    }
  }
  Prob_Update(c, bit);
  return cost;
}

/* ---------------------------------------------------------------- the model */
typedef struct
{
  CProb low[kNumPosStatesMax << (kLenNumLowBits + 1)];
  CProb high[kLenNumHighSymbols];
} CLenProbs;

static CProb isMatch[kNumStates][kNumPosStatesMax];
static CProb isRep[kNumStates], isRepG0[kNumStates], isRepG1[kNumStates], isRepG2[kNumStates];
static CProb isRep0Long[kNumStates][kNumPosStatesMax];
/* -d: the slot context is extended with the slot of the previous match (8 buckets) */
static int g_distCtx = 0;
static unsigned g_prevSlotBucket = 0;
static CProb posSlot[kNumLenToPosStates * 8][1 << kNumPosSlotBits];
static CProb posEncoders[kNumFullDistances];
static CProb posAlign[1 << kNumAlignBits];
static CLenProbs lenProbs, repLenProbs;

static void InitProbs(CProb *a, size_t n) { size_t i; for (i = 0; i < n; i++) Prob_Init(a + i); }

static double CodeLen(CLenProbs *lp, unsigned sym, unsigned posState)
{
  double bits = 0;
  CProb *probs = lp->low;
  if (sym < kLenNumLowSymbols)
  {
    unsigned m = 1, i;
    bits += CodeBit(probs, 0);
    probs += posState << (kLenNumLowBits + 1);
    for (i = 0; i < 3; i++)
    {
      const unsigned bit = (sym >> (2 - i)) & 1;
      bits += CodeBit(probs + m, bit);
      m = (m << 1) + bit;
    }
    return bits;
  }
  bits += CodeBit(probs, 1);
  probs += kLenNumLowSymbols;
  if (sym < kLenNumLowSymbols * 2)
  {
    unsigned m = 1, i;
    sym -= kLenNumLowSymbols;
    bits += CodeBit(probs, 0);
    probs += posState << (kLenNumLowBits + 1);
    for (i = 0; i < 3; i++)
    {
      const unsigned bit = (sym >> (2 - i)) & 1;
      bits += CodeBit(probs + m, bit);
      m = (m << 1) + bit;
    }
    return bits;
  }
  bits += CodeBit(probs, 1);
  {
    unsigned s = (sym - kLenNumLowSymbols * 2) | 0x100;
    do
    {
      const unsigned bit = (s >> 7) & 1;
      bits += CodeBit(lp->high + (s >> 8), bit);
      s <<= 1;
    }
    while (s < 0x10000);
  }
  return bits;
}

static unsigned GetPosSlot(UInt32 dist)
{
  unsigned n;
  if (dist < 4) return dist;
  n = 31 - (unsigned)__builtin_clz(dist);
  return (n << 1) + ((dist >> (n - 1)) & 1);
}

static double CodeDistance(UInt32 dist, unsigned len)
{
  double bits = 0;
  const unsigned slot = GetPosSlot(dist);
  {
    unsigned s = slot + (1 << kNumPosSlotBits);
    CProb *probs = posSlot[GetLenToPosState(len) + (g_distCtx ? 4 * g_prevSlotBucket : 0)];
    do
    {
      const unsigned bit = (s >> (kNumPosSlotBits - 1)) & 1;
      bits += CodeBit(probs + (s >> kNumPosSlotBits), bit);
      s <<= 1;
    }
    while (s < (1 << (kNumPosSlotBits * 2)));
  }
  g_prevSlotBucket = slot >> 3; /* 0..7 */
  if (dist >= kStartPosModelIndex)
  {
    const unsigned footerBits = (slot >> 1) - 1;
    if (dist < kNumFullDistances)
    {
      const unsigned base = (2 | (slot & 1)) << footerBits;
      unsigned m = 1, i;
      UInt32 v = dist;
      for (i = 0; i < footerBits; i++)
      {
        const unsigned bit = v & 1;
        v >>= 1;
        bits += CodeBit(posEncoders + base + m, bit);
        m = (m << 1) | bit;
      }
    }
    else
    {
      unsigned m = 1, i;
      UInt32 v = dist;
      bits += (double)(footerBits - kNumAlignBits); /* direct bits */
      for (i = 0; i < kNumAlignBits; i++)
      {
        const unsigned bit = v & 1;
        v >>= 1;
        bits += CodeBit(posAlign + m, bit);
        m = (m << 1) | bit;
      }
    }
  }
  return bits;
}

/* ---------------------------------------------------------------- main */
static void Usage(void)
{
  fprintf(stderr, "lzmodel <input> <tokens.bin> [-c shift5|dual|cnt[:limit]] [-l lc3|lc8|mix2|mix3] [-lc N] [-pb N] [-a]\n");
  exit(2);
}

int main(int argc, char **argv)
{
  const char *inName = NULL, *tokName = NULL;
  unsigned pb = 2;
  int i;
  for (i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "-c") && i + 1 < argc)
    {
      const char *v = argv[++i];
      if (!strcmp(v, "shift5")) g_counter = C_SHIFT5;
      else if (!strcmp(v, "dual")) g_counter = C_DUAL;
      else if (!strncmp(v, "cnt", 3)) { g_counter = C_CNT; if (v[3] == ':') g_cntLimit = (unsigned)atoi(v + 4); }
      else Usage();
    }
    else if (!strcmp(argv[i], "-l") && i + 1 < argc)
    {
      const char *v = argv[++i];
      if (!strcmp(v, "lc3")) g_litModel = L_LC3;
      else if (!strcmp(v, "lc8")) g_litModel = L_LC8;
      else if (!strcmp(v, "mix2")) g_litModel = L_MIX2;
      else if (!strcmp(v, "mix3")) g_litModel = L_MIX3;
      else Usage();
    }
    else if (!strcmp(argv[i], "-lc") && i + 1 < argc) g_lc = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "-pb") && i + 1 < argc) pb = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "-lr") && i + 1 < argc) g_mixLr = atof(argv[++i]);
    else if (!strcmp(argv[i], "-a")) g_apm = 1;
    else if (!strcmp(argv[i], "-d")) g_distCtx = 1;
    else if (!inName) inName = argv[i];
    else if (!tokName) tokName = argv[i];
    else Usage();
  }
  if (!inName || !tokName || g_lc > 8 || pb > 4 || g_cntLimit > 250)
    Usage();

  /* tables */
  for (i = 1; i < 65536; i++) g_cost0[i] = -log2((double)i / 65536.0);
  g_cost0[0] = 32;
  for (i = 0; i < 256; i++) g_rec[i] = 65536 / (i + 2);

  /* input */
  Byte *data; size_t dataSize;
  {
    FILE *f = fopen(inName, "rb");
    if (!f) { perror(inName); return 1; }
    fseek(f, 0, SEEK_END); dataSize = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    data = (Byte *)malloc(dataSize + 1);
    if (fread(data, 1, dataSize, f) != dataSize) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);
  }
  Byte *tok; size_t tokSize;
  {
    FILE *f = fopen(tokName, "rb");
    if (!f) { perror(tokName); return 1; }
    fseek(f, 0, SEEK_END); tokSize = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    tok = (Byte *)malloc(tokSize + 6);
    if (fread(tok, 1, tokSize, f) != tokSize) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);
  }
  const size_t numTok = tokSize / 6;

  /* model init */
  InitProbs(&isMatch[0][0], kNumStates * kNumPosStatesMax);
  InitProbs(isRep, kNumStates); InitProbs(isRepG0, kNumStates); InitProbs(isRepG1, kNumStates); InitProbs(isRepG2, kNumStates);
  InitProbs(&isRep0Long[0][0], kNumStates * kNumPosStatesMax);
  InitProbs(&posSlot[0][0], (kNumLenToPosStates * 8) << kNumPosSlotBits);
  InitProbs(posEncoders, kNumFullDistances);
  InitProbs(posAlign, 1 << kNumAlignBits);
  InitProbs(lenProbs.low, sizeof(lenProbs.low) / sizeof(CProb)); InitProbs(lenProbs.high, kLenNumHighSymbols);
  InitProbs(repLenProbs.low, sizeof(repLenProbs.low) / sizeof(CProb)); InitProbs(repLenProbs.high, kLenNumHighSymbols);
  g_litLc = (CProb *)malloc(sizeof(CProb) * ((size_t)0x300 << g_lc));
  InitProbs(g_litLc, (size_t)0x300 << g_lc);
  if (g_litModel != L_LC3)
  {
    g_litO1 = (CProb *)malloc(sizeof(CProb) * ((size_t)0x300 << 8));
    InitProbs(g_litO1, (size_t)0x300 << 8);
  }
  if (g_litModel == L_MIX3)
  {
    g_litO2 = (CProb *)malloc(sizeof(CProb) * ((size_t)0x300 << kO2Bits));
    InitProbs(g_litO2, (size_t)0x300 << kO2Bits);
  }
  if (g_litModel == L_MIX2 || g_litModel == L_MIX3)
  {
    size_t k;
    g_mixW = (double *)malloc(sizeof(double) * 0x600 * kMixInputs);
    for (k = 0; k < (size_t)0x600 * kMixInputs; k++)
      g_mixW[k] = (g_litModel == L_MIX3) ? 0.35 : 0.5;
  }
  if (g_apm)
    Apm_Init();

  /* replay */
  unsigned state = 0;
  UInt32 reps[kNumReps] = { 1, 1, 1, 1 };
  const unsigned pbMask = (1u << pb) - 1;
  size_t pos = 0;
  size_t t = 0;
  double total = 0;
  for (;;)
  {
    unsigned len;
    UInt32 back;
    if (pos == 0)
    {
      /* the first byte: a literal with no record (lztrace) */
      len = 1; back = MARK_LIT;
    }
    else
    {
      if (t >= numTok) break;
      len = tok[t * 6] | ((unsigned)tok[t * 6 + 1] << 8);
      back = tok[t * 6 + 2] | ((UInt32)tok[t * 6 + 3] << 8) | ((UInt32)tok[t * 6 + 4] << 16) | ((UInt32)tok[t * 6 + 5] << 24);
      t++;
    }
    if (pos + len > dataSize) { fprintf(stderr, "tokens beyond the input at %zu\n", pos); return 1; }
    const unsigned posState = (unsigned)pos & pbMask;
    const unsigned prevByte = pos ? data[pos - 1] : 0;
    const unsigned prev2 = pos >= 2 ? data[pos - 2] : 0;
    const unsigned apmCtx = state * 256 + (prevByte >> 4) * 16 + (prev2 >> 4);

    if (back == MARK_LIT)
    {
      g_bitsFlags += g_apm ? CodeBit_Apm(&isMatch[state][posState], 0, apmCtx) : CodeBit(&isMatch[state][posState], 0);
      {
        const unsigned ctxLc = prevByte >> (8 - g_lc);
        const unsigned ctxO2 = ((prevByte << 8) | prev2) * 2654435761u >> (32 - kO2Bits);
        const int matched = !IsLitState(state);
        const unsigned matchByte = matched ? data[pos - reps[0]] : 0;
        g_bitsLit += CodeLiteral(data[pos], matchByte, matched, ctxLc, prevByte, ctxO2 & ((1u << kO2Bits) - 1));
      }
      state = kLiteralNextStates[state];
      g_numLit++;
    }
    else
    {
      g_bitsFlags += g_apm ? CodeBit_Apm(&isMatch[state][posState], 1, apmCtx) : CodeBit(&isMatch[state][posState], 1);
      if (back < kNumReps)
      {
        g_bitsFlags += CodeBit(&isRep[state], 1);
        if (back == 0)
        {
          g_bitsFlags += CodeBit(&isRepG0[state], 0);
          g_bitsFlags += CodeBit(&isRep0Long[state][posState], len != 1);
        }
        else
        {
          UInt32 dist;
          g_bitsFlags += CodeBit(&isRepG0[state], 1);
          if (back == 1)
          {
            g_bitsFlags += CodeBit(&isRepG1[state], 0);
            dist = reps[1];
          }
          else
          {
            g_bitsFlags += CodeBit(&isRepG1[state], 1);
            if (back == 2)
            {
              g_bitsFlags += CodeBit(&isRepG2[state], 0);
              dist = reps[2];
            }
            else
            {
              g_bitsFlags += CodeBit(&isRepG2[state], 1);
              dist = reps[3];
              reps[3] = reps[2];
            }
            reps[2] = reps[1];
          }
          reps[1] = reps[0];
          reps[0] = dist;
        }
        if (len != 1)
        {
          g_bitsLen += CodeLen(&repLenProbs, len - kMatchMinLen, posState);
          state = kRepNextStates[state];
          g_numRep++;
        }
        else
        {
          state = kShortRepNextStates[state];
          g_numShortRep++;
        }
      }
      else
      {
        const UInt32 dist = back - kNumReps;
        g_bitsFlags += CodeBit(&isRep[state], 0);
        state = kMatchNextStates[state];
        g_bitsLen += CodeLen(&lenProbs, len - kMatchMinLen, posState);
        reps[3] = reps[2]; reps[2] = reps[1]; reps[1] = reps[0]; reps[0] = dist + 1;
        g_bitsDist += CodeDistance(dist, len);
        g_numMatch++;
      }
    }
    pos += len;
  }
  if (pos != dataSize)
    fprintf(stderr, "warning: replayed %zu of %zu bytes\n", pos, dataSize);
  total = g_bitsLit + g_bitsFlags + g_bitsLen + g_bitsDist;
  printf("input %zu  literals %llu  matches %llu  reps %llu  shortreps %llu\n", dataSize,
      (unsigned long long)g_numLit, (unsigned long long)g_numMatch, (unsigned long long)g_numRep, (unsigned long long)g_numShortRep);
  printf("bytes: literals %.0f  flags %.0f  lengths %.0f  distances %.0f\n",
      g_bitsLit / 8, g_bitsFlags / 8, g_bitsLen / 8, g_bitsDist / 8);
  printf("total %.0f (%.3f%%)\n", total / 8, total / 8 * 100.0 / (double)dataSize);
  return 0;
}
