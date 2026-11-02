/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip) */
/* FloatPlane.c -- lossless coder for arrays of floating-point numbers (AI model weights)
Heptazip (design 0008)
The block format is described in FloatPlane.h. */

#include "Precomp.h"

#include <string.h>

#include "CpuArch.h"
#include "FloatPlane.h"
#include "FloatQuant.h"
#include "HuffEnc.h"

#define kMaxK        2                   /* mantissa bits in the Huffman symbol: 0..2 */
#define kMaxSyms     (256 << kMaxK)      /* 1024: Huffman_Generate allows up to 1024 */
#define kMaxLen      12
#define kTableBits   kMaxLen
#define kSampleSize  ((size_t)1 << 14)   /* to choose type and phase: 8192 BF16 elements */
#define kNumStreams  4                   /* element i is in Huffman stream i % 4 */

#define MODE_STORED  0
#define MODE_SPLIT   1
#define MODE_LZ      2

#define kLzProbeSize  ((size_t)1 << 14)
#define kLzHeaderSize 9
#define kSplitHeaderSize  (1 + 4 + 3)    /* mode, outLen, type, phase, k */

typedef struct
{
  unsigned w;      /* element size in bytes */
  unsigned shift;  /* position of the exponent byte */
} CFloatType;

/* index = type byte of the block */
static const CFloatType g_Types[] = { { 2, 7 }, { 2, 8 }, { 4, 23 } };
#define kNumTypes  (sizeof(g_Types) / sizeof(g_Types[0]))


static UInt32 LoadElem(const Byte *p, unsigned w)
{
  return w == 2 ? GetUi16(p) : GetUi32(p);
}

/* With SK = s - k: the symbol is the exponent byte and the k mantissa bits below it, r the
   other bits (low SK bits, then the bits above the symbol). */
#define SPLIT_E(x, SK, K)  (((x) >> (SK)) & ((256u << (K)) - 1))
#define SPLIT_R(x, SK, K)  (((x) & (((UInt32)1 << (SK)) - 1)) | (((x) >> ((SK) + 8 + (K))) << (SK)))
#define JOIN(e, r, SK, K)  (((r) & (((UInt32)1 << (SK)) - 1)) | ((UInt32)(e) << (SK)) \
    | (((r) >> (SK)) << ((SK) + 8 + (K))))

/* bits of r per element, and bytes of the packed r plane */
#define R_BITS(w, k)  ((unsigned)(w) * 8 - 8 - (k))
#define R_BYTES(n, b)  ((size_t)(((UInt64)(n) * (b) + 7) >> 3))
#define LENS_BYTES(k)  ((size_t)(256 << (k)) / 2)


/* ---------- Quantized structs (mode 3): GGUF Q4_0, Q8_0, Q4_K, Q5_K, Q6_K ---------- */

#define MODE_QUANT  3
#define kQuantHeaderSize  (1 + 4 + 2)   /* mode, outLen, qtype, phase */
#define kQSampleSize  ((size_t)1 << 16) /* to find type and phase */
#define kQuantCtxOnly ((size_t)1 << 18) /* from this block size, mode 4 without mode 3 */
#define kMaxQFields  4

/* A field of the struct is one Huffman-coded sequence of symbols:
   QF_BYTES: the bytes start + j * step (j < len) of each struct, 256 symbols;
   QF_Q5, QF_Q6: the 256 quantized values of a Q5_K / Q6_K struct, 5 or 6 bits each,
     whose low 4 bits and high 1-2 bits are in different bytes (layout in FloatPlane.h): 32 / 64 symbols. */
#define QF_BYTES  0
#define QF_Q5     1
#define QF_Q6     2

typedef struct
{
  Byte kind, start, len, step;
} CQField;

typedef struct
{
  unsigned size;       /* bytes of one struct (ggml block of 32 or 256 weights) */
  unsigned mark[2];    /* high bytes of the fp16 scales d (and dmin): few values in a
                          tensor; (mark[1] == mark[0]: one scale) */
  unsigned numFields;
  CQField f[kMaxQFields];
} CQType;

/* index = qtype byte of the block; layouts of ggml-common.h */
static const CQType g_QTypes[] =
{
  /* Q4_0: d, qs[16] */
  {  18, { 1, 1 }, 3, { { QF_BYTES, 0, 1, 1 }, { QF_BYTES, 1, 1, 1 }, { QF_BYTES, 2, 16, 1 } } },
  /* Q8_0: d, qs[32] */
  {  34, { 1, 1 }, 3, { { QF_BYTES, 0, 1, 1 }, { QF_BYTES, 1, 1, 1 }, { QF_BYTES, 2, 32, 1 } } },
  /* Q4_K: d, dmin, scales[12], qs[128]: low bytes of d and dmin, high bytes, ... */
  { 144, { 1, 3 }, 4, { { QF_BYTES, 0, 2, 2 }, { QF_BYTES, 1, 2, 2 }, { QF_BYTES, 4, 12, 1 },
                   { QF_BYTES, 16, 128, 1 } } },
  /* Q5_K: d, dmin, scales[12], qh[32], qs[128] */
  { 176, { 1, 3 }, 4, { { QF_BYTES, 0, 2, 2 }, { QF_BYTES, 1, 2, 2 }, { QF_BYTES, 4, 12, 1 },
                   { QF_Q5, 16, 0, 0 } } },
  /* Q6_K: ql[128], qh[64], scales[16], d */
  { 210, { 209, 209 }, 4, { { QF_Q6, 0, 0, 0 }, { QF_BYTES, 192, 16, 1 }, { QF_BYTES, 208, 1, 1 },
                   { QF_BYTES, 209, 1, 1 } } },
  /* Q2_K: scales[16], qs[64], d, dmin */
  {  84, { 81, 83 }, 4, { { QF_BYTES, 80, 2, 2 }, { QF_BYTES, 81, 2, 2 }, { QF_BYTES, 0, 16, 1 },
                   { QF_BYTES, 16, 64, 1 } } },
  /* Q3_K: hmask[32], qs[64], scales[12], d */
  { 110, { 109, 109 }, 4, { { QF_BYTES, 108, 1, 1 }, { QF_BYTES, 109, 1, 1 }, { QF_BYTES, 96, 12, 1 },
                   { QF_BYTES, 0, 96, 1 } } }
};
#define kNumQTypes  (sizeof(g_QTypes) / sizeof(g_QTypes[0]))

#define QF_WIDTH(f)     ((f)->kind == QF_BYTES ? (unsigned)(f)->len : 256u)   /* symbols per struct */
#define QF_NUM_SYMS(f)  ((f)->kind == QF_BYTES ? 256u : (f)->kind == QF_Q5 ? 32u : 64u)

/* ---------- Encoder ---------- */

/* Order-0 cost of the exponent bytes in bits, fixed point (x 2^16): a log2 approximation
   is enough to rank the candidates of the sample. */
static UInt64 Hist_Cost(const UInt32 *hist, unsigned numSyms, UInt32 total)
{
  /* cost = sum f * log2(total / f) */
  UInt64 cost = 0;
  unsigned i;
  for (i = 0; i < numSyms; i++)
  {
    const UInt32 f = hist[i];
    if (f != 0)
    {
      /* log2 in 16.16 fixed point via the position of the top bit and 8 more bits */
      UInt32 v = total / f;   /* >= 1 */
      unsigned b = 0;
      UInt32 frac;
      while ((v >> b) > 1)
        b++;
      frac = (UInt32)(((UInt64)(total) << 8) / ((UInt64)f << b)); /* in [256, 512) */
      cost += (UInt64)f * (((UInt64)b << 16) + (((UInt64)frac - 256) << 8));
    }
  }
  return cost;
}

/* histogram of the exponent bytes (k = 0) of n elements */
static void Histogram(const Byte *src, size_t n, const CFloatType *t, UInt32 *hist)
{
  size_t i;
  const unsigned w = t->w, s = t->shift;
  memset(hist, 0, 256 * sizeof(hist[0]));
  for (i = 0; i < n; i++)
    hist[SPLIT_E(LoadElem(src + i * w, w), s, 0)]++;
}

/* histograms of the k = 2 symbols of each stream: hist4[j * 1024 + sym], element i in
   stream i % 4. The histograms for k = 1, 0 are sums of these. */
static void Histogram4(const Byte *src, size_t n, const CFloatType *t, UInt32 *hist4)
{
  size_t i;
  const unsigned w = t->w, sk = t->shift - kMaxK;
  memset(hist4, 0, kNumStreams * kMaxSyms * sizeof(hist4[0]));
  for (i = 0; i < n; i++)
    hist4[(i & 3) * kMaxSyms + SPLIT_E(LoadElem(src + i * w, w), sk, kMaxK)]++;
}

static void Canonical_Codes(const Byte *lens, unsigned numSyms, UInt32 *codes)
{
  unsigned counts[kMaxLen + 1];
  UInt32 next[kMaxLen + 1];
  UInt32 code = 0;
  unsigned i;
  memset(counts, 0, sizeof(counts));
  for (i = 0; i < numSyms; i++)
    counts[lens[i]]++;
  counts[0] = 0;
  for (i = 1; i <= kMaxLen; i++)
  {
    code = (code + counts[i - 1]) << 1;
    next[i] = code;
  }
  for (i = 0; i < numSyms; i++)
    codes[i] = lens[i] ? next[lens[i]]++ : 0;
}

typedef struct
{
  unsigned k;
  Byte lens[kMaxSyms];
  size_t huffSizes[kNumStreams];
  size_t total;   /* size of the whole split block */
} CSplitPlan;

/* Estimated size in bits (16.16) with k mantissa bits in the symbol: entropy of the
   symbols, the r bits and the table of code lengths. Ranks k = 0..2 without building
   three Huffman codes. */
static UInt64 Estimate_K(const UInt32 *hist4, unsigned k, size_t n, unsigned w)
{
  const unsigned numSyms = 256u << k;
  const unsigned fold = kMaxK - k;
  UInt32 hist[kMaxSyms];
  unsigned i;
  memset(hist, 0, numSyms * sizeof(UInt32));
  for (i = 0; i < kNumStreams * kMaxSyms; i++)
    hist[(i & (kMaxSyms - 1)) >> fold] += hist4[i];
  return Hist_Cost(hist, numSyms, (UInt32)n)
      + (((UInt64)n * R_BITS(w, k) + LENS_BYTES(k) * 8) << 16);
}

/* Huffman code and exact block size for k mantissa bits in the symbol. */
static void Plan_Split(const UInt32 *hist4, unsigned k, size_t n, unsigned w, size_t phase,
    size_t tail, CSplitPlan *plan)
{
  const unsigned numSyms = 256u << k;
  const unsigned fold = kMaxK - k;   /* k = 2 symbols per k symbol: 1 << fold */
  UInt32 hist[kMaxSyms];
  UInt32 histJ[kNumStreams][kMaxSyms];
  UInt32 p[kMaxSyms];
  size_t huffTotal = 0;
  unsigned i, j;
  for (j = 0; j < kNumStreams; j++)
  {
    memset(histJ[j], 0, numSyms * sizeof(UInt32));
    for (i = 0; i < kMaxSyms; i++)
      histJ[j][i >> fold] += hist4[j * kMaxSyms + i];
  }
  for (i = 0; i < numSyms; i++)
    hist[i] = histJ[0][i] + histJ[1][i] + histJ[2][i] + histJ[3][i];
  Huffman_Generate(hist, p, plan->lens, numSyms, kMaxLen);
  for (j = 0; j < kNumStreams; j++)
  {
    UInt64 numBits = 0;
    for (i = 0; i < numSyms; i++)
      numBits += (UInt64)histJ[j][i] * plan->lens[i];
    plan->huffSizes[j] = (size_t)((numBits + 7) >> 3);
    huffTotal += plan->huffSizes[j];
  }
  plan->k = k;
  plan->total = kSplitHeaderSize + phase + tail + LENS_BYTES(k) + 4 * kNumStreams
      + huffTotal + R_BYTES(n, R_BITS(w, k));
}

