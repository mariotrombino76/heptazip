/* FloatQuant.c -- context model for the quantized structs of GGUF files (FLOAT mode 4)
7zip-modern (design 0008) : Public domain

The fields of the K-quant structs (Q4_K, Q5_K, Q6_K) are coded with rANS (8 or 32
interleaved states) and one frequency table per context: the context of a quantized value is a
property of its sub-block that is decoded before it (the zero point of Q4_K / Q5_K, the
scale of Q6_K), the context of a min of Q4_K / Q5_K is its scale. The block format is
described in FloatPlane.h. rANS: J. Duda, "Asymmetric numeral systems" (2013); the
encoder with reciprocals as in F. Giesen's rans_byte.h (public domain). */

#include "Precomp.h"

#include <string.h>

#include "CpuArch.h"
#include "FloatQuant.h"

#define kHeaderSize   (1 + 4 + 2)       /* mode, outLen, qtype, phase */
#define kMaxStates    64                /* symbol i of a field is coded by state i % N */
#define kRansL        ((UInt32)1 << 15) /* states are in [kRansL, 2^31) */
#define kLevelSteps   8                 /* levels of the frequency tables per octave */
#define kMaxLevel     (1 + kLevelSteps * 23)
#define kMaxGamma     9                 /* bits of a level delta code (zigzag + 1 < 512) */
#define kGather       32                /* structs per chunk: 32 * width is a multiple of N */
#define kMaxWidth     256               /* symbols of a field per struct */
#define kMaxTabSlots  ((size_t)1 << 16) /* decoder: numCtx << P of every field fits */
#define kTabSlack     4                 /* the AVX2 decoder reads 4 bytes at each slot */

/* kinds of fields */
#define QM_BYTES   0   /* bytes start + j * step (j < len) of the struct; context j or 0 */
#define QM_SCALE6  1   /* the 8 scales (6 bits) of the K-quant scales[12] at start */
#define QM_MIN6    2   /* the 8 mins (6 bits): context = scale >> 2 */
#define QM_Q4K     3   /* the 256 values of Q4_K: context = zero point, scale >> 4 */
#define QM_Q5K     4   /* the 256 values of Q5_K: as Q4_K */
#define QM_Q6K     5   /* the 256 values of Q6_K: context = |scale| >> 3 */

/* states N of a field: 64 for the values (the bulk of the data: 8 vectors of 8 states
   in flight in the AVX2 decoder, to hide the latency of its gathers), 8 for the others */
#define QM_STATES(f)  ((f)->kind >= QM_Q4K ? 64u : 8u)

typedef struct
{
  Byte kind;
  Byte start, len, step;   /* QM_BYTES */
  Byte logSyms;            /* alphabet of 1 << logSyms symbols */
  Byte prec;               /* P: the frequencies of a context sum to 1 << P */
  UInt16 numCtx;
  UInt16 width;            /* symbols per struct */
} CQMField;

typedef struct
{
  unsigned size;
  unsigned numFields;
  CQMField f[FLOAT_QUANT_MAX_FIELDS];
} CQMType;

#define kFirstQType  2

/* index = qtype - 2. The fields are coded in this order: the contexts of a field are
   computed from the fields before it. */
static const CQMType g_QMTypes[] =
{
  /* Q4_K: d, dmin, scales[12], qs[128] */
  { 144, 5, { { QM_BYTES, 0, 2, 2, 8, 12, 2, 2 }, { QM_BYTES, 1, 2, 2, 8, 12, 2, 2 },
              { QM_SCALE6, 4, 0, 0, 6, 10, 1, 8 }, { QM_MIN6, 4, 0, 0, 6, 10, 16, 8 },
              { QM_Q4K, 16, 0, 0, 4, 9, 128, 256 } } },
  /* Q5_K: d, dmin, scales[12], qh[32], qs[128] */
  { 176, 5, { { QM_BYTES, 0, 2, 2, 8, 12, 2, 2 }, { QM_BYTES, 1, 2, 2, 8, 12, 2, 2 },
              { QM_SCALE6, 4, 0, 0, 6, 10, 1, 8 }, { QM_MIN6, 4, 0, 0, 6, 10, 16, 8 },
              { QM_Q5K, 16, 0, 0, 5, 9, 128, 256 } } },
  /* Q6_K: ql[128], qh[64], scales[16], d */
  { 210, 4, { { QM_BYTES, 192, 16, 1, 8, 12, 1, 16 }, { QM_BYTES, 208, 1, 1, 8, 12, 1, 1 },
              { QM_BYTES, 209, 1, 1, 8, 12, 1, 1 }, { QM_Q6K, 0, 0, 0, 6, 11, 16, 256 } } }
};
#define kNumQMTypes  (sizeof(g_QMTypes) / sizeof(g_QMTypes[0]))


/* ---------- Fields of the structs ---------- */

/* scale j (6 bits) and min j of the K-quant scales[12] q (get_scale_min_k4 of ggml) */
#define KQ_SCALE(q, j)  ((j) < 4 ? (q)[j] & 63u : ((q)[(j) + 4] & 15u) | (((unsigned)(q)[(j) - 4] >> 6) << 4))
#define KQ_MIN(q, j)    ((j) < 4 ? (q)[(j) + 4] & 63u : ((unsigned)(q)[(j) + 4] >> 4) | (((unsigned)(q)[j] >> 6) << 4))

/* fp16 magnitude as mant * 2^(e - 25) */
static void Fp16_Split(unsigned h, UInt32 *mant, int *e)
{
  const unsigned ex = (h >> 10) & 31, m = h & 1023;
  *mant = ex == 0 ? m : m | 1024;
  *e = ex == 0 ? 1 : (int)ex;
}

/* floor(k * q0) of the zero point q0 = dmin * mn / (d * sc) of a sub-block (the quantized
   value of the weight 0), up to nb - 1; exact integer arithmetic on the fp16 d, dmin */
static unsigned ZeroBucket(UInt32 md, int ed, UInt32 mm, int em, unsigned sc, unsigned mn,
    unsigned k, unsigned nb)
{
  const UInt64 den = (UInt64)md * sc;
  const UInt64 num = (UInt64)mm * mn * k;
  const int sh = em - ed;   /* -30 .. 30 */
  UInt64 q;
  if (den == 0)
    return nb - 1;
  if (num == 0)
    return 0;
  if (sh >= 0)
    q = (num << sh) / den;   /* num < 2^18 */
  else
    q = (num / den) >> (unsigned)-sh;
  return q < nb ? (unsigned)q : nb - 1;
}

/* contexts of the symbols of field f of the structs p[0 .. ns) */
static void Q_Contexts(const Byte *p, unsigned S, const CQMField *f, size_t ns, Byte *ctx)
{
  size_t s;
  unsigned j;
  for (s = 0; s < ns; s++, p += S)
  {
    switch (f->kind)
    {
      case QM_BYTES:
        for (j = 0; j < f->len; j++)
          ctx[j] = (Byte)(f->numCtx > 1 ? j : 0);
        break;
      case QM_SCALE6:
        memset(ctx, 0, 8);
        break;
      case QM_MIN6:
        for (j = 0; j < 8; j++)
          ctx[j] = (Byte)(KQ_SCALE(p + 4, j) >> 2);
        break;
      case QM_Q4K:
      case QM_Q5K:
      {
        UInt32 md, mm;
        int ed, em;
        Fp16_Split(GetUi16(p), &md, &ed);
        Fp16_Split(GetUi16(p + 2), &mm, &em);
        for (j = 0; j < 8; j++)
        {
          const unsigned sc = KQ_SCALE(p + 4, j);
          const unsigned b = ZeroBucket(md, ed, mm, em, sc, KQ_MIN(p + 4, j),
              f->kind == QM_Q4K ? 2 : 1, 32);
          memset(ctx + 32 * j, (int)(b * 4 + (sc >> 4)), 32);
        }
        break;
      }
      default:   /* QM_Q6K: value v is in the sub-block v / 16 */
        for (j = 0; j < 16; j++)
        {
          const int sc = (int)(signed char)p[192 + j];
          const unsigned a = (unsigned)(sc < 0 ? -sc : sc) >> 3;
          memset(ctx + 16 * j, (int)(a > 15 ? 15 : a), 16);
        }
        break;
    }
    ctx += f->width;
  }
}