/* Code writers, MSB first: A has C pending bits in its low bits. ADD_CODE appends a code
   (at most 12 bits); FLUSH_CODES stores the whole bytes with one 8-byte store while the
   stream has 8 bytes of room before its end E (the extra bytes are rewritten by the next
   store), else byte by byte: no branch that depends on the code lengths. C < 8 after a
   flush: 4 codes between flushes fit in A. */
#define ADD_CODE(A, C, sym) \
  { A = (A << lens[sym]) | codes[sym]; C += lens[sym]; }
#define FLUSH_CODES(A, C, D, E) \
  { \
    if ((size_t)((E) - (D)) >= 8) \
    { \
      SetBe64(D, (A << 1) << (63 - C))   /* (C may be 0) */ \
      D += C >> 3; \
      C &= 7; \
    } \
    else \
      while (C >= 8) { C -= 8; *D++ = (Byte)(A >> C); } \
  }
/* the end of a stream: the last bits, zero padding to a byte */
#define END_CODES(A, C, D) \
  { \
    while (C >= 8) { C -= 8; *D++ = (Byte)(A >> C); } \
    if (C != 0) \
      *D++ = (Byte)(A << (8 - C)); \
  }

/* Writes the split block (mode 1) planned by Plan_Split(). */
static size_t Write_Split(const Byte *src, size_t srcLen, Byte *dest,
    unsigned type, unsigned phase, const CSplitPlan *plan)
{
  const CFloatType *ft = &g_Types[type];
  const unsigned w = ft->w, K = plan->k, SK = ft->shift - K;
  const unsigned rb = R_BITS(w, K);
  const unsigned numSyms = 256u << K;
  const size_t n = (srcLen - phase) / w;
  const size_t tail = srcLen - phase - n * w;
  const Byte *e = src + phase;
  const Byte *lens = plan->lens;
  UInt32 codes[kMaxSyms];
  Byte *d = dest;
  Byte *ds[kNumStreams];
  Byte *dr;
  UInt64 racc = 0;
  unsigned rcnt = 0;
  size_t i;
  unsigned j;

  Canonical_Codes(lens, numSyms, codes);
  *d++ = MODE_SPLIT;
  SetUi32(d, (UInt32)srcLen)  d += 4;
  *d++ = (Byte)type;
  *d++ = (Byte)phase;
  *d++ = (Byte)K;
  memcpy(d, src, phase);                    d += phase;
  memcpy(d, src + srcLen - tail, tail);     d += tail;
  for (j = 0; j < numSyms; j += 2)
    *d++ = (Byte)(lens[j] | (lens[j + 1] << 4));
  for (j = 0; j < kNumStreams; j++)
  {
    SetUi32(d, (UInt32)plan->huffSizes[j])  d += 4;
  }
  for (j = 0; j < kNumStreams; j++)
  {
    ds[j] = d;
    d += plan->huffSizes[j];
  }
  dr = d;   /* the packed r plane, written in the same pass as the codes */

  /* r: rb bits per element, little-endian bit order */
  #define PUT_R(x) \
  { \
    racc |= (UInt64)SPLIT_R(x, SK, K) << rcnt; \
    rcnt += rb; \
    if (rcnt >= 32) \
    { \
      SetUi32(dr, (UInt32)racc) \
      dr += 4; \
      racc >>= 32; \
      rcnt -= 32; \
    } \
  }

  /* 16 elements (4 codes per stream) per iteration with the writers in local variables */
  {
    Byte *d0 = ds[0], *d1 = ds[1], *d2 = ds[2], *d3 = ds[3];
    Byte * const e0 = ds[0] + plan->huffSizes[0], * const e1 = ds[1] + plan->huffSizes[1];
    Byte * const e2 = ds[2] + plan->huffSizes[2], * const e3 = ds[3] + plan->huffSizes[3];
    UInt64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (i = 0; i + 16 <= n; i += 16)
    {
      unsigned t;
      for (t = 0; t < 16; t += 4)
      {
        const UInt32 x0 = LoadElem(e + (i + t + 0) * w, w);
        const UInt32 x1 = LoadElem(e + (i + t + 1) * w, w);
        const UInt32 x2 = LoadElem(e + (i + t + 2) * w, w);
        const UInt32 x3 = LoadElem(e + (i + t + 3) * w, w);
        ADD_CODE(a0, c0, SPLIT_E(x0, SK, K))
        ADD_CODE(a1, c1, SPLIT_E(x1, SK, K))
        ADD_CODE(a2, c2, SPLIT_E(x2, SK, K))
        ADD_CODE(a3, c3, SPLIT_E(x3, SK, K))
        PUT_R(x0)
        PUT_R(x1)
        PUT_R(x2)
        PUT_R(x3)
      }
      FLUSH_CODES(a0, c0, d0, e0)
      FLUSH_CODES(a1, c1, d1, e1)
      FLUSH_CODES(a2, c2, d2, e2)
      FLUSH_CODES(a3, c3, d3, e3)
    }
    for (; i < n; i++)
    {
      const UInt32 x = LoadElem(e + i * w, w);
      const unsigned sym = SPLIT_E(x, SK, K);
      switch (i & 3)
      {
        case 0: ADD_CODE(a0, c0, sym)  FLUSH_CODES(a0, c0, d0, e0)  break;
        case 1: ADD_CODE(a1, c1, sym)  FLUSH_CODES(a1, c1, d1, e1)  break;
        case 2: ADD_CODE(a2, c2, sym)  FLUSH_CODES(a2, c2, d2, e2)  break;
        default: ADD_CODE(a3, c3, sym)  FLUSH_CODES(a3, c3, d3, e3)  break;
      }
      PUT_R(x)
    }
    END_CODES(a0, c0, d0)
    END_CODES(a1, c1, d1)
    END_CODES(a2, c2, d2)
    END_CODES(a3, c3, d3)
  }
  while (rcnt > 0)
  {
    *dr++ = (Byte)racc;
    racc >>= 8;
    rcnt = rcnt > 8 ? rcnt - 8 : 0;
  }
  return (size_t)(dr - dest);
}

/* ---------- Encoder of quantized blocks (mode 3) ---------- */

/* collision probability sum(f^2) / n^2 (16.16) of the bytes b[i * S], i < n; hist[] is
   all zero before and after */
static UInt32 Collision(const Byte *b, unsigned S, size_t n, UInt32 *hist)
{
  UInt64 sumSq = 0;
  size_t i;
  for (i = 0; i < n; i++)
    sumSq += 2 * (UInt64)hist[b[i * S]]++ + 1;    /* (f + 1)^2 - f^2 */
  for (i = 0; i < n; i++)
    hist[b[i * S]] = 0;
  return (UInt32)((sumSq << 16) / ((UInt64)n * n));
}

/* (type, phase) of the quantized structs, from a sample: the high bytes of the fp16
   scales (d, and dmin in Q4_K/Q5_K) take few values in a tensor of quantized weights,
   while at a wrong period or phase they are quantized values or scale bytes. The measure
   is the collision probability of these bytes, sum(f^2) / n^2: 1/256 for random bytes,
   above 1/16 for the scales; the smaller of d and dmin, so that a phase with only one of
   them in place (2 bytes off in Q4_K, or Q4_0 structs seen as Q4_K: 144 = 8 * 18) loses.
   Returns 1 if the best candidate is above 1/64 (Renyi entropy below 6 bits). */
static int Detect_Quant(const Byte *src, size_t srcLen, unsigned *type, unsigned *phase)
{
  const size_t sampleLen = srcLen < kQSampleSize ? srcLen : kQSampleSize;
  UInt32 best = 0;
  UInt32 hist[256];
  unsigned t;
  memset(hist, 0, sizeof(hist));
  for (t = 0; t < kNumQTypes; t++)
  {
    const CQType *qt = &g_QTypes[t];
    const unsigned S = qt->size;
    unsigned p;
    if (sampleLen < (size_t)S * 16)
      continue;
    for (p = 0; p < S; p++)
    {
      const size_t n = (sampleLen - p - S) / S + 1;   /* whole structs from p */
      UInt32 c = Collision(src + p + qt->mark[0], S, n, hist);
      if (c > best && qt->mark[1] != qt->mark[0])
      {
        const UInt32 c2 = Collision(src + p + qt->mark[1], S, n, hist);
        if (c > c2)
          c = c2;
      }
      if (c > best)
      {
        best = c;
        *type = t;
        *phase = p;
      }
    }
  }
  return best > ((UInt32)1 << 16) / 64;
}

/* The symbols of field f of the structs s0 .. s0 + ns - 1 into buf (ns * w symbols).
   Q5_K / Q6_K values: layout in FloatPlane.h (as Join_QBits). */
#define kQGather  64   /* structs per call: buf has up to 64 * 256 symbols */

static void Q_Gather(const Byte *p, unsigned S, const CQField *f, size_t ns, UInt16 *buf)
{
  size_t s;
  unsigned l, c;
  for (s = 0; s < ns; s++, p += S)
  {
    if (f->kind == QF_BYTES)
    {
      const Byte *b = p + f->start;
      const unsigned step = f->step;
      for (l = 0; l < f->len; l++)
        buf[l] = b[l * step];
      buf += f->len;
    }
    else if (f->kind == QF_Q6)
    {
      for (c = 0; c < 2; c++)
      {
        const Byte *ql = p + 64 * c, *qh = p + 128 + 32 * c;
        UInt16 *v = buf + 128 * c;
        for (l = 0; l < 32; l++)
        {
          v[l]      = (UInt16)((ql[l] & 15)      | ((qh[l] & 3) << 4));
          v[l + 32] = (UInt16)((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4));
          v[l + 64] = (UInt16)((ql[l] >> 4)      | (((qh[l] >> 4) & 3) << 4));
          v[l + 96] = (UInt16)((ql[l + 32] >> 4) | ((qh[l] >> 6) << 4));
        }
      }
      buf += 256;
    }
    else
    {
      const Byte *qh = p + 16, *qs = p + 48;
      for (c = 0; c < 4; c++)
        for (l = 0; l < 32; l++)
        {
          buf[64 * c + l]      = (UInt16)((qs[32 * c + l] & 15) | (((qh[l] >> (2 * c)) & 1) << 4));
          buf[64 * c + 32 + l] = (UInt16)((qs[32 * c + l] >> 4) | (((qh[l] >> (2 * c + 1)) & 1) << 4));
        }
      buf += 256;
    }
  }
}

typedef struct
{
  unsigned type, phase;
  size_t n, tail;
  Byte lens[kMaxQFields][256];
  size_t huffSizes[kMaxQFields][kNumStreams];
  size_t total;   /* size of the whole quantized block */
} CQuantPlan;

/* Huffman code of each field and exact block size */
static void Plan_Quant(const Byte *src, size_t srcLen, unsigned type, unsigned phase, CQuantPlan *plan)
{
  const CQType *qt = &g_QTypes[type];
  const unsigned S = qt->size;
  const size_t n = (srcLen - phase) / S;
  UInt16 buf[kQGather * 256];
  unsigned fi;
  plan->type = type;
  plan->phase = phase;
  plan->n = n;
  plan->tail = srcLen - phase - n * S;
  plan->total = kQuantHeaderSize + phase + plan->tail;
  for (fi = 0; fi < qt->numFields; fi++)
  {
    const CQField *f = &qt->f[fi];
    const unsigned w = QF_WIDTH(f), numSyms = QF_NUM_SYMS(f);
    UInt32 hist4[kNumStreams][256];
    UInt32 hist[256];
    UInt32 pp[256];
    UInt64 sum = 0;
    size_t s;
    unsigned j, sym, sh = 0;
    memset(hist4, 0, sizeof(hist4));
    for (s = 0; s < n; s += kQGather)
    {
      const size_t ns = n - s < kQGather ? n - s : kQGather;
      const size_t num = ns * w;
      size_t k = 0;
      /* symbol i of the field is in stream i % 4: s * w is a multiple of 4 (kQGather is) */
      Q_Gather(src + phase + s * S, S, f, ns, buf);
      for (; k + 4 <= num; k += 4)
      {
        hist4[0][buf[k]]++;
        hist4[1][buf[k + 1]]++;
        hist4[2][buf[k + 2]]++;
        hist4[3][buf[k + 3]]++;
      }
      for (; k < num; k++)
        hist4[k & 3][buf[k]]++;
    }
    /* Huffman_Generate() needs a sum of frequencies < 4M: scaled down if it is larger
       (the code is a little worse, the sizes below use the real counts) */
    for (sym = 0; sym < numSyms; sym++)
    {
      hist[sym] = hist4[0][sym] + hist4[1][sym] + hist4[2][sym] + hist4[3][sym];
      sum += hist[sym];
    }
    while ((sum >> sh) + numSyms > Z7_HUFFMAN_FREQS_SUM_MAX)
      sh++;
    if (sh != 0)
      for (sym = 0; sym < numSyms; sym++)
        if (hist[sym] != 0)
          hist[sym] = (hist[sym] >> sh) + 1;
    Huffman_Generate(hist, pp, plan->lens[fi], numSyms, kMaxLen);
    plan->total += numSyms / 2 + 4 * kNumStreams;
    for (j = 0; j < kNumStreams; j++)
    {
      UInt64 numBits = 0;
      for (sym = 0; sym < numSyms; sym++)
        numBits += (UInt64)hist4[j][sym] * plan->lens[fi][sym];
      plan->huffSizes[fi][j] = (size_t)((numBits + 7) >> 3);
      plan->total += plan->huffSizes[fi][j];
    }
  }
}

/* Writes the quantized block (mode 3) planned by Plan_Quant(). */
static size_t Write_Quant(const Byte *src, size_t srcLen, Byte *dest, const CQuantPlan *plan)
{
  const CQType *qt = &g_QTypes[plan->type];
  const unsigned S = qt->size;
  const size_t n = plan->n, phase = plan->phase, tail = plan->tail;
  Byte *d = dest;
  UInt16 buf[kQGather * 256];
  unsigned fi, j;

  *d++ = MODE_QUANT;
  SetUi32(d, (UInt32)srcLen)  d += 4;
  *d++ = (Byte)plan->type;
  *d++ = (Byte)phase;
  memcpy(d, src, phase);                    d += phase;
  memcpy(d, src + srcLen - tail, tail);     d += tail;
  for (fi = 0; fi < qt->numFields; fi++)
  {
    const unsigned numSyms = QF_NUM_SYMS(&qt->f[fi]);
    for (j = 0; j < numSyms; j += 2)
      *d++ = (Byte)(plan->lens[fi][j] | (plan->lens[fi][j + 1] << 4));
    for (j = 0; j < kNumStreams; j++)
    {
      SetUi32(d, (UInt32)plan->huffSizes[fi][j])  d += 4;
    }
  }

  for (fi = 0; fi < qt->numFields; fi++)
  {
    const CQField *f = &qt->f[fi];
    const unsigned w = QF_WIDTH(f), numSyms = QF_NUM_SYMS(f);
    const Byte *lens = plan->lens[fi];
    UInt32 codes[256];
    Byte *d0, *d1, *d2, *d3, *e0, *e1, *e2, *e3;
    UInt64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    size_t s;
    Canonical_Codes(lens, numSyms, codes);
    d0 = d;   d += plan->huffSizes[fi][0];   e0 = d;
    d1 = d;   d += plan->huffSizes[fi][1];   e1 = d;
    d2 = d;   d += plan->huffSizes[fi][2];   e2 = d;
    d3 = d;   d += plan->huffSizes[fi][3];   e3 = d;
    for (s = 0; s < n; s += kQGather)
    {
      const size_t ns = n - s < kQGather ? n - s : kQGather;
      const size_t num = ns * w;
      size_t k = 0;
      Q_Gather(src + phase + s * S, S, f, ns, buf);
      /* symbol k of the buffer is in stream k % 4: s * w is a multiple of 4 */
      for (; k + 16 <= num; k += 16)
      {
        ADD_CODE(a0, c0, buf[k])       ADD_CODE(a1, c1, buf[k + 1])
        ADD_CODE(a2, c2, buf[k + 2])   ADD_CODE(a3, c3, buf[k + 3])
        ADD_CODE(a0, c0, buf[k + 4])   ADD_CODE(a1, c1, buf[k + 5])
        ADD_CODE(a2, c2, buf[k + 6])   ADD_CODE(a3, c3, buf[k + 7])
        ADD_CODE(a0, c0, buf[k + 8])   ADD_CODE(a1, c1, buf[k + 9])
        ADD_CODE(a2, c2, buf[k + 10])  ADD_CODE(a3, c3, buf[k + 11])
        ADD_CODE(a0, c0, buf[k + 12])  ADD_CODE(a1, c1, buf[k + 13])
        ADD_CODE(a2, c2, buf[k + 14])  ADD_CODE(a3, c3, buf[k + 15])
        FLUSH_CODES(a0, c0, d0, e0)
        FLUSH_CODES(a1, c1, d1, e1)
        FLUSH_CODES(a2, c2, d2, e2)
        FLUSH_CODES(a3, c3, d3, e3)
      }
      for (; k < num; k++)
      {
        const unsigned sym = buf[k];
        switch (k & 3)
        {
          case 0: ADD_CODE(a0, c0, sym)  FLUSH_CODES(a0, c0, d0, e0)  break;
          case 1: ADD_CODE(a1, c1, sym)  FLUSH_CODES(a1, c1, d1, e1)  break;
          case 2: ADD_CODE(a2, c2, sym)  FLUSH_CODES(a2, c2, d2, e2)  break;
          default: ADD_CODE(a3, c3, sym)  FLUSH_CODES(a3, c3, d3, e3)  break;
        }
      }
    }
    END_CODES(a0, c0, d0)
    END_CODES(a1, c1, d1)
    END_CODES(a2, c2, d2)
    END_CODES(a3, c3, d3)
  }
  return (size_t)(d - dest);
}


size_t FloatPlane_EncodeBlock(const Byte *src, size_t srcLen, Byte *dest, const CFloatPlaneLz *lz)
{
  UInt32 hist[256];
  unsigned bestType = 0, bestPhase = 0;
  UInt64 bestCost = (UInt64)8 << 16;   /* bits per byte (16.16) of the best candidate; 8 = stored */
  const size_t storedTotal = FLOAT_PLANE_ENCODED_MAX(srcLen);
  size_t lzTotal = storedTotal;
  CSplitPlan plan;
  size_t splitTotal = storedTotal;
  CQuantPlan qplan;
  size_t quantTotal = storedTotal;
  CFloatQuantPlan cplan;
  size_t ctxTotal = storedTotal;
  unsigned t;
  int haveCandidate = 0;

  /* 1. Type and phase from a sample: cost per byte of exponent + raw rest (k = 0). */
  {
    const size_t sampleLen = srcLen < kSampleSize ? srcLen : kSampleSize;
    for (t = 0; t < kNumTypes; t++)
    {
      const CFloatType *ft = &g_Types[t];
      unsigned phase;
      for (phase = 0; phase < ft->w; phase++)
      {
        size_t n;
        UInt64 cost;
        if (sampleLen < phase + ft->w)
          continue;
        n = (sampleLen - phase) / ft->w;
        Histogram(src + phase, n, ft, hist);
        cost = Hist_Cost(hist, 256, (UInt32)n) + ((UInt64)n * (ft->w - 1) * 8 << 16);
        /* normalize to the bytes covered: n * w */
        cost = cost / (n * ft->w);
        if (!haveCandidate || cost < bestCost)
        {
          haveCandidate = 1;
          bestCost = cost;
          bestType = t;
          bestPhase = phase;
        }
      }
    }
  }

  /* 2. Quantized structs (mode 3), if the sample does not look like floats (above 7 bits
     per byte) and the scales of quantized structs show up in it. */
  if (bestCost >= ((UInt64)7 << 16))
  {
    unsigned qType = 0, qPhase = 0;
    if (Detect_Quant(src, srcLen, &qType, &qPhase))
    {
      size_t qBest;
      /* the K-quants with the context model (mode 4); the Huffman code (mode 3) only for
         the other types and in small blocks, where the tables of the contexts can make
         mode 4 larger (from 256 KiB mode 4 wins: about 1.7 points on Q4_K / Q5_K) */
      if (FloatQuant_Plan(src, srcLen, qType, qPhase, &cplan))
        ctxTotal = cplan.total;
      if (ctxTotal == storedTotal || srcLen < kQuantCtxOnly)
      {
        Plan_Quant(src, srcLen, qType, qPhase, &qplan);
        quantTotal = qplan.total;
      }
      qBest = ctxTotal < quantTotal ? ctxTotal : quantTotal;
      if (qBest < storedTotal)
      {
        const UInt64 qCost = ((UInt64)qBest * 8 << 16) / srcLen;
        if (qCost < bestCost)
        {
          bestCost = qCost;         /* the bar for the zstd probe */
          haveCandidate = 0;        /* a split block would be larger: not planned */
        }
      }
    }
  }

  /* 3. k = 0, 1 or 2 mantissa bits in the symbol, by estimated size; then the Huffman code
     of the whole block and its exact size. */
  if (haveCandidate)
  {
    const CFloatType *ft = &g_Types[bestType];
    const unsigned w = ft->w;
    const size_t n = (srcLen - bestPhase) / w;
    const size_t tail = srcLen - bestPhase - n * w;
    UInt32 hist4[kNumStreams * kMaxSyms];
    unsigned k, bestK = 0;
    UInt64 bestEst = 0;
    Histogram4(src + bestPhase, n, ft, hist4);
    for (k = 0; k <= kMaxK; k++)
    {
      const UInt64 est = Estimate_K(hist4, k, n, w);
      if (k == 0 || est < bestEst)
      {
        bestEst = est;
        bestK = k;
      }
    }
    Plan_Split(hist4, bestK, n, w, bestPhase, tail, &plan);
    splitTotal = plan.total;
  }

  /* 4. zstd (mode 2), if a probe of the block start compresses better than the float
     (or quantized) estimate: then the whole block, and the smaller result wins. The frame
     is written in place (dest + 9); another block overwrites it. */
  if (lz && lz->Compress && storedTotal > kLzHeaderSize)
  {
    const size_t probeLen = srcLen < kLzProbeSize ? srcLen : kLzProbeSize;
    const size_t probeSize = lz->Compress(lz->ctx, src, probeLen, dest, storedTotal);
    if (probeSize != 0 && (((UInt64)probeSize * 8 << 16) / probeLen) < bestCost)
    {
      const size_t lzSize = lz->Compress(lz->ctx, src, srcLen, dest + kLzHeaderSize,
          storedTotal - kLzHeaderSize);
      if (lzSize != 0)
        lzTotal = kLzHeaderSize + lzSize;
    }
  }

  if (ctxTotal < quantTotal && ctxTotal < storedTotal && ctxTotal < splitTotal && ctxTotal < lzTotal)
  {
    /* the size of the plan is an estimate: the block is written if it fits */
    const size_t size = FloatQuant_Write(src, srcLen, dest, storedTotal - 1, &cplan);
    if (size != 0)
      return size;
  }
  if (quantTotal < storedTotal && quantTotal < splitTotal && quantTotal < lzTotal)
    return Write_Quant(src, srcLen, dest, &qplan);
  if (lzTotal < storedTotal && lzTotal <= splitTotal)
  {
    dest[0] = MODE_LZ;
    SetUi32(dest + 1, (UInt32)srcLen)
    SetUi32(dest + 5, (UInt32)(lzTotal - kLzHeaderSize))
    return lzTotal;
  }
  if (splitTotal < storedTotal)
    return Write_Split(src, srcLen, dest, bestType, bestPhase, &plan);

  dest[0] = MODE_STORED;
  SetUi32(dest + 1, (UInt32)srcLen)
  memcpy(dest + 5, src, srcLen);
  return srcLen + 5;
}