/* the symbols of field f of the structs p[0 .. ns). Values of Q5_K / Q6_K: layout in
   FloatPlane.h; value v of Q4_K: c = v / 64, h = (v / 32) % 2, l = v % 32: qs[32c + l]
   at bit 4h. */
static void Q_Symbols(const Byte *p, unsigned S, const CQMField *f, size_t ns, Byte *sym)
{
  size_t s;
  unsigned j, c, l;
  for (s = 0; s < ns; s++, p += S)
  {
    switch (f->kind)
    {
      case QM_BYTES:
        for (j = 0; j < f->len; j++)
          sym[j] = p[f->start + j * f->step];
        break;
      case QM_SCALE6:
        for (j = 0; j < 8; j++)
          sym[j] = (Byte)KQ_SCALE(p + 4, j);
        break;
      case QM_MIN6:
        for (j = 0; j < 8; j++)
          sym[j] = (Byte)KQ_MIN(p + 4, j);
        break;
      /* the values: from a local copy of the struct to a local array (nothing aliases:
         the compiler vectorizes the loops of 32 bytes) */
      case QM_Q4K:
      {
        Byte b[128], v[256];
        memcpy(b, p + 16, 128);
        for (c = 0; c < 4; c++)
        {
          const Byte *b0 = b + 32 * c;
          Byte *v0 = v + 64 * c;
          for (l = 0; l < 32; l++)
            v0[l] = (Byte)(b0[l] & 15);
          for (l = 0; l < 32; l++)
            v0[l + 32] = (Byte)(b0[l] >> 4);
        }
        memcpy(sym, v, 256);
        break;
      }
      case QM_Q5K:
      {
        Byte b[160], v[256];   /* qh[32], qs[128] */
        memcpy(b, p + 16, 160);
        for (c = 0; c < 4; c++)
        {
          const Byte *qs = b + 32 + 32 * c;
          Byte *v0 = v + 64 * c;
          const unsigned s0 = 2 * c, s1 = 2 * c + 1;
          for (l = 0; l < 32; l++)
            v0[l] = (Byte)((qs[l] & 15) | (((b[l] >> s0) & 1) << 4));
          for (l = 0; l < 32; l++)
            v0[l + 32] = (Byte)((qs[l] >> 4) | (((b[l] >> s1) & 1) << 4));
        }
        memcpy(sym, v, 256);
        break;
      }
      default:   /* QM_Q6K */
      {
        Byte b[192], v[256];   /* ql[128], qh[64] */
        memcpy(b, p, 192);
        for (c = 0; c < 2; c++)
        {
          const Byte *ql = b + 64 * c, *qh = b + 128 + 32 * c;
          Byte *v0 = v + 128 * c;
          for (l = 0; l < 32; l++)
            v0[l]      = (Byte)((ql[l] & 15)      | ((qh[l] & 3) << 4));
          for (l = 0; l < 32; l++)
            v0[l + 32] = (Byte)((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4));
          for (l = 0; l < 32; l++)
            v0[l + 64] = (Byte)((ql[l] >> 4)      | (((qh[l] >> 4) & 3) << 4));
          for (l = 0; l < 32; l++)
            v0[l + 96] = (Byte)((ql[l + 32] >> 4) | ((qh[l] >> 6) << 4));
        }
        memcpy(sym, v, 256);
        break;
      }
    }
    sym += f->width;
  }
}

/* the symbols back to the structs: the inverse of Q_Symbols(). QM_SCALE6 writes all 12
   bytes of scales[] with the mins at 0, QM_MIN6 adds the mins. */
static void Q_Join(Byte *p, unsigned S, const CQMField *f, size_t ns, const Byte *sym)
{
  size_t s;
  unsigned j, c, l;
  for (s = 0; s < ns; s++, p += S, sym += f->width)
  {
    Byte *q = p + 4;
    switch (f->kind)
    {
      case QM_BYTES:
        for (j = 0; j < f->len; j++)
          p[f->start + j * f->step] = sym[j];
        break;
      case QM_SCALE6:
        for (j = 0; j < 4; j++)
        {
          q[j] = (Byte)(sym[j] | ((sym[j + 4] >> 4) << 6));
          q[j + 4] = 0;
          q[j + 8] = (Byte)(sym[j + 4] & 15);
        }
        break;
      case QM_MIN6:
        for (j = 0; j < 4; j++)
        {
          q[j + 4] = (Byte)(sym[j] | ((sym[j + 4] >> 4) << 6));
          q[j + 8] = (Byte)(q[j + 8] | ((sym[j + 4] & 15) << 4));
        }
        break;
      /* the values: from a local copy to a local array, then copied (the compiler then
         knows that nothing aliases, and vectorizes the loops of 32 bytes) */
      case QM_Q4K:
      {
        Byte w[256], v[128];
        memcpy(w, sym, 256);
        for (c = 0; c < 4; c++)
        {
          const Byte *w0 = w + 64 * c;
          Byte *v0 = v + 32 * c;
          for (l = 0; l < 32; l++)
            v0[l] = (Byte)(w0[l] | (w0[l + 32] << 4));
        }
        memcpy(p + 16, v, 128);
        break;
      }
      case QM_Q5K:
      {
        Byte w[256], v[160];   /* qh[32], qs[128] */
        memcpy(w, sym, 256);
        for (l = 0; l < 32; l++)
          v[l] = (Byte)((w[l] >> 4) | ((w[l + 32] >> 4) << 1) | ((w[l + 64] >> 4) << 2)
              | ((w[l + 96] >> 4) << 3) | ((w[l + 128] >> 4) << 4) | ((w[l + 160] >> 4) << 5)
              | ((w[l + 192] >> 4) << 6) | ((w[l + 224] >> 4) << 7));
        for (c = 0; c < 4; c++)
        {
          const Byte *w0 = w + 64 * c;
          Byte *v0 = v + 32 + 32 * c;
          for (l = 0; l < 32; l++)
            v0[l] = (Byte)((w0[l] & 15) | ((w0[l + 32] & 15) << 4));
        }
        memcpy(p + 16, v, 160);
        break;
      }
      default:   /* QM_Q6K */
      {
        Byte w[256], v[192];   /* ql[128], qh[64] */
        memcpy(w, sym, 256);
        for (c = 0; c < 2; c++)
        {
          const Byte *w0 = w + 128 * c;
          Byte *ql = v + 64 * c, *qh = v + 128 + 32 * c;
          for (l = 0; l < 32; l++)
            ql[l] = (Byte)((w0[l] & 15) | ((w0[l + 64] & 15) << 4));
          for (l = 0; l < 32; l++)
            ql[l + 32] = (Byte)((w0[l + 32] & 15) | ((w0[l + 96] & 15) << 4));
          for (l = 0; l < 32; l++)
            qh[l] = (Byte)((w0[l] >> 4) | ((w0[l + 32] >> 4) << 2)
                | ((w0[l + 64] >> 4) << 4) | ((w0[l + 96] >> 4) << 6));
        }
        memcpy(p, v, 192);
        break;
      }
    }
  }
}


/* ---------- Frequency tables ---------- */

/* A table is sent as one level per symbol: 0 = absent, L >= 1: a relative frequency of
   about 2^((L - 1) / 8). Both sides scale the levels to frequencies that sum to 2^P
   (Norm_Freqs): the rounding to 1/8 octave costs about 0.01% of the coded size. */