/* ---------- Decoder ---------- */

SRes FloatPlane_GetBlockOutLen(const Byte *src, size_t srcLen, size_t *outLen)
{
  UInt32 len;
  if (srcLen < 5 || src[0] > FLOAT_QUANT_MODE)
    return SZ_ERROR_DATA;
  len = GetUi32(src + 1);
  if (len == 0 || len > FLOAT_PLANE_BLOCK_MAX)
    return SZ_ERROR_DATA;
  *outLen = len;
  return SZ_OK;
}

/* Sizes of the 4 Huffman streams from the block header, with the same limits for
   FloatPlane_GetBlockSize() and FloatPlane_DecodeBlock(): stream j has the codes of
   nj = (n + 3 - j) / 4 elements, so at least nj bits and at most nj * kMaxLen bits. */
static SRes Get_StreamSizes(const Byte *p, size_t n, size_t *sizes, size_t *total)
{
  unsigned j;
  *total = 0;
  for (j = 0; j < kNumStreams; j++)
  {
    const size_t nj = (n + 3 - j) / 4;
    const size_t size = GetUi32(p + j * 4);
    if (size > ((nj * kMaxLen + 7) >> 3) || size < ((nj + 7) >> 3))
      return SZ_ERROR_DATA;
    if (sizes)
      sizes[j] = size;
    *total += size;
  }
  return SZ_OK;
}

/* Fields of a split block header (after mode and outLen). */
typedef struct
{
  unsigned type, w, s, phase, k;
  size_t n, tail;
  size_t headerSize;   /* up to the end of the stream sizes */
} CSplitHeader;

static SRes Parse_SplitHeader(const Byte *src, size_t srcLen, size_t outLen, CSplitHeader *h)
{
  if (srcLen < kSplitHeaderSize)
    return SZ_ERROR_INPUT_EOF;
  h->type = src[5];
  h->phase = src[6];
  h->k = src[7];
  if (h->type >= kNumTypes || h->k > kMaxK)
    return SZ_ERROR_DATA;
  h->w = g_Types[h->type].w;
  h->s = g_Types[h->type].shift;
  if (h->phase >= h->w || outLen < h->phase)
    return SZ_ERROR_DATA;
  h->n = (outLen - h->phase) / h->w;
  h->tail = outLen - h->phase - h->n * h->w;
  h->headerSize = kSplitHeaderSize + h->phase + h->tail + LENS_BYTES(h->k) + 4 * kNumStreams;
  return SZ_OK;
}

/* Fields of a quantized block header (after mode and outLen). */
typedef struct
{
  unsigned type, phase;
  size_t n, tail;
  const Byte *fieldHeaders;   /* per field: code lengths, then 4 stream sizes */
  size_t headerSize;          /* up to the end of the last stream sizes */
} CQuantHeader;

static SRes Parse_QuantHeader(const Byte *src, size_t srcLen, size_t outLen, CQuantHeader *h)
{
  const CQType *qt;
  unsigned fi;
  if (srcLen < kQuantHeaderSize)
    return SZ_ERROR_INPUT_EOF;
  h->type = src[5];
  h->phase = src[6];
  if (h->type >= kNumQTypes)
    return SZ_ERROR_DATA;
  qt = &g_QTypes[h->type];
  if (h->phase >= qt->size || outLen < h->phase + qt->size)   /* at least one struct */
    return SZ_ERROR_DATA;
  h->n = (outLen - h->phase) / qt->size;
  h->tail = outLen - h->phase - h->n * qt->size;
  h->fieldHeaders = src + kQuantHeaderSize + h->phase + h->tail;
  h->headerSize = kQuantHeaderSize + h->phase + h->tail;
  for (fi = 0; fi < qt->numFields; fi++)
    h->headerSize += QF_NUM_SYMS(&qt->f[fi]) / 2 + 4 * kNumStreams;
  return SZ_OK;
}

/* Sizes of the Huffman streams of all fields: total in *huffTotal */
static SRes Get_QuantStreamSizes(const CQuantHeader *h, size_t *huffTotal)
{
  const CQType *qt = &g_QTypes[h->type];
  const Byte *p = h->fieldHeaders;
  unsigned fi;
  *huffTotal = 0;
  for (fi = 0; fi < qt->numFields; fi++)
  {
    const CQField *f = &qt->f[fi];
    size_t total;
    p += QF_NUM_SYMS(f) / 2;
    RINOK(Get_StreamSizes(p, h->n * QF_WIDTH(f), NULL, &total))
    p += 4 * kNumStreams;
    *huffTotal += total;
  }
  return SZ_OK;
}

SRes FloatPlane_GetBlockSize(const Byte *src, size_t srcLen, size_t *blockSize)
{
  size_t outLen;
  if (srcLen < 5)
    return SZ_ERROR_INPUT_EOF;
  RINOK(FloatPlane_GetBlockOutLen(src, srcLen, &outLen))
  if (src[0] == MODE_STORED)
  {
    *blockSize = 5 + outLen;
    return SZ_OK;
  }
  if (src[0] == MODE_LZ)
  {
    size_t lzSize;
    if (srcLen < kLzHeaderSize)
      return SZ_ERROR_INPUT_EOF;
    lzSize = GetUi32(src + 5);
    /* as the encoder: the block is smaller than the stored block */
    if (lzSize == 0 || kLzHeaderSize + lzSize >= FLOAT_PLANE_ENCODED_MAX(outLen))
      return SZ_ERROR_DATA;
    *blockSize = kLzHeaderSize + lzSize;
    return SZ_OK;
  }
  if (src[0] == FLOAT_QUANT_MODE)
    return FloatQuant_GetBlockSize(src, srcLen, outLen, blockSize);
  if (src[0] == MODE_QUANT)
  {
    CQuantHeader h;
    size_t huffTotal;
    RINOK(Parse_QuantHeader(src, srcLen, outLen, &h))
    if (srcLen < h.headerSize)
      return SZ_ERROR_INPUT_EOF;
    RINOK(Get_QuantStreamSizes(&h, &huffTotal))
    *blockSize = h.headerSize + huffTotal;
    return SZ_OK;
  }
  {
    CSplitHeader h;
    size_t huffTotal;
    RINOK(Parse_SplitHeader(src, srcLen, outLen, &h))
    if (srcLen < h.headerSize)
      return SZ_ERROR_INPUT_EOF;
    RINOK(Get_StreamSizes(src + h.headerSize - 4 * kNumStreams, h.n, NULL, &huffTotal))
    *blockSize = h.headerSize + huffTotal + R_BYTES(h.n, R_BITS(h.w, h.k));
  }
  return SZ_OK;
}

/* table[v] for the next kTableBits bits v: symbol | (length << 10); 0 = not a code */
static int Build_Table(const Byte *packedLens, unsigned numSyms, UInt16 *table)
{
  Byte lens[kMaxSyms];
  UInt32 codes[kMaxSyms];
  unsigned counts[kMaxLen + 1];
  UInt32 kraft = 0;
  unsigned i;
  memset(counts, 0, sizeof(counts));
  for (i = 0; i < numSyms; i += 2)
  {
    lens[i] = (Byte)(packedLens[i / 2] & 15);
    lens[i + 1] = (Byte)(packedLens[i / 2] >> 4);
  }
  for (i = 0; i < numSyms; i++)
  {
    if (lens[i] > kMaxLen)
      return 0;
    counts[lens[i]]++;
  }
  for (i = 1; i <= kMaxLen; i++)
    kraft += (UInt32)counts[i] << (kMaxLen - i);
  if (kraft > ((UInt32)1 << kMaxLen))
    return 0;   /* over-subscribed */
  Canonical_Codes(lens, numSyms, codes);
  memset(table, 0, sizeof(UInt16) << kTableBits);
  for (i = 0; i < numSyms; i++)
  {
    const unsigned len = lens[i];
    if (len != 0)
    {
      const UInt32 first = codes[i] << (kMaxLen - len);
      const UInt32 num = (UInt32)1 << (kMaxLen - len);
      const UInt16 v = (UInt16)(i | (len << 10));
      UInt32 k;
      for (k = 0; k < num; k++)
        table[first + k] = v;
    }
  }
  return 1;
}

/* table2[v] for the next kTableBits bits v: one or two symbols, when two codes fit in
   the kTableBits bits: sym1 | (sym2 << 10) | (two << 20) | (length of both << 24);
   0 = the first code is not a code. Built from table[]. Returns the number of entries
   with two symbols: (numTwo / 4096) is the share of the lookups that give two symbols. */
#define T2_TWO  ((UInt32)1 << 20)

static unsigned Build_Table2(const UInt16 *table, UInt32 *table2)
{
  unsigned v, numTwo = 0;
  for (v = 0; v < ((unsigned)1 << kTableBits); v++)
  {
    const unsigned t1 = table[v];
    const unsigned len1 = t1 >> 10;
    UInt32 x = 0;
    if (len1 != 0)
    {
      const unsigned t2 = table[(v << len1) & (((unsigned)1 << kTableBits) - 1)];
      const unsigned len2 = t2 >> 10;
      x = (t1 & 0x3FF) | ((UInt32)len1 << 24);
      if (len2 != 0 && len1 + len2 <= kTableBits)
      {
        x = (t1 & 0x3FF) | ((UInt32)(t2 & 0x3FF) << 10) | T2_TWO | ((UInt32)(len1 + len2) << 24);
        numTwo++;
      }
    }
    table2[v] = x;
  }
  return numTwo;
}

/* r of element i, reading only the bytes inside the r plane: the bits of the field are
   always inside, the missing bytes would only give bits above it. */
static UInt32 Read_R(const Byte *r, size_t rBytes, size_t i, unsigned rb)
{
  const UInt64 pos = (UInt64)i * rb;
  const size_t byte = (size_t)(pos >> 3);
  UInt32 v = 0;
  unsigned b;
  for (b = 0; b < 4 && byte + b < rBytes; b++)
    v |= (UInt32)r[byte + b] << (8 * b);
  return (v >> (unsigned)(pos & 7)) & (((UInt32)1 << rb) - 1);
}

/* ---------- Split block: decoding loops ---------- */

/* Two phases per chunk of kChunk elements:
   1. Decode_Syms(): the Huffman symbols of the 4 streams into a small buffer (L1 cache).
      Scalar: each stream is a chain of dependent shifts; the 4 chains run in parallel.
   2. Join_*(): elements from the symbols and the packed r plane. A regular loop, one
      per (element size, SK, k), with constant shifts (and an AVX2 version).
   Two small loops instead of one big one: fewer live variables, better code. */

#define kChunk  2048   /* multiple of 16: every chunk starts at stream 0 */

/* state of the 4 bit readers and of the output: (bits) has (cnt) valid bits, MSB aligned;
   pad = zero bytes read after the end of a stream (an error if they are consumed) */
typedef struct
{
  const UInt16 *table;
  const UInt32 *table2;
  unsigned fast;       /* FAST_NONE: the reference decoder; FAST_ONE / FAST_TWO: fast loop
                          with one symbol per lookup (table) or up to two (table2) */
  const Byte *r;       /* the packed r plane */
  size_t rBytes;
  size_t n;            /* elements */
  size_t nSafe;        /* 8-byte loads of r are safe for the elements before nSafe */
  Byte *e;             /* the first element in dest */
  const Byte *hp[kNumStreams];
  const Byte *hEnd[kNumStreams];
  UInt64 bits[kNumStreams];
  unsigned cnt[kNumStreams];
  size_t pad[kNumStreams];
  /* quantized blocks: the field being decoded, the struct size */
  const CQField *qf;
  unsigned qS;
} CDecState;

#define DECODE_SYM(B, C, sym) \
{ \
  const unsigned v = table[(size_t)((B) >> (64 - kTableBits))]; \
  const unsigned len = v >> 10; \
  if (len == 0) \
    return SZ_ERROR_DATA; \
  sym = (UInt16)(v & 0x3FF); \
  B <<= len; \
  C -= len; \
}

/* reader with at least 8 bytes left: adds whole bytes up to 56..63 valid bits */
#define REFILL_FAST(H, B, C) \
{ \
  B |= GetBe64(H) >> C; \
  H += (63 - C) >> 3; \
  C |= 56; \
}

/* two symbols at most (or one) with table2, to positions o and o + 4 of the stream */
#define DECODE_SYM2(B, C, o) \
{ \
  const UInt32 v = table2[(size_t)((B) >> (64 - kTableBits))]; \
  const unsigned len = v >> 24; \
  if (v == 0) \
    return SZ_ERROR_DATA; \
  o[0] = (UInt16)(v & 0x3FF); \
  o[4] = (UInt16)((v >> 10) & 0x3FF); \
  o += 4 + ((v >> 18) & 4); \
  B <<= len; \
  C -= len; \
}

/* stream j alone, one symbol at a time with careful refill, up to the end of the chunk */
static SRes Decode_Syms_Tail(CDecState *st, unsigned j, UInt16 *o, const UInt16 *oEnd)
{
  const UInt16 * const table = st->table;
  for (; o < oEnd; o += kNumStreams)
  {
    if (st->hEnd[j] - st->hp[j] >= 8)
      REFILL_FAST(st->hp[j], st->bits[j], st->cnt[j])
    else
      while (st->cnt[j] <= 56)
      {
        if (st->hp[j] != st->hEnd[j])
          st->bits[j] |= (UInt64)*st->hp[j]++ << (56 - st->cnt[j]);
        else
          st->pad[j]++;
        st->cnt[j] += 8;
      }
    DECODE_SYM(st->bits[j], st->cnt[j], *o)
  }
  return SZ_OK;
}

/* iterations of the fast loop that stream j can do: each one reads at most 7 bytes (and
   needs 8 bytes to read) and gives at most 8 symbols, all before the end of the chunk */
static size_t Fast_Iters(const Byte *h, const Byte *hEnd, const UInt16 *o, const UInt16 *oEnd)
{
  size_t a, b;
  if (hEnd - h < 8 || o >= oEnd)
    return 0;
  a = (size_t)(hEnd - h - 8) / 7 + 1;
  b = ((size_t)(oEnd - o) + kNumStreams - 1) / kNumStreams / 8;
  return a < b ? a : b;
}

#define FAST_NONE  0
#define FAST_ONE   1
#define FAST_TWO   2

/* symbols of the elements i0 .. i0 + num - 1 (i0 % 16 == 0) into syms[]: element i of
   the chunk is in stream i % 4 */
static SRes Decode_Syms(CDecState *st, UInt16 *syms, size_t num)
{
  const UInt16 * const oEnd = syms + num;
  UInt16 *o0 = syms, *o1 = syms + 1, *o2 = syms + 2, *o3 = syms + 3;
  if (st->fast == FAST_ONE)
  {
    /* one symbol per lookup, the 4 streams in step: per iteration a refill of each stream
       and 4 lookups (16 symbols); safe iterations computed in advance as below */
    const UInt16 * const table = st->table;
    const Byte *h0 = st->hp[0], *h1 = st->hp[1], *h2 = st->hp[2], *h3 = st->hp[3];
    UInt64 b0 = st->bits[0], b1 = st->bits[1], b2 = st->bits[2], b3 = st->bits[3];
    unsigned c0 = st->cnt[0], c1 = st->cnt[1], c2 = st->cnt[2], c3 = st->cnt[3];
    UInt16 *o = syms;
    for (;;)
    {
      size_t it = (size_t)(oEnd - o) / 16;
      unsigned j;
      for (j = 0; j < kNumStreams; j++)
      {
        const Byte *h = j == 0 ? h0 : j == 1 ? h1 : j == 2 ? h2 : h3;
        const size_t a = st->hEnd[j] - h < 8 ? 0 : (size_t)(st->hEnd[j] - h - 8) / 7 + 1;
        if (it > a)
          it = a;
      }
      if (it == 0)
        break;
      do
      {
        REFILL_FAST(h0, b0, c0)
        REFILL_FAST(h1, b1, c1)
        REFILL_FAST(h2, b2, c2)
        REFILL_FAST(h3, b3, c3)
        DECODE_SYM(b0, c0, o[0])   DECODE_SYM(b1, c1, o[1])
        DECODE_SYM(b2, c2, o[2])   DECODE_SYM(b3, c3, o[3])
        DECODE_SYM(b0, c0, o[4])   DECODE_SYM(b1, c1, o[5])
        DECODE_SYM(b2, c2, o[6])   DECODE_SYM(b3, c3, o[7])
        DECODE_SYM(b0, c0, o[8])   DECODE_SYM(b1, c1, o[9])
        DECODE_SYM(b2, c2, o[10])  DECODE_SYM(b3, c3, o[11])
        DECODE_SYM(b0, c0, o[12])  DECODE_SYM(b1, c1, o[13])
        DECODE_SYM(b2, c2, o[14])  DECODE_SYM(b3, c3, o[15])
        o += 16;
      }
      while (--it);
    }
    st->hp[0] = h0; st->hp[1] = h1; st->hp[2] = h2; st->hp[3] = h3;
    st->bits[0] = b0; st->bits[1] = b1; st->bits[2] = b2; st->bits[3] = b3;
    st->cnt[0] = c0; st->cnt[1] = c1; st->cnt[2] = c2; st->cnt[3] = c3;
    o0 = o; o1 = o + 1; o2 = o + 2; o3 = o + 3;
  }
  else if (st->fast == FAST_TWO)
  {
    /* up to two symbols per lookup, each stream at its own pace */
    const UInt32 * const table2 = st->table2;
    /* fast loop: per iteration and stream, a refill (56..63 bits) and 4 lookups of at
       most 12 bits each; the number of safe iterations is computed in advance */
    const Byte *h0 = st->hp[0], *h1 = st->hp[1], *h2 = st->hp[2], *h3 = st->hp[3];
    UInt64 b0 = st->bits[0], b1 = st->bits[1], b2 = st->bits[2], b3 = st->bits[3];
    unsigned c0 = st->cnt[0], c1 = st->cnt[1], c2 = st->cnt[2], c3 = st->cnt[3];
    for (;;)
    {
      size_t it = Fast_Iters(h0, st->hEnd[0], o0, oEnd), t;
      t = Fast_Iters(h1, st->hEnd[1], o1, oEnd);  if (it > t) it = t;
      t = Fast_Iters(h2, st->hEnd[2], o2, oEnd);  if (it > t) it = t;
      t = Fast_Iters(h3, st->hEnd[3], o3, oEnd);  if (it > t) it = t;
      if (it == 0)
        break;
      do
      {
        REFILL_FAST(h0, b0, c0)
        REFILL_FAST(h1, b1, c1)
        REFILL_FAST(h2, b2, c2)
        REFILL_FAST(h3, b3, c3)
        /* 4 rounds written out: GCC -O2 does not unroll such a loop by itself */
        DECODE_SYM2(b0, c0, o0)  DECODE_SYM2(b1, c1, o1)  DECODE_SYM2(b2, c2, o2)  DECODE_SYM2(b3, c3, o3)
        DECODE_SYM2(b0, c0, o0)  DECODE_SYM2(b1, c1, o1)  DECODE_SYM2(b2, c2, o2)  DECODE_SYM2(b3, c3, o3)
        DECODE_SYM2(b0, c0, o0)  DECODE_SYM2(b1, c1, o1)  DECODE_SYM2(b2, c2, o2)  DECODE_SYM2(b3, c3, o3)
        DECODE_SYM2(b0, c0, o0)  DECODE_SYM2(b1, c1, o1)  DECODE_SYM2(b2, c2, o2)  DECODE_SYM2(b3, c3, o3)
      }
      while (--it);
    }
    st->hp[0] = h0; st->hp[1] = h1; st->hp[2] = h2; st->hp[3] = h3;
    st->bits[0] = b0; st->bits[1] = b1; st->bits[2] = b2; st->bits[3] = b3;
    st->cnt[0] = c0; st->cnt[1] = c1; st->cnt[2] = c2; st->cnt[3] = c3;
  }
  /* near the end of the chunk or of the streams */
  RINOK(Decode_Syms_Tail(st, 0, o0, oEnd))
  RINOK(Decode_Syms_Tail(st, 1, o1, oEnd))
  RINOK(Decode_Syms_Tail(st, 2, o2, oEnd))
  return Decode_Syms_Tail(st, 3, o3, oEnd);
}

/* r of element i (RB bits at bit i * RB, little-endian) */
#define LOAD_R64(i, RB)  (GetUi64(r + (((i) * (RB)) >> 3)) >> (((i) * (RB)) & 7))
/* (& 63): no warning for the branches of other RB that are never taken */
#define FIELD(v, t, RB)  ((UInt32)((v) >> (((t) * (RB)) & 63)) & (((UInt32)1 << (RB)) - 1))

/* Elements i0 .. i0 + num - 1 (i0 % 4 == 0) from syms[] and r. Groups of 4 elements
   read r with one or two 8-byte loads while they are before nSafe; after that, byte by
   byte inside the r plane (Read_R). */