static const UInt16 kLevelMant[kLevelSteps] = { 256, 279, 304, 332, 362, 395, 431, 470 };
/* 256 * 2^((t - 0.5) / 8), t = 1 .. 8: the bounds between the levels */
static const UInt16 kLevelBound[kLevelSteps] = { 267, 292, 318, 347, 378, 412, 450, 490 };

/* level of a count c >= 1 (c < 2^23) */
static unsigned Count_Level(UInt32 c)
{
  unsigned q = 0, r = 0, t;
  while ((c >> q) > 1)
    q++;
  for (t = 0; t < kLevelSteps; t++)
    if (((UInt64)c << 8) >= ((UInt64)kLevelBound[t] << q))
      r = t + 1;
  q = 1 + q * kLevelSteps + r;
  return q < kMaxLevel ? q : kMaxLevel;
}

/* frequencies f[0 .. numSyms) (sum 2^P, each present symbol >= 1) from the levels of a
   context; returns 0 if no symbol is present */
static int Norm_Freqs(const Byte *lev, unsigned numSyms, unsigned P, UInt32 *f)
{
  const UInt32 M = (UInt32)1 << P;
  UInt64 sumA = 0;
  UInt32 sum = 0;
  unsigned s;
  for (s = 0; s < numSyms; s++)
  {
    const unsigned L = lev[s];
    f[s] = 0;
    if (L != 0)
    {
      f[s] = (UInt32)kLevelMant[(L - 1) % kLevelSteps] << ((L - 1) / kLevelSteps);   /* < 2^32 */
      sumA += f[s];
    }
  }
  if (sumA == 0)
    return 0;
  for (s = 0; s < numSyms; s++)
    if (f[s] != 0)
    {
      f[s] = (UInt32)(((UInt64)f[s] << P) / sumA);
      if (f[s] == 0)
        f[s] = 1;
      sum += f[s];
    }
  /* the rounding error goes to the most frequent symbols (numSyms <= M: always possible) */
  while (sum != M)
  {
    unsigned best = 0;
    for (s = 1; s < numSyms; s++)
      if (f[s] > f[best])
        best = s;
    if (sum < M)
    {
      f[best] += M - sum;
      sum = M;
    }
    else
    {
      const UInt32 take = sum - M < f[best] - 1 ? sum - M : f[best] - 1;
      f[best] -= take;
      sum -= take;
    }
  }
  return 1;
}

/* log2(v) in 16.16 fixed point, v >= 1 (v < 2^31) */
static UInt32 Log2_Fix(UInt32 v)
{
  UInt32 r = 0;
  unsigned i;
  UInt64 x;
  while ((v >> r) > 1)
    r++;
  x = ((UInt64)v << 30) >> r;   /* v / 2^r in [1, 2) as 2.30 */
  r <<= 16;
  for (i = 16; i-- != 0;)
  {
    x = (x * x) >> 30;
    if (x >= ((UInt64)2 << 30))
    {
      x >>= 1;
      r |= (UInt32)1 << i;
    }
  }
  return r;
}

/* bit I/O of the tables: LSB first */
typedef struct
{
  Byte *p;        /* NULL: only counts the bits */
  UInt64 acc;
  unsigned cnt;
  size_t bits;
} CBitW;

static void BitW_Put(CBitW *w, UInt32 v, unsigned nb)
{
  w->bits += nb;
  if (!w->p)
    return;
  w->acc |= (UInt64)v << w->cnt;
  w->cnt += nb;
  while (w->cnt >= 8)
  {
    *w->p++ = (Byte)w->acc;
    w->acc >>= 8;
    w->cnt -= 8;
  }
}

static void BitW_Flush(CBitW *w)
{
  if (w->p && w->cnt != 0)
    *w->p++ = (Byte)w->acc;
  w->acc = 0;
  w->cnt = 0;
}

/* v + 1 as Elias gamma: n - 1 zero bits, a 1, the n - 1 low bits */
static void BitW_Gamma(CBitW *w, UInt32 v)
{
  const UInt32 x = v + 1;
  unsigned n = 1;
  while ((x >> n) != 0)
    n++;
  BitW_Put(w, 0, n - 1);
  BitW_Put(w, 1, 1);
  BitW_Put(w, x & (((UInt32)1 << (n - 1)) - 1), n - 1);
}

typedef struct
{
  const Byte *p, *lim;
  UInt32 acc;
  unsigned cnt;
  int error;
} CBitR;

static UInt32 BitR_Get(CBitR *r, unsigned nb)
{
  UInt32 v;
  while (r->cnt < nb)
  {
    if (r->p == r->lim)
    {
      r->error = 1;
      return 0;
    }
    r->acc |= (UInt32)*r->p++ << r->cnt;
    r->cnt += 8;
  }
  v = r->acc & (((UInt32)1 << nb) - 1);
  r->acc >>= nb;
  r->cnt -= nb;
  return v;
}

static UInt32 BitR_Gamma(CBitR *r)
{
  unsigned n = 1;
  while (BitR_Get(r, 1) == 0)
  {
    if (r->error || ++n > kMaxGamma)
    {
      r->error = 1;
      return 0;
    }
  }
  return (((UInt32)1 << (n - 1)) | BitR_Get(r, n - 1)) - 1;
}

/* tables of a field: per context a bit (used), then the level deltas of its symbols,
   zigzag, as Elias gamma */
static void Tables_Write(CBitW *w, const Byte *levels, unsigned numCtx, unsigned numSyms)
{
  unsigned c, s;
  for (c = 0; c < numCtx; c++, levels += numSyms)
  {
    unsigned prev = 0, used = 0;
    for (s = 0; s < numSyms; s++)
      used |= levels[s];
    BitW_Put(w, used != 0, 1);
    if (used)
      for (s = 0; s < numSyms; s++)
      {
        const int d = (int)levels[s] - (int)prev;
        BitW_Gamma(w, d >= 0 ? (UInt32)d * 2 : (UInt32)(-d) * 2 - 1);
        prev = levels[s];
      }
  }
}

static int Tables_Read(CBitR *r, Byte *levels, unsigned numCtx, unsigned numSyms)
{
  unsigned c, s;
  for (c = 0; c < numCtx; c++, levels += numSyms)
  {
    int prev = 0;
    if (BitR_Get(r, 1) == 0)
    {
      memset(levels, 0, numSyms);
      continue;
    }
    for (s = 0; s < numSyms; s++)
    {
      const UInt32 z = BitR_Gamma(r);
      const int d = (z & 1) ? -(int)((z + 1) >> 1) : (int)(z >> 1);
      prev += d;
      if (r->error || prev < 0 || prev > kMaxLevel)
        return 0;
      levels[s] = (Byte)prev;
    }
  }
  return !r->error;
}


/* ---------- Encoder ---------- */

int FloatQuant_Plan(const Byte *src, size_t srcLen, unsigned qtype, unsigned phase, CFloatQuantPlan *plan)
{
  const CQMType *t;
  unsigned fi;
  size_t n;
  if (qtype < kFirstQType || qtype - kFirstQType >= kNumQMTypes)
    return 0;
  t = &g_QMTypes[qtype - kFirstQType];
  if (srcLen < phase + t->size)
    return 0;
  n = (srcLen - phase) / t->size;
  plan->qtype = qtype;
  plan->phase = phase;
  plan->n = n;
  plan->tail = srcLen - phase - n * t->size;
  plan->total = kHeaderSize + phase + plan->tail + 8 * t->numFields;
  for (fi = 0; fi < t->numFields; fi++)
  {
    const CQMField *f = &t->f[fi];
    const unsigned numSyms = 1u << f->logSyms, P = f->prec;
    const unsigned numTab = f->numCtx * numSyms;
    UInt32 counts[FLOAT_QUANT_MAX_TAB];
    UInt32 freqs[256];
    Byte sym[kGather * kMaxWidth], ctx[kGather * kMaxWidth];
    Byte *levels = plan->levels[fi];
    UInt64 cost = 0;   /* bits, 16.16 */
    CBitW w;
    size_t s;
    unsigned c, k;
    memset(counts, 0, numTab * sizeof(counts[0]));
    for (s = 0; s < n; s += kGather)
    {
      const size_t ns = n - s < kGather ? n - s : kGather;
      const Byte *p = src + phase + s * t->size;
      size_t i;
      Q_Symbols(p, t->size, f, ns, sym);
      Q_Contexts(p, t->size, f, ns, ctx);
      for (i = 0; i < ns * f->width; i++)
        counts[((unsigned)ctx[i] << f->logSyms) + sym[i]]++;
    }
    for (k = 0; k < numTab; k++)
      levels[k] = (Byte)(counts[k] != 0 ? Count_Level(counts[k]) : 0);
    for (c = 0; c < f->numCtx; c++)
      if (Norm_Freqs(levels + c * numSyms, numSyms, P, freqs))
        for (k = 0; k < numSyms; k++)
          if (counts[c * numSyms + k] != 0)
            cost += (UInt64)counts[c * numSyms + k] * (((UInt32)P << 16) - Log2_Fix(freqs[k]));
    w.p = NULL;
    w.bits = 0;
    Tables_Write(&w, levels, f->numCtx, numSyms);
    plan->tabBytes[fi] = (w.bits + 7) >> 3;
    /* stream: the states, the 16-bit words (about cost / 16) */
    plan->total += plan->tabBytes[fi] + 4 * QM_STATES(f) + (size_t)((cost >> 20) + 1) * 2;
  }
  return 1;
}

typedef struct
{
  UInt32 rcp;
  UInt32 xMax;     /* freq << (31 - P): the state is renormalized from here */
  UInt16 bias, cmpl;
  UInt32 shift;    /* 32 + the shift of the reciprocal */
} CEncSym;

/* rans_byte.h: x / freq as (x * rcp) >> (32 + shift), exact for x < 2^31 */
static void EncSym_Init(CEncSym *e, UInt32 cum, UInt32 freq, unsigned P)
{
  const UInt32 M = (UInt32)1 << P;
  e->xMax = freq << (31 - P);
  e->cmpl = (UInt16)(M - freq);
  if (freq < 2)
  {
    e->rcp = 0xFFFFFFFF;
    e->shift = 32;
    e->bias = (UInt16)(cum + M - 1);
  }
  else
  {
    unsigned sh = 0;
    while (freq > ((UInt32)1 << sh))
      sh++;
    e->rcp = (UInt32)((((UInt64)1 << (sh + 31)) + freq - 1) / freq);
    e->shift = 32 + sh - 1;
    e->bias = (UInt16)cum;
  }
}

/* Branchless: the low 16 bits are always stored below o, and o moves down only if the
   state is renormalized (there must be room for the word). */
#define RANS_ENC(x, e) { \
  const CEncSym *e_ = (e); \
  const UInt32 m_ = (UInt32)0 - (UInt32)((x) >= e_->xMax); \
  SetUi16(o - 2, (UInt16)(x)) \
  o -= m_ & 2; \
  (x) >>= (m_ & 16); \
  (x) += e_->bias + (UInt32)(((UInt64)(x) * e_->rcp) >> e_->shift) * e_->cmpl; }

/* the symbols i - 1 .. 0 of a chunk (i is a multiple of 8), 8 at a time: symbol i with
   state x[i % N]; NULL if the output reaches oLim. GROUP: the 8 symbols of a group have
   the same context (the values), else each its own. */
#define ENC_SYM(j, LOG, GROUP)  (GROUP ? &eg[sym[i + j]] : &es[((unsigned)ctx[i + j] << LOG) + sym[i + j]])

#define DEFINE_ENC_GROUPS(name, LOG, GROUP) \
static Byte *name(UInt32 *x, unsigned N, const CEncSym *es, const Byte *sym, const Byte *ctx, \
    size_t i, Byte *o, const Byte *oLim) \
{ \
  while (i != 0) \
  { \
    UInt32 *xs; \
    const CEncSym *eg; \
    UInt32 x0, x1, x2, x3, x4, x5, x6, x7; \
    if ((size_t)(o - oLim) < 2 * 8) \
      return NULL; \
    i -= 8; \
    xs = x + (i & (N - 1)); \
    eg = es + ((size_t)ctx[i] << LOG); \
    x0 = xs[0]; x1 = xs[1]; x2 = xs[2]; x3 = xs[3]; \
    x4 = xs[4]; x5 = xs[5]; x6 = xs[6]; x7 = xs[7]; \
    RANS_ENC(x7, ENC_SYM(7, LOG, GROUP)) \
    RANS_ENC(x6, ENC_SYM(6, LOG, GROUP)) \
    RANS_ENC(x5, ENC_SYM(5, LOG, GROUP)) \
    RANS_ENC(x4, ENC_SYM(4, LOG, GROUP)) \
    RANS_ENC(x3, ENC_SYM(3, LOG, GROUP)) \
    RANS_ENC(x2, ENC_SYM(2, LOG, GROUP)) \
    RANS_ENC(x1, ENC_SYM(1, LOG, GROUP)) \
    RANS_ENC(x0, ENC_SYM(0, LOG, GROUP)) \
    xs[0] = x0; xs[1] = x1; xs[2] = x2; xs[3] = x3; \
    xs[4] = x4; xs[5] = x5; xs[6] = x6; xs[7] = x7; \
  } \
  return o; \
}

DEFINE_ENC_GROUPS(Enc_Groups4, 4, 1)   /* Q4_K values */
DEFINE_ENC_GROUPS(Enc_Groups5, 5, 1)   /* Q5_K values */
DEFINE_ENC_GROUPS(Enc_Groups6G, 6, 1)  /* Q6_K values */
DEFINE_ENC_GROUPS(Enc_Groups6, 6, 0)   /* scales, mins */
DEFINE_ENC_GROUPS(Enc_Groups8, 8, 0)   /* bytes */

typedef Byte *(*Func_EncGroups)(UInt32 *x, unsigned N, const CEncSym *es, const Byte *sym,
    const Byte *ctx, size_t i, Byte *o, const Byte *oLim);

/* Codes one field: the stream is written backwards, down from oEnd (to oLim at most);
   returns its start, or NULL if it does not fit. */
static Byte *Encode_Field(const Byte *src, const CQMType *t, const CQMField *f, size_t n,
    const Byte *levels, Byte *oLim, Byte *oEnd)
{
  const unsigned numSyms = 1u << f->logSyms, P = f->prec;
  const Func_EncGroups groups = f->kind == QM_Q4K ? Enc_Groups4 : f->kind == QM_Q5K ? Enc_Groups5
      : f->kind == QM_Q6K ? Enc_Groups6G : f->logSyms == 6 ? Enc_Groups6 : Enc_Groups8;
  CEncSym es[FLOAT_QUANT_MAX_TAB];
  Byte sym[kGather * kMaxWidth], ctx[kGather * kMaxWidth];
  const unsigned N = QM_STATES(f);
  UInt32 x[kMaxStates];
  Byte *o = oEnd;
  size_t s0;
  unsigned c, k;

  for (c = 0; c < f->numCtx; c++)
  {
    UInt32 freqs[256], cum = 0;
    if (!Norm_Freqs(levels + c * numSyms, numSyms, P, freqs))
      continue;
    for (k = 0; k < numSyms; k++)
    {
      if (freqs[k] != 0)
        EncSym_Init(&es[c * numSyms + k], cum, freqs[k], P);
      cum += freqs[k];
    }
  }
  for (k = 0; k < N; k++)
    x[k] = kRansL;

  /* backwards: chunks from the last, symbols from the last of each chunk; symbol i (of
     the field) with state i % N, and a chunk starts at a multiple of N */
  for (s0 = ((n - 1) / kGather) * kGather;; s0 -= kGather)
  {
    const size_t ns = n - s0 < kGather ? n - s0 : kGather;
    const Byte *p = src + s0 * t->size;
    size_t i = ns * f->width;
    Q_Symbols(p, t->size, f, ns, sym);
    Q_Contexts(p, t->size, f, ns, ctx);
    if ((size_t)(o - oLim) < 2 * 8)
      return NULL;
    while (i & 7)
    {
      i--;
      RANS_ENC(x[i & (N - 1)], &es[((unsigned)ctx[i] << f->logSyms) + sym[i]])
    }
    o = groups(x, N, es, sym, ctx, i, o, oLim);
    if (!o)
      return NULL;
    if (s0 == 0)
      break;
  }
  if ((size_t)(o - oLim) < 4 * N)
    return NULL;
  o -= 4 * N;
  for (k = 0; k < N; k++)
    SetUi32(o + 4 * k, x[k])
  return o;
}