#define DEFINE_JOIN(W, SK, K) \
static void Join_##W##_##SK##_##K(const CDecState *st, const UInt16 *syms, size_t i0, size_t num) \
{ \
  const Byte * const r = st->r; \
  Byte * const e = st->e; \
  const unsigned RB = R_BITS(W, K); \
  size_t i = 0; \
  for (; i + 4 <= num && i0 + i + 4 <= st->nSafe; i += 4) \
  { \
    const size_t ii = i0 + i; \
    UInt32 r0, r1, r2, r3; \
    if (RB == 8) \
    { \
      r0 = r[ii]; r1 = r[ii + 1]; r2 = r[ii + 2]; r3 = r[ii + 3]; \
    } \
    else if (4 * RB <= 56) \
    { \
      const UInt64 v = LOAD_R64(ii, RB); \
      r0 = FIELD(v, 0, RB); r1 = FIELD(v, 1, RB); r2 = FIELD(v, 2, RB); r3 = FIELD(v, 3, RB); \
    } \
    else \
    { \
      const UInt64 v0 = LOAD_R64(ii, RB); \
      const UInt64 v1 = LOAD_R64(ii + 2, RB); \
      r0 = FIELD(v0, 0, RB); r1 = FIELD(v0, 1, RB); r2 = FIELD(v1, 0, RB); r3 = FIELD(v1, 1, RB); \
    } \
    STORE_ELEM(W, ii + 0, JOIN(syms[i + 0], r0, SK, K)) \
    STORE_ELEM(W, ii + 1, JOIN(syms[i + 1], r1, SK, K)) \
    STORE_ELEM(W, ii + 2, JOIN(syms[i + 2], r2, SK, K)) \
    STORE_ELEM(W, ii + 3, JOIN(syms[i + 3], r3, SK, K)) \
  } \
  for (; i < num; i++) \
    STORE_ELEM(W, i0 + i, JOIN(syms[i], Read_R(r, st->rBytes, i0 + i, RB), SK, K)) \
}

#define STORE_ELEM(W, i, x) \
  if (W == 2) { SetUi16(e + (i) * 2, (UInt16)(x)) } else { SetUi32(e + (i) * 4, (UInt32)(x)) }

DEFINE_JOIN(2, 7, 0)    /* BF16 */
DEFINE_JOIN(2, 6, 1)
DEFINE_JOIN(2, 5, 2)
DEFINE_JOIN(2, 8, 0)    /* FP16 */
DEFINE_JOIN(2, 7, 1)
DEFINE_JOIN(2, 6, 2)
DEFINE_JOIN(4, 23, 0)   /* FP32 */
DEFINE_JOIN(4, 22, 1)
DEFINE_JOIN(4, 21, 2)

typedef void (*Func_Join)(const CDecState *st, const UInt16 *syms, size_t i0, size_t num);


/* ---------- AVX2 join (BF16, FP16, FP32) ---------- */

#ifdef MY_CPU_X86_OR_AMD64
  #if defined(__clang__) && (__clang_major__ >= 4) \
      || defined(Z7_GCC_VERSION) && (Z7_GCC_VERSION >= 40900)
    #define FLOAT_PLANE_USE_AVX2
    #define FLOAT_PLANE_ATTRIB_AVX2  __attribute__((__target__("avx2")))
  #elif defined(_MSC_VER) && (_MSC_VER >= 1900)
    #define FLOAT_PLANE_USE_AVX2
    #define FLOAT_PLANE_ATTRIB_AVX2
  #endif
#endif

#ifdef FLOAT_PLANE_USE_AVX2

#include <immintrin.h>
#if defined(__clang__)
#include <avxintrin.h>
#include <avx2intrin.h>
#endif

/* 16 elements per iteration:
   - 16 symbols: one 32-byte load;
   - 16 fields of r (RB = 8 - k bits, 16 * RB <= 128 bits): one 16-byte load at a byte
     boundary (16 * RB is a multiple of 8), copied to both 128-bit lanes; vpshufb puts in
     lane t the 2 bytes that hold field t; the per-lane right shift by sh_t (AVX2 has no
     vpsrlvw) is a multiplication by 2^(8 - sh_t) and a right shift by 8;
   - JOIN with constant shifts, one 32-byte store.
   The rest (fewer than 16 elements, or the last 16 bytes of r) goes to the scalar join. */
#define DEFINE_JOIN_AVX2(SK, K) \
static FLOAT_PLANE_ATTRIB_AVX2 void JoinAvx2_2_##SK##_##K(const CDecState *st, const UInt16 *syms, \
    size_t i0, size_t num) \
{ \
  const unsigned RB = R_BITS(2, K); \
  const Byte * const r = st->r; \
  Byte * const e = st->e; \
  const size_t rBytes = st->rBytes; \
  Byte shuf[32]; \
  UInt16 mul[16]; \
  unsigned t; \
  size_t i = 0; \
  __m256i vShuf, vMul; \
  for (t = 0; t < 16; t++) \
  { \
    const unsigned pos = t * RB, b = pos >> 3; \
    shuf[t * 2] = (Byte)b; \
    shuf[t * 2 + 1] = (Byte)(b + 1 < 16 ? b + 1 : 0x80); /* 0x80: zero */ \
    mul[t] = (UInt16)(1u << (8 - (pos & 7))); \
  } \
  vShuf = _mm256_loadu_si256((const __m256i *)(const void *)shuf); \
  vMul = _mm256_loadu_si256((const __m256i *)(const void *)mul); \
  { \
    const __m256i vMaskR = _mm256_set1_epi16((short)((1u << RB) - 1)); \
    const __m256i vMaskLow = _mm256_set1_epi16((short)((1u << (SK)) - 1)); \
    for (; i + 16 <= num; i += 16) \
    { \
      const size_t ii = i0 + i; \
      const size_t rPos = (ii * RB) >> 3; \
      __m256i vr, vs, vx; \
      if (rPos + 16 > rBytes) \
        break; \
      vr = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)(const void *)(r + rPos))); \
      vr = _mm256_shuffle_epi8(vr, vShuf); \
      vr = _mm256_and_si256(_mm256_srli_epi16(_mm256_mullo_epi16(vr, vMul), 8), vMaskR); \
      vs = _mm256_loadu_si256((const __m256i *)(const void *)(syms + i)); \
      vx = _mm256_or_si256( \
          _mm256_or_si256(_mm256_and_si256(vr, vMaskLow), _mm256_slli_epi16(vs, SK)), \
          _mm256_slli_epi16(_mm256_srli_epi16(vr, SK), (SK) + 8 + (K))); \
      _mm256_storeu_si256((__m256i *)(void *)(e + ii * 2), vx); \
    } \
  } \
  if (i < num) \
    Join_2_##SK##_##K(st, syms + i, i0 + i, num - i); \
}

DEFINE_JOIN_AVX2(7, 0)    /* BF16 */
DEFINE_JOIN_AVX2(6, 1)
DEFINE_JOIN_AVX2(5, 2)
DEFINE_JOIN_AVX2(8, 0)    /* FP16 */
DEFINE_JOIN_AVX2(7, 1)
DEFINE_JOIN_AVX2(6, 2)

/* FP32, 8 elements per iteration:
   - 8 symbols: one 16-byte load, zero-extended to 32 bits;
   - 8 fields of r (RB = 24 - k bits, 8 * RB is a multiple of 8): elements 0..3 from a
     16-byte load at the first byte of the group, elements 4..7 from a 16-byte load at the
     byte of field 4 (each field with its shift fits in 4 bytes: 7 + 24 <= 32); vpshufb puts
     the 4 bytes of field t in lane t, vpsrlvd shifts it by sh_t;
   - JOIN with constant shifts, one 32-byte store. */
#define DEFINE_JOIN_AVX2_4(SK, K) \
static FLOAT_PLANE_ATTRIB_AVX2 void JoinAvx2_4_##SK##_##K(const CDecState *st, const UInt16 *syms, \
    size_t i0, size_t num) \
{ \
  const unsigned RB = R_BITS(4, K); \
  const unsigned b4 = (4 * RB) >> 3;  /* byte of field 4 in the group */ \
  const Byte * const r = st->r; \
  Byte * const e = st->e; \
  const size_t rBytes = st->rBytes; \
  Byte shuf[32]; \
  UInt32 sh[8]; \
  unsigned t; \
  size_t i = 0; \
  __m256i vShuf, vSh; \
  for (t = 0; t < 8; t++) \
  { \
    const unsigned pos = t * RB, b = (pos >> 3) - (t < 4 ? 0 : b4); \
    unsigned j; \
    for (j = 0; j < 4; j++) \
      shuf[t * 4 + j] = (Byte)(b + j); \
    sh[t] = pos & 7; \
  } \
  vShuf = _mm256_loadu_si256((const __m256i *)(const void *)shuf); \
  vSh = _mm256_loadu_si256((const __m256i *)(const void *)sh); \
  { \
    const __m256i vMaskR = _mm256_set1_epi32((int)((1u << RB) - 1)); \
    const __m256i vMaskLow = _mm256_set1_epi32((int)((1u << (SK)) - 1)); \
    for (; i + 8 <= num; i += 8) \
    { \
      const size_t ii = i0 + i; \
      const size_t rPos = (ii * RB) >> 3; \
      __m256i vr, vs, vx; \
      if (rPos + b4 + 16 > rBytes) \
        break; \
      vr = _mm256_inserti128_si256(_mm256_castsi128_si256( \
          _mm_loadu_si128((const __m128i *)(const void *)(r + rPos))), \
          _mm_loadu_si128((const __m128i *)(const void *)(r + rPos + b4)), 1); \
      vr = _mm256_shuffle_epi8(vr, vShuf); \
      vr = _mm256_and_si256(_mm256_srlv_epi32(vr, vSh), vMaskR); \
      vs = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(const void *)(syms + i))); \
      vx = _mm256_or_si256( \
          _mm256_or_si256(_mm256_and_si256(vr, vMaskLow), _mm256_slli_epi32(vs, SK)), \
          _mm256_slli_epi32(_mm256_srli_epi32(vr, SK), (SK) + 8 + (K))); \
      _mm256_storeu_si256((__m256i *)(void *)(e + ii * 4), vx); \
    } \
  } \
  if (i < num) \
    Join_4_##SK##_##K(st, syms + i, i0 + i, num - i); \
}

DEFINE_JOIN_AVX2_4(23, 0)   /* FP32 */
DEFINE_JOIN_AVX2_4(22, 1)
DEFINE_JOIN_AVX2_4(21, 2)

#endif

/* ---------- NEON join (ARM64: BF16, FP16, FP32) ---------- */

#if defined(MY_CPU_ARM64) && (defined(__ARM_NEON) || defined(_MSC_VER))
#define FLOAT_PLANE_USE_NEON

#include <arm_neon.h>

/* 16 elements of 2 bytes per iteration, as the AVX2 join: one 16-byte load of r (16 * RB
   <= 128 bits, at a byte boundary), vqtbl1q puts in lane t the 2 bytes that hold field t
   (indices >= 16 give 0), vshlq with negative counts shifts each lane right by sh_t. */