size_t FloatQuant_Write(const Byte *src, size_t srcLen, Byte *dest, size_t destCap, const CFloatQuantPlan *plan)
{
  const CQMType *t = &g_QMTypes[plan->qtype - kFirstQType];
  const size_t phase = plan->phase, tail = plan->tail;
  Byte * const destEnd = dest + destCap;
  Byte *d = dest, *sizes;
  unsigned fi;

  if (destCap < kHeaderSize + phase + tail + 8 * t->numFields)
    return 0;
  *d++ = FLOAT_QUANT_MODE;
  SetUi32(d, (UInt32)srcLen)  d += 4;
  *d++ = (Byte)plan->qtype;
  *d++ = (Byte)phase;
  memcpy(d, src, phase);                    d += phase;
  memcpy(d, src + srcLen - tail, tail);     d += tail;
  sizes = d;
  d += 8 * t->numFields;

  for (fi = 0; fi < t->numFields; fi++)
  {
    const CQMField *f = &t->f[fi];
    const size_t tabBytes = plan->tabBytes[fi];
    CBitW w;
    Byte *start;
    size_t streamSize;
    if ((size_t)(destEnd - d) < tabBytes)
      return 0;
    w.p = d;
    w.acc = 0;
    w.cnt = 0;
    w.bits = 0;
    Tables_Write(&w, plan->levels[fi], f->numCtx, 1u << f->logSyms);
    BitW_Flush(&w);
    d += tabBytes;
    start = Encode_Field(src + phase, t, f, plan->n, plan->levels[fi], d, destEnd);
    if (!start)
      return 0;
    streamSize = (size_t)(destEnd - start);
    memmove(d, start, streamSize);
    d += streamSize;
    SetUi32(sizes + 8 * fi, (UInt32)tabBytes)
    SetUi32(sizes + 8 * fi + 4, (UInt32)streamSize)
  }
  return (size_t)(d - dest);
}


/* ---------- Decoder ---------- */

typedef struct
{
  const CQMType *t;
  unsigned phase;
  size_t n, tail;
  size_t headerSize;   /* up to the end of the field sizes */
} CQMHeader;

static SRes Parse_Header(const Byte *src, size_t srcLen, size_t outLen, CQMHeader *h)
{
  unsigned qtype;
  if (srcLen < kHeaderSize)
    return SZ_ERROR_INPUT_EOF;
  qtype = src[5];
  h->phase = src[6];
  if (qtype < kFirstQType || qtype - kFirstQType >= kNumQMTypes)
    return SZ_ERROR_DATA;
  h->t = &g_QMTypes[qtype - kFirstQType];
  if (h->phase >= h->t->size || outLen < h->phase + h->t->size)   /* at least one struct */
    return SZ_ERROR_DATA;
  h->n = (outLen - h->phase) / h->t->size;
  h->tail = outLen - h->phase - h->n * h->t->size;
  h->headerSize = kHeaderSize + h->phase + h->tail + 8 * h->t->numFields;
  return SZ_OK;
}

/* the sizes of the fields: tables, stream (the states and an even number of bytes) */
static SRes Get_FieldSizes(const Byte *src, const CQMHeader *h, size_t outLen,
    size_t *tabSizes, size_t *streamSizes, size_t *total)
{
  const Byte *p = src + h->headerSize - 8 * h->t->numFields;
  unsigned fi;
  *total = 0;
  for (fi = 0; fi < h->t->numFields; fi++)
  {
    const UInt32 tabSize = GetUi32(p + 8 * fi), streamSize = GetUi32(p + 8 * fi + 4);
    const UInt32 statesBytes = 4 * QM_STATES(&h->t->f[fi]);
    if (tabSize == 0 || tabSize > outLen || streamSize < statesBytes
        || streamSize > outLen + statesBytes || (streamSize & 1) != 0)
      return SZ_ERROR_DATA;
    tabSizes[fi] = tabSize;
    streamSizes[fi] = streamSize;
    *total += (size_t)tabSize + streamSize;
  }
  return SZ_OK;
}

SRes FloatQuant_GetBlockSize(const Byte *src, size_t srcLen, size_t outLen, size_t *blockSize)
{
  CQMHeader h;
  size_t tabSizes[FLOAT_QUANT_MAX_FIELDS], streamSizes[FLOAT_QUANT_MAX_FIELDS], total;
  RINOK(Parse_Header(src, srcLen, outLen, &h))
  if (srcLen < h.headerSize)
    return SZ_ERROR_INPUT_EOF;
  RINOK(Get_FieldSizes(src, &h, outLen, tabSizes, streamSizes, &total))
  *blockSize = h.headerSize + total;
  return SZ_OK;
}

typedef struct
{
  const Byte *symTab;   /* [ctx << P | slot]: symbol */
  const UInt32 *fc;     /* [ctx << logSyms | sym]: freq | (cum << 16) */
  unsigned P, logSyms, N;
  const Byte *p, *end;  /* 16-bit words */
  UInt32 x[kMaxStates];
} CRansDec;

#define RANS_DEC_SYM(xx, tb, fb, s, P) { \
  const UInt32 slot_ = (xx) & (((UInt32)1 << (P)) - 1); \
  UInt32 e_; \
  s = (tb)[slot_]; \
  e_ = (fb)[s]; \
  xx = (e_ & 0xFFFF) * ((xx) >> (P)) + slot_ - (e_ >> 16); }

/* branchless: the word is used only if the state is below kRansL */
#define RANS_RENORM_FAST(xx) { \
  const UInt32 n_ = ((xx) << 16) | GetUi16(p); \
  const UInt32 m_ = (UInt32)0 - (UInt32)((xx) < kRansL); \
  xx = (n_ & m_) | ((xx) & ~m_); \
  p += m_ & 2; }

#define DEC_ONE(xx, j, P, LOG) { \
  const Byte *tb_ = symTab + ((size_t)ctx[i + j] << (P)); \
  const UInt32 *fb_ = fc + ((size_t)ctx[i + j] << (LOG)); \
  RANS_DEC_SYM(xx, tb_, fb_, s, P)  out[i + j] = (Byte)s;  RANS_RENORM_FAST(xx) }

/* The symbols of a chunk, 8 at a time while the stream has 8 words: returns the
   symbols done. GROUP: the 8 symbols of a group have the same context (the values of a
   sub-block of 16 or 32), else each its own. */
#define DEFINE_DEC_GROUPS(name, P, LOG, GROUP) \
static size_t name(CRansDec *d, const Byte *ctx, Byte *out, size_t num) \
{ \
  const Byte * const symTab = d->symTab; \
  const UInt32 * const fc = d->fc; \
  const Byte *p = d->p; \
  const Byte * const end = d->end; \
  const unsigned N = d->N; \
  size_t i = 0; \
  for (; i + 8 <= num && (size_t)(end - p) >= 2 * 8; i += 8) \
  { \
    UInt32 * const xs = d->x + (i & (N - 1)); \
    UInt32 x0 = xs[0], x1 = xs[1], x2 = xs[2], x3 = xs[3]; \
    UInt32 x4 = xs[4], x5 = xs[5], x6 = xs[6], x7 = xs[7]; \
    unsigned s; \
    if (GROUP) \
    { \
      const Byte *tb = symTab + ((size_t)ctx[i] << (P)); \
      const UInt32 *fb = fc + ((size_t)ctx[i] << (LOG)); \
      RANS_DEC_SYM(x0, tb, fb, s, P)  out[i]     = (Byte)s;  RANS_RENORM_FAST(x0) \
      RANS_DEC_SYM(x1, tb, fb, s, P)  out[i + 1] = (Byte)s;  RANS_RENORM_FAST(x1) \
      RANS_DEC_SYM(x2, tb, fb, s, P)  out[i + 2] = (Byte)s;  RANS_RENORM_FAST(x2) \
      RANS_DEC_SYM(x3, tb, fb, s, P)  out[i + 3] = (Byte)s;  RANS_RENORM_FAST(x3) \
      RANS_DEC_SYM(x4, tb, fb, s, P)  out[i + 4] = (Byte)s;  RANS_RENORM_FAST(x4) \
      RANS_DEC_SYM(x5, tb, fb, s, P)  out[i + 5] = (Byte)s;  RANS_RENORM_FAST(x5) \
      RANS_DEC_SYM(x6, tb, fb, s, P)  out[i + 6] = (Byte)s;  RANS_RENORM_FAST(x6) \
      RANS_DEC_SYM(x7, tb, fb, s, P)  out[i + 7] = (Byte)s;  RANS_RENORM_FAST(x7) \
    } \
    else \
    { \
      DEC_ONE(x0, 0, P, LOG) DEC_ONE(x1, 1, P, LOG) DEC_ONE(x2, 2, P, LOG) DEC_ONE(x3, 3, P, LOG) \
      DEC_ONE(x4, 4, P, LOG) DEC_ONE(x5, 5, P, LOG) DEC_ONE(x6, 6, P, LOG) DEC_ONE(x7, 7, P, LOG) \
    } \
    xs[0] = x0; xs[1] = x1; xs[2] = x2; xs[3] = x3; \
    xs[4] = x4; xs[5] = x5; xs[6] = x6; xs[7] = x7; \
  } \
  d->p = p; \
  return i; \
}

DEFINE_DEC_GROUPS(Dec_Groups_9_4, 9, 4, 1)    /* Q4_K values */
DEFINE_DEC_GROUPS(Dec_Groups_9_5, 9, 5, 1)    /* Q5_K values */
DEFINE_DEC_GROUPS(Dec_Groups_11_6, 11, 6, 1)  /* Q6_K values */
DEFINE_DEC_GROUPS(Dec_Groups_10_6, 10, 6, 0)  /* scales, mins */
DEFINE_DEC_GROUPS(Dec_Groups_12_8, 12, 8, 0)  /* bytes */

typedef size_t (*Func_DecGroups)(CRansDec *d, const Byte *ctx, Byte *out, size_t num);


/* ---------- AVX2 decoder of the values ---------- */

#ifdef MY_CPU_X86_OR_AMD64
  #if defined(__clang__) && (__clang_major__ >= 4) \
      || defined(Z7_GCC_VERSION) && (Z7_GCC_VERSION >= 40900)
    #define FLOAT_QUANT_USE_AVX2
    #define FLOAT_QUANT_ATTRIB_AVX2  __attribute__((__target__("avx2")))
  #elif defined(_MSC_VER) && (_MSC_VER >= 1900)
    #define FLOAT_QUANT_USE_AVX2
    #define FLOAT_QUANT_ATTRIB_AVX2
  #endif
#endif

#ifdef FLOAT_QUANT_USE_AVX2

#include <immintrin.h>
#if defined(__clang__)
#include <avxintrin.h>
#include <avx2intrin.h>
#endif

/* byte j: the rank of lane j among the set bits of the index (the word of lane j) */
static const UInt64 g_LaneRank[256] =
{
  0x0000000000000000, 0x0000000000000000, 0x0000000000000000, 0x0000000000000100,
  0x0000000000000000, 0x0000000000010000, 0x0000000000010000, 0x0000000000020100,
  0x0000000000000000, 0x0000000001000000, 0x0000000001000000, 0x0000000002000100,
  0x0000000001000000, 0x0000000002010000, 0x0000000002010000, 0x0000000003020100,
  0x0000000000000000, 0x0000000100000000, 0x0000000100000000, 0x0000000200000100,
  0x0000000100000000, 0x0000000200010000, 0x0000000200010000, 0x0000000300020100,
  0x0000000100000000, 0x0000000201000000, 0x0000000201000000, 0x0000000302000100,
  0x0000000201000000, 0x0000000302010000, 0x0000000302010000, 0x0000000403020100,
  0x0000000000000000, 0x0000010000000000, 0x0000010000000000, 0x0000020000000100,
  0x0000010000000000, 0x0000020000010000, 0x0000020000010000, 0x0000030000020100,
  0x0000010000000000, 0x0000020001000000, 0x0000020001000000, 0x0000030002000100,
  0x0000020001000000, 0x0000030002010000, 0x0000030002010000, 0x0000040003020100,
  0x0000010000000000, 0x0000020100000000, 0x0000020100000000, 0x0000030200000100,
  0x0000020100000000, 0x0000030200010000, 0x0000030200010000, 0x0000040300020100,
  0x0000020100000000, 0x0000030201000000, 0x0000030201000000, 0x0000040302000100,
  0x0000030201000000, 0x0000040302010000, 0x0000040302010000, 0x0000050403020100,
  0x0000000000000000, 0x0001000000000000, 0x0001000000000000, 0x0002000000000100,
  0x0001000000000000, 0x0002000000010000, 0x0002000000010000, 0x0003000000020100,
  0x0001000000000000, 0x0002000001000000, 0x0002000001000000, 0x0003000002000100,
  0x0002000001000000, 0x0003000002010000, 0x0003000002010000, 0x0004000003020100,
  0x0001000000000000, 0x0002000100000000, 0x0002000100000000, 0x0003000200000100,
  0x0002000100000000, 0x0003000200010000, 0x0003000200010000, 0x0004000300020100,
  0x0002000100000000, 0x0003000201000000, 0x0003000201000000, 0x0004000302000100,
  0x0003000201000000, 0x0004000302010000, 0x0004000302010000, 0x0005000403020100,
  0x0001000000000000, 0x0002010000000000, 0x0002010000000000, 0x0003020000000100,
  0x0002010000000000, 0x0003020000010000, 0x0003020000010000, 0x0004030000020100,
  0x0002010000000000, 0x0003020001000000, 0x0003020001000000, 0x0004030002000100,
  0x0003020001000000, 0x0004030002010000, 0x0004030002010000, 0x0005040003020100,
  0x0002010000000000, 0x0003020100000000, 0x0003020100000000, 0x0004030200000100,
  0x0003020100000000, 0x0004030200010000, 0x0004030200010000, 0x0005040300020100,
  0x0003020100000000, 0x0004030201000000, 0x0004030201000000, 0x0005040302000100,
  0x0004030201000000, 0x0005040302010000, 0x0005040302010000, 0x0006050403020100,
  0x0000000000000000, 0x0100000000000000, 0x0100000000000000, 0x0200000000000100,
  0x0100000000000000, 0x0200000000010000, 0x0200000000010000, 0x0300000000020100,
  0x0100000000000000, 0x0200000001000000, 0x0200000001000000, 0x0300000002000100,
  0x0200000001000000, 0x0300000002010000, 0x0300000002010000, 0x0400000003020100,
  0x0100000000000000, 0x0200000100000000, 0x0200000100000000, 0x0300000200000100,
  0x0200000100000000, 0x0300000200010000, 0x0300000200010000, 0x0400000300020100,
  0x0200000100000000, 0x0300000201000000, 0x0300000201000000, 0x0400000302000100,
  0x0300000201000000, 0x0400000302010000, 0x0400000302010000, 0x0500000403020100,
  0x0100000000000000, 0x0200010000000000, 0x0200010000000000, 0x0300020000000100,
  0x0200010000000000, 0x0300020000010000, 0x0300020000010000, 0x0400030000020100,
  0x0200010000000000, 0x0300020001000000, 0x0300020001000000, 0x0400030002000100,
  0x0300020001000000, 0x0400030002010000, 0x0400030002010000, 0x0500040003020100,
  0x0200010000000000, 0x0300020100000000, 0x0300020100000000, 0x0400030200000100,
  0x0300020100000000, 0x0400030200010000, 0x0400030200010000, 0x0500040300020100,
  0x0300020100000000, 0x0400030201000000, 0x0400030201000000, 0x0500040302000100,
  0x0400030201000000, 0x0500040302010000, 0x0500040302010000, 0x0600050403020100,
  0x0100000000000000, 0x0201000000000000, 0x0201000000000000, 0x0302000000000100,
  0x0201000000000000, 0x0302000000010000, 0x0302000000010000, 0x0403000000020100,
  0x0201000000000000, 0x0302000001000000, 0x0302000001000000, 0x0403000002000100,
  0x0302000001000000, 0x0403000002010000, 0x0403000002010000, 0x0504000003020100,
  0x0201000000000000, 0x0302000100000000, 0x0302000100000000, 0x0403000200000100,
  0x0302000100000000, 0x0403000200010000, 0x0403000200010000, 0x0504000300020100,
  0x0302000100000000, 0x0403000201000000, 0x0403000201000000, 0x0504000302000100,
  0x0403000201000000, 0x0504000302010000, 0x0504000302010000, 0x0605000403020100,
  0x0201000000000000, 0x0302010000000000, 0x0302010000000000, 0x0403020000000100,
  0x0302010000000000, 0x0403020000010000, 0x0403020000010000, 0x0504030000020100,
  0x0302010000000000, 0x0403020001000000, 0x0403020001000000, 0x0504030002000100,
  0x0403020001000000, 0x0504030002010000, 0x0504030002010000, 0x0605040003020100,
  0x0302010000000000, 0x0403020100000000, 0x0403020100000000, 0x0504030200000100,
  0x0403020100000000, 0x0504030200010000, 0x0504030200010000, 0x0605040300020100,
  0x0403020100000000, 0x0504030201000000, 0x0504030201000000, 0x0605040302000100,
  0x0504030201000000, 0x0605040302010000, 0x0605040302010000, 0x0706050403020100,
};