#define DEFINE_JOIN_NEON(SK, K) \
static void JoinNeon_2_##SK##_##K(const CDecState *st, const UInt16 *syms, size_t i0, size_t num) \
{ \
  const unsigned RB = R_BITS(2, K); \
  const Byte * const r = st->r; \
  Byte * const e = st->e; \
  const size_t rBytes = st->rBytes; \
  Byte idx[32]; \
  int16_t sh[16]; \
  unsigned t; \
  size_t i = 0; \
  for (t = 0; t < 16; t++) \
  { \
    const unsigned pos = t * RB, b = pos >> 3; \
    idx[t * 2] = (Byte)b; \
    idx[t * 2 + 1] = (Byte)(b + 1); /* 16: zero */ \
    sh[t] = (int16_t)-(int)(pos & 7); \
  } \
  { \
    const uint8x16_t vIdx0 = vld1q_u8(idx), vIdx1 = vld1q_u8(idx + 16); \
    const int16x8_t vSh0 = vld1q_s16(sh), vSh1 = vld1q_s16(sh + 8); \
    const uint16x8_t vMaskR = vdupq_n_u16((UInt16)((1u << RB) - 1)); \
    const uint16x8_t vMaskLow = vdupq_n_u16((UInt16)((1u << (SK)) - 1)); \
    for (; i + 16 <= num; i += 16) \
    { \
      const size_t ii = i0 + i; \
      const size_t rPos = (ii * RB) >> 3; \
      uint8x16_t vr; \
      uint16x8_t r0, r1, x0, x1; \
      if (rPos + 16 > rBytes) \
        break; \
      vr = vld1q_u8(r + rPos); \
      r0 = vandq_u16(vshlq_u16(vreinterpretq_u16_u8(vqtbl1q_u8(vr, vIdx0)), vSh0), vMaskR); \
      r1 = vandq_u16(vshlq_u16(vreinterpretq_u16_u8(vqtbl1q_u8(vr, vIdx1)), vSh1), vMaskR); \
      x0 = vorrq_u16(vandq_u16(r0, vMaskLow), vshlq_n_u16(vld1q_u16(syms + i), SK)); \
      x1 = vorrq_u16(vandq_u16(r1, vMaskLow), vshlq_n_u16(vld1q_u16(syms + i + 8), SK)); \
      if ((SK) + 8 + (K) < 16) /* else r has no bits above the symbol */ \
      { \
        x0 = vorrq_u16(x0, vshlq_n_u16(vshrq_n_u16(r0, SK), ((SK) + 8 + (K)) & 15)); \
        x1 = vorrq_u16(x1, vshlq_n_u16(vshrq_n_u16(r1, SK), ((SK) + 8 + (K)) & 15)); \
      } \
      vst1q_u16((UInt16 *)(void *)(e + ii * 2), x0); \
      vst1q_u16((UInt16 *)(void *)(e + ii * 2 + 16), x1); \
    } \
  } \
  if (i < num) \
    Join_2_##SK##_##K(st, syms + i, i0 + i, num - i); \
}

/* 8 elements of 4 bytes per iteration: the RB (22..24) bytes of r from a 32-byte table
   (two 16-byte loads), vqtbl2q puts in lane t the 4 bytes that hold field t. */
#define DEFINE_JOIN_NEON_4(SK, K) \
static void JoinNeon_4_##SK##_##K(const CDecState *st, const UInt16 *syms, size_t i0, size_t num) \
{ \
  const unsigned RB = R_BITS(4, K); \
  const Byte * const r = st->r; \
  Byte * const e = st->e; \
  const size_t rBytes = st->rBytes; \
  Byte idx[32]; \
  int32_t sh[8]; \
  unsigned t; \
  size_t i = 0; \
  for (t = 0; t < 8; t++) \
  { \
    const unsigned pos = t * RB; \
    unsigned j; \
    for (j = 0; j < 4; j++) \
      idx[t * 4 + j] = (Byte)((pos >> 3) + j); \
    sh[t] = -(int32_t)(pos & 7); \
  } \
  { \
    const uint8x16_t vIdx0 = vld1q_u8(idx), vIdx1 = vld1q_u8(idx + 16); \
    const int32x4_t vSh0 = vld1q_s32(sh), vSh1 = vld1q_s32(sh + 4); \
    const uint32x4_t vMaskR = vdupq_n_u32(((UInt32)1 << RB) - 1); \
    const uint32x4_t vMaskLow = vdupq_n_u32(((UInt32)1 << (SK)) - 1); \
    for (; i + 8 <= num; i += 8) \
    { \
      const size_t ii = i0 + i; \
      const size_t rPos = (ii * RB) >> 3; \
      uint8x16x2_t vr; \
      uint16x8_t vs; \
      uint32x4_t r0, r1, x0, x1; \
      if (rPos + 32 > rBytes) \
        break; \
      vr.val[0] = vld1q_u8(r + rPos); \
      vr.val[1] = vld1q_u8(r + rPos + 16); \
      r0 = vandq_u32(vshlq_u32(vreinterpretq_u32_u8(vqtbl2q_u8(vr, vIdx0)), vSh0), vMaskR); \
      r1 = vandq_u32(vshlq_u32(vreinterpretq_u32_u8(vqtbl2q_u8(vr, vIdx1)), vSh1), vMaskR); \
      vs = vld1q_u16(syms + i); \
      x0 = vorrq_u32(vorrq_u32(vandq_u32(r0, vMaskLow), vshlq_n_u32(vmovl_u16(vget_low_u16(vs)), SK)), \
          vshlq_n_u32(vshrq_n_u32(r0, SK), (SK) + 8 + (K))); \
      x1 = vorrq_u32(vorrq_u32(vandq_u32(r1, vMaskLow), vshlq_n_u32(vmovl_u16(vget_high_u16(vs)), SK)), \
          vshlq_n_u32(vshrq_n_u32(r1, SK), (SK) + 8 + (K))); \
      vst1q_u32((UInt32 *)(void *)(e + ii * 4), x0); \
      vst1q_u32((UInt32 *)(void *)(e + ii * 4 + 16), x1); \
    } \
  } \
  if (i < num) \
    Join_4_##SK##_##K(st, syms + i, i0 + i, num - i); \
}

DEFINE_JOIN_NEON(7, 0)    /* BF16 */
DEFINE_JOIN_NEON(6, 1)
DEFINE_JOIN_NEON(5, 2)
DEFINE_JOIN_NEON(8, 0)    /* FP16 */
DEFINE_JOIN_NEON(7, 1)
DEFINE_JOIN_NEON(6, 2)
DEFINE_JOIN_NEON_4(23, 0) /* FP32 */
DEFINE_JOIN_NEON_4(22, 1)
DEFINE_JOIN_NEON_4(21, 2)

#endif

/* elements i0 .. n-1 (i0 % 16 == 0, the streams at element i0) */
static SRes Decode_Elements(CDecState *st, Func_Join join, size_t i0)
{
  UInt16 syms[kChunk + 8];   /* + 8: the second symbol of a lookup past the end */
  for (; i0 < st->n; i0 += kChunk)
  {
    const size_t num = st->n - i0 < kChunk ? st->n - i0 : kChunk;
    RINOK(Decode_Syms(st, syms, num))
    join(st, syms, i0, num);
  }
  return SZ_OK;
}

/* the join of each (type, k): index = type * 3 + k */
static const Func_Join g_Joins[9] =
{
  Join_2_7_0, Join_2_6_1, Join_2_5_2,      /* BF16 */
  Join_2_8_0, Join_2_7_1, Join_2_6_2,      /* FP16 */
  Join_4_23_0, Join_4_22_1, Join_4_21_2    /* FP32 */
};

#ifdef FLOAT_PLANE_USE_AVX2
static const Func_Join g_JoinsAvx2[9] =
{
  JoinAvx2_2_7_0, JoinAvx2_2_6_1, JoinAvx2_2_5_2,
  JoinAvx2_2_8_0, JoinAvx2_2_7_1, JoinAvx2_2_6_2,
  JoinAvx2_4_23_0, JoinAvx2_4_22_1, JoinAvx2_4_21_2
};
#endif

#ifdef FLOAT_PLANE_USE_NEON
static const Func_Join g_JoinsNeon[9] =
{
  JoinNeon_2_7_0, JoinNeon_2_6_1, JoinNeon_2_5_2,
  JoinNeon_2_8_0, JoinNeon_2_7_1, JoinNeon_2_6_2,
  JoinNeon_4_23_0, JoinNeon_4_22_1, JoinNeon_4_21_2
};
#endif

static BoolInt g_SimdEnabled = True;

void FloatPlane_SetSimd(BoolInt enable)
{
  g_SimdEnabled = enable;
}

/* The fast loop with two symbols per lookup is worth it when enough lookups give two
   symbols (short codes: exponents of floats); with long codes (bytes of quantized
   weights, ~8 bits) its extra work per lookup makes it slower than one symbol per lookup. */
#ifndef TWO_MIN
#define TWO_MIN 1024
#endif
#define kTwoMin  TWO_MIN   /* entries of table2 with two symbols, of 4096 */

static void Set_Tables(CDecState *st, const UInt16 *table, UInt32 *table2)
{
  st->table = table;
  st->table2 = table2;
  st->fast = FAST_NONE;
  if (g_SimdEnabled)
    st->fast = Build_Table2(table, table2) >= kTwoMin ? FAST_TWO : FAST_ONE;
}

static Func_Join Get_Join(unsigned index)
{
  #ifdef FLOAT_PLANE_USE_AVX2
  if (g_SimdEnabled && CPU_IsSupported_AVX2())
    return g_JoinsAvx2[index];
  #endif
  #ifdef FLOAT_PLANE_USE_NEON
  if (g_SimdEnabled)   /* NEON: always in ARM64 */
    return g_JoinsNeon[index];
  #endif
  return g_Joins[index];
}


/* ---------- Decoder of quantized blocks (mode 3) ---------- */

/* field symbols i0 .. i0 + num - 1 to their bytes in the structs */
static void Join_QBytes(const CDecState *st, const UInt16 *syms, size_t i0, size_t num)
{
  const CQField *f = st->qf;
  const unsigned w = f->len, step = f->step, S = st->qS;
  const size_t s = i0 / w;
  unsigned o = (unsigned)(i0 - s * w);
  Byte *p = st->e + s * S + f->start;
  const UInt16 * const symsEnd = syms + num;
  while (syms != symsEnd)
  {
    /* the rest of the struct, or of the chunk */
    unsigned m = w - o, j;
    if (m > (size_t)(symsEnd - syms))
      m = (unsigned)(symsEnd - syms);
    if (step == 1)
    {
      Byte *q = p + o;
      for (j = 0; j < m; j++)
        q[j] = (Byte)syms[j];
    }
    else
      for (j = 0; j < m; j++)
        p[(o + j) * step] = (Byte)syms[j];
    syms += m;
    o += m;
    if (o == w)
    {
      o = 0;
      p += S;
    }
  }
}

/* Q5_K / Q6_K: the 256 values of each struct to qs/ql and qh. The chunks have whole
   structs: i0 and num are multiples of 256 (kChunk is, and n * 256 is). The values are
   copied to a local array first: then the compiler knows that the stores to the struct
   do not change them, and vectorizes the loops. */
static void Join_QBits(const CDecState *st, const UInt16 *q, size_t i0, size_t num)
{
  const unsigned S = st->qS;
  const int q6 = (st->qf->kind == QF_Q6);
  Byte *p = st->e + (i0 >> 8) * S;
  size_t s;
  for (s = 0; s < (num >> 8); s++, p += S, q += 256)
  {
    UInt16 v[256];
    Byte hb[64];
    unsigned l, c;
    memcpy(v, q, sizeof(v));
    if (q6)
    {
      /* half c: ql[64c + l] = values 128c + l (low nibble), 128c + 64 + l (high nibble);
         ql[64c + 32 + l] = values 128c + 32 + l, 128c + 96 + l; qh[32c + l]: bits 4-5
         of the values 128c + 32t + l at bit 2t */
      for (c = 0; c < 2; c++)
      {
        const UInt16 *w = v + 128 * c;
        Byte *ql = p + 64 * c, *qh = hb + 32 * c;
        for (l = 0; l < 32; l++)
        {
          ql[l]      = (Byte)((w[l]      & 15) | ((w[l + 64] & 15) << 4));
          ql[l + 32] = (Byte)((w[l + 32] & 15) | ((w[l + 96] & 15) << 4));
          qh[l] = (Byte)((w[l] >> 4) | ((w[l + 32] >> 4) << 2)
              | ((w[l + 64] >> 4) << 4) | ((w[l + 96] >> 4) << 6));
        }
      }
      memcpy(p + 128, hb, 64);
    }
    else
    {
      /* qs[32c + l] = values 64c + l (low nibble), 64c + 32 + l (high nibble); qh[l]: bit 4
         of the value 64c + 32h + l at bit 2c + h */
      for (l = 0; l < 32; l++)
        hb[l] = 0;
      for (c = 0; c < 4; c++)
        for (l = 0; l < 32; l++)
        {
          const unsigned x0 = v[64 * c + l], x1 = v[64 * c + 32 + l];
          p[48 + 32 * c + l] = (Byte)((x0 & 15) | ((x1 & 15) << 4));
          hb[l] = (Byte)(hb[l] | (((x0 >> 4) | ((x1 >> 4) << 1)) << (2 * c)));
        }
      memcpy(p + 16, hb, 32);
    }
  }
}