/* number of set bits */
static const Byte g_LaneCount[256] =
{
#define B2(n)  n, n + 1, n + 1, n + 2
#define B4(n)  B2(n), B2(n + 1), B2(n + 1), B2(n + 2)
#define B6(n)  B4(n), B4(n + 1), B4(n + 1), B4(n + 2)
  B6(0), B6(1), B6(1), B6(2)
#undef B2
#undef B4
#undef B6
};

/* GCC drops the zero source of a gather with a constant full mask, and then gathers
   into a register of the previous round: a dependency between the rounds. The mask is
   made opaque to keep the zero source. */
#if defined(__GNUC__) && !defined(__clang__)
#define AVX2_OPAQUE(v)  __asm__("" : "+x"(v));
#else
#define AVX2_OPAQUE(v)
#endif

/* One vector of 8 states (one symbol each, with the same context c): the symbol from
   the byte table (a 4-byte gather at each slot), freq and cum from the second table
   (the gathers into a zero vector: a gather depends on its destination);
   then the lanes below kRansL take the next words of the stream, in lane order. */
#define AVX2_STEP(x, sv, c, P, LOG) { \
  const int *tb_ = (const int *)(const void *)(symTab + ((size_t)(c) << (P))); \
  const int *fb_ = (const int *)(const void *)(fc + ((size_t)(c) << (LOG))); \
  const __m256i slot_ = _mm256_and_si256(x, vMask); \
  __m256i e_, need_, w_; \
  unsigned m_; \
  __m256i m1_ = vAll, m2_ = vAll; \
  AVX2_OPAQUE(m1_) \
  AVX2_OPAQUE(m2_) \
  sv = _mm256_and_si256(_mm256_mask_i32gather_epi32(_mm256_setzero_si256(), tb_, slot_, m1_, 1), vFF); \
  e_ = _mm256_mask_i32gather_epi32(_mm256_setzero_si256(), fb_, sv, m2_, 4); \
  x = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(e_, vFFFF), _mm256_srli_epi32(x, P)), \
      _mm256_sub_epi32(slot_, _mm256_srli_epi32(e_, 16))); \
  need_ = _mm256_cmpgt_epi32(vL, x); \
  m_ = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(need_)); \
  w_ = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(const void *)p)); \
  w_ = _mm256_permutevar8x32_epi32(w_, _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(const void *)&g_LaneRank[m_]))); \
  x = _mm256_blendv_epi8(x, _mm256_or_si256(_mm256_slli_epi32(x, 16), w_), need_); \
  p += 2 * (size_t)g_LaneCount[m_]; }

/* 64 symbols (8 vectors) at a time, while the stream has 128 bytes; num is a multiple
   of 64 and the states are 64 (the values) */
#define DEFINE_DEC_AVX2(name, P, LOG) \
static FLOAT_QUANT_ATTRIB_AVX2 size_t name(CRansDec *d, const Byte *ctx, Byte *out, size_t num) \
{ \
  const Byte * const symTab = d->symTab; \
  const UInt32 * const fc = d->fc; \
  const Byte *p = d->p; \
  const Byte * const end = d->end; \
  const __m256i vMask = _mm256_set1_epi32((1 << (P)) - 1); \
  const __m256i vFF = _mm256_set1_epi32(0xFF), vFFFF = _mm256_set1_epi32(0xFFFF); \
  const __m256i vL = _mm256_set1_epi32((int)kRansL); \
  const __m256i vAll = _mm256_set1_epi32(-1); \
  const __m256i vOrder = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7); \
  __m256i x0 = _mm256_loadu_si256((const __m256i *)(const void *)d->x); \
  __m256i x1 = _mm256_loadu_si256((const __m256i *)(const void *)(d->x + 8)); \
  __m256i x2 = _mm256_loadu_si256((const __m256i *)(const void *)(d->x + 16)); \
  __m256i x3 = _mm256_loadu_si256((const __m256i *)(const void *)(d->x + 24)); \
  __m256i x4 = _mm256_loadu_si256((const __m256i *)(const void *)(d->x + 32)); \
  __m256i x5 = _mm256_loadu_si256((const __m256i *)(const void *)(d->x + 40)); \
  __m256i x6 = _mm256_loadu_si256((const __m256i *)(const void *)(d->x + 48)); \
  __m256i x7 = _mm256_loadu_si256((const __m256i *)(const void *)(d->x + 56)); \
  size_t i = 0; \
  for (; i < num && (size_t)(end - p) >= 128; i += 64) \
  { \
    __m256i s0, s1, s2, s3; \
    AVX2_STEP(x0, s0, ctx[i], P, LOG) \
    AVX2_STEP(x1, s1, ctx[i + 8], P, LOG) \
    AVX2_STEP(x2, s2, ctx[i + 16], P, LOG) \
    AVX2_STEP(x3, s3, ctx[i + 24], P, LOG) \
    /* bytes of 4 vectors: the packs interleave the 128-bit halves, the permutation \
       puts the 4-byte groups back in order */ \
    s0 = _mm256_packus_epi16(_mm256_packus_epi32(s0, s1), _mm256_packus_epi32(s2, s3)); \
    _mm256_storeu_si256((__m256i *)(void *)(out + i), _mm256_permutevar8x32_epi32(s0, vOrder)); \
    AVX2_STEP(x4, s0, ctx[i + 32], P, LOG) \
    AVX2_STEP(x5, s1, ctx[i + 40], P, LOG) \
    AVX2_STEP(x6, s2, ctx[i + 48], P, LOG) \
    AVX2_STEP(x7, s3, ctx[i + 56], P, LOG) \
    s0 = _mm256_packus_epi16(_mm256_packus_epi32(s0, s1), _mm256_packus_epi32(s2, s3)); \
    _mm256_storeu_si256((__m256i *)(void *)(out + i + 32), _mm256_permutevar8x32_epi32(s0, vOrder)); \
  } \
  _mm256_storeu_si256((__m256i *)(void *)d->x, x0); \
  _mm256_storeu_si256((__m256i *)(void *)(d->x + 8), x1); \
  _mm256_storeu_si256((__m256i *)(void *)(d->x + 16), x2); \
  _mm256_storeu_si256((__m256i *)(void *)(d->x + 24), x3); \
  _mm256_storeu_si256((__m256i *)(void *)(d->x + 32), x4); \
  _mm256_storeu_si256((__m256i *)(void *)(d->x + 40), x5); \
  _mm256_storeu_si256((__m256i *)(void *)(d->x + 48), x6); \
  _mm256_storeu_si256((__m256i *)(void *)(d->x + 56), x7); \
  d->p = p; \
  return i; \
}