/* A field of contiguous bytes, a multiple of 16 per struct (qs of Q4_0, Q8_0, Q4_K),
   with one symbol per lookup: the symbols go straight to the structs, 16 per round (4
   from each stream, in step), without the buffer of Decode_Elements(). *done = the
   symbols decoded (a multiple of 16): the rest is left to Decode_Elements(). */
static SRes Decode_QDirect(CDecState *st, size_t *done)
{
  const UInt16 * const table = st->table;
  const unsigned S = st->qS, w = st->qf->len;
  const Byte *h0 = st->hp[0], *h1 = st->hp[1], *h2 = st->hp[2], *h3 = st->hp[3];
  UInt64 b0 = st->bits[0], b1 = st->bits[1], b2 = st->bits[2], b3 = st->bits[3];
  unsigned c0 = st->cnt[0], c1 = st->cnt[1], c2 = st->cnt[2], c3 = st->cnt[3];
  Byte *o = st->e + st->qf->start;
  unsigned pos = 0;   /* symbols done in the current struct */
  size_t i = 0;
  for (;;)
  {
    size_t it = (st->n - i) / 16;
    unsigned j;
    for (j = 0; j < kNumStreams; j++)
    {
      const Byte *h = j == 0 ? h0 : j == 1 ? h1 : j == 2 ? h2 : h3;
      const size_t a = st->hEnd[j] - h < 8 ? 0 : (size_t)(st->hEnd[j] - h - 8) / 7 + 1;
      if (it > a)
        it = a;
    }
    if (it == 0)
      break;
    i += it * 16;
    do
    {
      REFILL_FAST(h0, b0, c0)
      REFILL_FAST(h1, b1, c1)
      REFILL_FAST(h2, b2, c2)
      REFILL_FAST(h3, b3, c3)
      /* the symbols of byte fields are < 256 */
      #define DECODE_BYTE(B, C, dst)  { unsigned sym_; DECODE_SYM(B, C, sym_)  dst = (Byte)sym_; }
      DECODE_BYTE(b0, c0, o[0])   DECODE_BYTE(b1, c1, o[1])
      DECODE_BYTE(b2, c2, o[2])   DECODE_BYTE(b3, c3, o[3])
      DECODE_BYTE(b0, c0, o[4])   DECODE_BYTE(b1, c1, o[5])
      DECODE_BYTE(b2, c2, o[6])   DECODE_BYTE(b3, c3, o[7])
      DECODE_BYTE(b0, c0, o[8])   DECODE_BYTE(b1, c1, o[9])
      DECODE_BYTE(b2, c2, o[10])  DECODE_BYTE(b3, c3, o[11])
      DECODE_BYTE(b0, c0, o[12])  DECODE_BYTE(b1, c1, o[13])
      DECODE_BYTE(b2, c2, o[14])  DECODE_BYTE(b3, c3, o[15])
      o += 16;
      pos += 16;
      if (pos == w)
      {
        pos = 0;
        o += S - w;
      }
    }
    while (--it);
  }
  st->hp[0] = h0; st->hp[1] = h1; st->hp[2] = h2; st->hp[3] = h3;
  st->bits[0] = b0; st->bits[1] = b1; st->bits[2] = b2; st->bits[3] = b3;
  st->cnt[0] = c0; st->cnt[1] = c1; st->cnt[2] = c2; st->cnt[3] = c3;
  *done = i;
  return SZ_OK;
}

static SRes Decode_Quant(const Byte *src, size_t srcLen, Byte *dest, size_t outLen, size_t *srcUsed)
{
  UInt16 table[(size_t)1 << kTableBits];
  UInt32 table2[(size_t)1 << kTableBits];
  const Byte * const srcEnd = src + srcLen;
  CQuantHeader h;
  const CQType *qt;
  const Byte *p, *fh;
  unsigned fi, j;

  RINOK(Parse_QuantHeader(src, srcLen, outLen, &h))
  if (srcLen < h.headerSize)
    return SZ_ERROR_INPUT_EOF;
  qt = &g_QTypes[h.type];
  p = src + kQuantHeaderSize;
  memcpy(dest, p, h.phase);                         p += h.phase;
  memcpy(dest + outLen - h.tail, p, h.tail);
  fh = h.fieldHeaders;
  p = src + h.headerSize;   /* the streams of the first field */

  for (fi = 0; fi < qt->numFields; fi++)
  {
    const CQField *f = &qt->f[fi];
    const unsigned numSyms = QF_NUM_SYMS(f);
    const size_t num = h.n * QF_WIDTH(f);
    size_t sizes[kNumStreams];
    const Byte *hStart[kNumStreams];
    size_t huffTotal;
    CDecState st;

    if (!Build_Table(fh, numSyms, table))
      return SZ_ERROR_DATA;
    fh += numSyms / 2;
    RINOK(Get_StreamSizes(fh, num, sizes, &huffTotal))
    fh += 4 * kNumStreams;
    if ((size_t)(srcEnd - p) < huffTotal)
      return SZ_ERROR_INPUT_EOF;
    for (j = 0; j < kNumStreams; j++)
    {
      hStart[j] = st.hp[j] = p;
      p += sizes[j];
      st.hEnd[j] = p;
      st.bits[j] = 0;
      st.cnt[j] = 0;
      st.pad[j] = 0;
    }
    Set_Tables(&st, table, table2);
    st.r = NULL;
    st.rBytes = 0;
    st.nSafe = 0;
    st.n = num;
    st.e = dest + h.phase;
    st.qf = f;
    st.qS = qt->size;
    {
      size_t done = 0;
      if (st.fast == FAST_ONE && f->kind == QF_BYTES && f->step == 1 && (f->len & 15) == 0)
        RINOK(Decode_QDirect(&st, &done))
      RINOK(Decode_Elements(&st, f->kind == QF_BYTES ? Join_QBytes : Join_QBits, done))
    }
    /* as in split blocks: each stream fills exactly its declared size */
    for (j = 0; j < kNumStreams; j++)
    {
      const UInt64 used = ((UInt64)(size_t)(st.hp[j] - hStart[j]) + st.pad[j]) * 8 - st.cnt[j];
      if (((used + 7) >> 3) != sizes[j])
        return SZ_ERROR_DATA;
    }
  }
  *srcUsed = (size_t)(p - src);
  return SZ_OK;
}


SRes FloatPlane_DecodeBlock(const Byte *src, size_t srcLen, Byte *dest, size_t destLen, size_t *srcUsed,
    const CFloatPlaneLz *lz)
{
  size_t outLen;
  const Byte *p = src;
  const Byte * const srcEnd = src + srcLen;

  *srcUsed = 0;
  RINOK(FloatPlane_GetBlockOutLen(src, srcLen, &outLen))
  if (outLen > destLen)
    return SZ_ERROR_DATA;
  p += 5;

  if (src[0] == MODE_STORED)
  {
    if ((size_t)(srcEnd - p) < outLen)
      return SZ_ERROR_INPUT_EOF;
    memcpy(dest, p, outLen);
    *srcUsed = 5 + outLen;
    return SZ_OK;
  }

  if (src[0] == MODE_LZ)
  {
    size_t blockSize;
    SRes res;
    RINOK(FloatPlane_GetBlockSize(src, srcLen, &blockSize))
    if (srcLen < blockSize)
      return SZ_ERROR_INPUT_EOF;
    if (!lz || !lz->Decompress)
      return SZ_ERROR_UNSUPPORTED;
    res = lz->Decompress(lz->ctx, src + kLzHeaderSize, blockSize - kLzHeaderSize, dest, outLen);
    if (res != SZ_OK)
      return res == SZ_ERROR_MEM ? SZ_ERROR_MEM : SZ_ERROR_DATA;
    *srcUsed = blockSize;
    return SZ_OK;
  }

  if (src[0] == MODE_QUANT)
    return Decode_Quant(src, srcLen, dest, outLen, srcUsed);
  if (src[0] == FLOAT_QUANT_MODE)
    return FloatQuant_Decode(src, srcLen, dest, outLen, srcUsed, g_SimdEnabled);

  {
    UInt16 table[(size_t)1 << kTableBits];
    UInt32 table2[(size_t)1 << kTableBits];
    CSplitHeader hd;
    CDecState st;
    unsigned j;
    size_t n, huffTotal, rBytes;
    size_t sizes[kNumStreams];
    const Byte *hStart[kNumStreams];
    SRes res;

    RINOK(Parse_SplitHeader(src, srcLen, outLen, &hd))
    if (srcLen < hd.headerSize)
      return SZ_ERROR_INPUT_EOF;
    n = hd.n;
    p = src + kSplitHeaderSize;
    memcpy(dest, p, hd.phase);                       p += hd.phase;
    memcpy(dest + outLen - hd.tail, p, hd.tail);     p += hd.tail;
    if (!Build_Table(p, 256u << hd.k, table))
      return SZ_ERROR_DATA;
    p += LENS_BYTES(hd.k);
    RINOK(Get_StreamSizes(p, n, sizes, &huffTotal))
    p += 4 * kNumStreams;
    rBytes = R_BYTES(n, R_BITS(hd.w, hd.k));
    if ((size_t)(srcEnd - p) < huffTotal || (size_t)(srcEnd - p) - huffTotal < rBytes)
      return SZ_ERROR_INPUT_EOF;
    for (j = 0; j < kNumStreams; j++)
    {
      hStart[j] = st.hp[j] = p;
      p += sizes[j];
      st.hEnd[j] = p;
      st.bits[j] = 0;
      st.cnt[j] = 0;
      st.pad[j] = 0;
    }
    Set_Tables(&st, table, table2);
    st.r = p;
    st.rBytes = rBytes;
    st.n = n;
    st.e = dest + hd.phase;
    /* r (RB bits per element at bit i * RB, little-endian) is read with 8-byte loads at
       the bit position of an element: the fast loop stops at nSafe, the first element
       whose 8 bytes would pass the end of the r plane */
    st.nSafe = 0;
    if (rBytes >= 8)
    {
      const size_t m = (size_t)(((UInt64)(rBytes - 8) * 8) / R_BITS(hd.w, hd.k)) + 1;
      st.nSafe = m < n ? m : n;
    }

    res = Decode_Elements(&st, Get_Join(hd.type * 3 + hd.k), 0);
    RINOK(res)

    /* each stream: the bits used by its codes (all loaded minus those left) fill exactly
       its declared size */
    for (j = 0; j < kNumStreams; j++)
    {
      const UInt64 used = ((UInt64)(size_t)(st.hp[j] - hStart[j]) + st.pad[j]) * 8 - st.cnt[j];
      if (((used + 7) >> 3) != sizes[j])
        return SZ_ERROR_DATA;
    }
    *srcUsed = (size_t)(st.r + rBytes - src);
    return SZ_OK;
  }
}