DEFINE_DEC_AVX2(DecAvx2_9_4, 9, 4)
DEFINE_DEC_AVX2(DecAvx2_9_5, 9, 5)
DEFINE_DEC_AVX2(DecAvx2_11_6, 11, 6)

#endif

static Func_DecGroups Get_DecGroups(const CQMField *f, int simd)
{
  #ifdef FLOAT_QUANT_USE_AVX2
  if (simd && CPU_IsSupported_AVX2())
    switch (f->kind)
    {
      case QM_Q4K: return DecAvx2_9_4;
      case QM_Q5K: return DecAvx2_9_5;
      case QM_Q6K: return DecAvx2_11_6;
      default: break;
    }
  #else
  UNUSED_VAR(simd)
  #endif
  switch (f->kind)
  {
    case QM_Q4K: return Dec_Groups_9_4;
    case QM_Q5K: return Dec_Groups_9_5;
    case QM_Q6K: return Dec_Groups_11_6;
    case QM_SCALE6:
    case QM_MIN6: return Dec_Groups_10_6;
    default: return Dec_Groups_12_8;
  }
}

/* num symbols with the contexts ctx[] to out[] (the first at a multiple of N): the fast
   groups, then one at a time with the checks. Returns 0 if the stream ends before. */
static int Rans_Decode(CRansDec *d, Func_DecGroups groups, const Byte *ctx, Byte *out, size_t num)
{
  const unsigned P = d->P, logSyms = d->logSyms, N = d->N;
  size_t i = groups(d, ctx, out, num);
  for (; i < num; i++)
  {
    const Byte *tb = d->symTab + ((size_t)ctx[i] << P);
    const UInt32 *fb = d->fc + ((size_t)ctx[i] << logSyms);
    UInt32 xx = d->x[i & (N - 1)];
    unsigned s;
    RANS_DEC_SYM(xx, tb, fb, s, P)
    out[i] = (Byte)s;
    if (xx < kRansL)
    {
      if ((size_t)(d->end - d->p) < 2)
        return 0;
      xx = (xx << 16) | GetUi16(d->p);
      d->p += 2;
    }
    d->x[i & (N - 1)] = xx;
  }
  return 1;
}

SRes FloatQuant_Decode(const Byte *src, size_t srcLen, Byte *dest, size_t outLen, size_t *srcUsed, int simd)
{
  Byte symTab[kMaxTabSlots + kTabSlack];
  UInt32 fc[FLOAT_QUANT_MAX_TAB];
  Byte levels[FLOAT_QUANT_MAX_TAB];
  Byte sym[kGather * kMaxWidth], ctx[kGather * kMaxWidth];
  size_t tabSizes[FLOAT_QUANT_MAX_FIELDS], streamSizes[FLOAT_QUANT_MAX_FIELDS], total;
  CQMHeader h;
  const CQMType *t;
  const Byte *p;
  Byte *e;
  unsigned fi;

  RINOK(Parse_Header(src, srcLen, outLen, &h))
  if (srcLen < h.headerSize)
    return SZ_ERROR_INPUT_EOF;
  RINOK(Get_FieldSizes(src, &h, outLen, tabSizes, streamSizes, &total))
  if (srcLen - h.headerSize < total)
    return SZ_ERROR_INPUT_EOF;
  t = h.t;
  p = src + kHeaderSize;
  memcpy(dest, p, h.phase);                       p += h.phase;
  memcpy(dest + outLen - h.tail, p, h.tail);
  p = src + h.headerSize;
  e = dest + h.phase;

  for (fi = 0; fi < t->numFields; fi++)
  {
    const CQMField *f = &t->f[fi];
    const unsigned numSyms = 1u << f->logSyms, P = f->prec;
    const Func_DecGroups groups = Get_DecGroups(f, simd);
    const unsigned N = QM_STATES(f);
    CRansDec rd;
    CBitR br;
    size_t s;
    unsigned c, k;

    /* tables: unused contexts get one symbol (0) of frequency 2^P */
    br.p = p;
    br.lim = p + tabSizes[fi];
    br.acc = 0;
    br.cnt = 0;
    br.error = 0;
    if (!Tables_Read(&br, levels, f->numCtx, numSyms) || br.p != br.lim)
      return SZ_ERROR_DATA;
    p += tabSizes[fi];
    for (c = 0; c < f->numCtx; c++)
    {
      UInt32 freqs[256], cum = 0;
      Byte *st = symTab + ((size_t)c << P);
      if (!Norm_Freqs(levels + c * numSyms, numSyms, P, freqs))
      {
        memset(freqs, 0, numSyms * sizeof(freqs[0]));
        freqs[0] = (UInt32)1 << P;
      }
      for (k = 0; k < numSyms; k++)
      {
        fc[c * numSyms + k] = freqs[k] | (cum << 16);
        memset(st + cum, (int)k, freqs[k]);
        cum += freqs[k];
      }
    }

    rd.symTab = symTab;
    rd.fc = fc;
    rd.P = P;
    rd.logSyms = f->logSyms;
    rd.N = N;
    for (k = 0; k < N; k++)
    {
      rd.x[k] = GetUi32(p + 4 * k);
      if (rd.x[k] < kRansL || rd.x[k] >= ((UInt32)1 << 31))
        return SZ_ERROR_DATA;
    }
    rd.p = p + 4 * N;
    rd.end = p + streamSizes[fi];
    for (s = 0; s < h.n; s += kGather)
    {
      const size_t ns = h.n - s < kGather ? h.n - s : kGather;
      Byte *q = e + s * t->size;
      Q_Contexts(q, t->size, f, ns, ctx);
      if (!Rans_Decode(&rd, groups, ctx, sym, ns * f->width))
        return SZ_ERROR_DATA;
      Q_Join(q, t->size, f, ns, sym);
    }
    /* the stream ends with the initial states of the encoder, and is used up */
    for (k = 0; k < N; k++)
      if (rd.x[k] != kRansL)
        return SZ_ERROR_DATA;
    if (rd.p != rd.end)
      return SZ_ERROR_DATA;
    p = rd.end;
  }
  *srcUsed = (size_t)(p - src);
  return SZ_OK;
}
