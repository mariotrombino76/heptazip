/* FloatPlane.c -- lossless coder for arrays of floating-point numbers (AI model weights)
7zip-modern (design 0008) : Public domain
The block format is described in FloatPlane.h. */

#include "Precomp.h"

#include <string.h>

#include "CpuArch.h"
#include "FloatPlane.h"
#include "HuffEnc.h"

#define kNumSyms     256
#define kMaxLen      12
#define kTableBits   kMaxLen
#define kLensSize    (kNumSyms / 2)
#define kSampleSize  ((size_t)1 << 16)

#define MODE_STORED  0
#define MODE_SPLIT   1

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

/* exponent byte and the other bits of x */
#define SPLIT_E(x, s)  (((x) >> (s)) & 0xFF)
#define SPLIT_R(x, s)  (((x) & (((UInt32)1 << (s)) - 1)) | (((x) >> ((s) + 8)) << (s)))
#define JOIN(e, r, s)  (((r) & (((UInt32)1 << (s)) - 1)) | ((UInt32)(e) << (s)) | (((r) >> (s)) << ((s) + 8)))


/* ---------- Encoder ---------- */

/* Order-0 cost of the exponent bytes in bits, fixed point (x 2^16): a log2 table is
   enough to rank the candidates of the sample. */
static UInt64 Hist_Cost(const UInt32 *hist, UInt32 total)
{
  /* cost = sum f * log2(total / f) ~ sum f * (log2(total) - log2(f)) */
  UInt64 cost = 0;
  unsigned i;
  for (i = 0; i < kNumSyms; i++)
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

static void Histogram(const Byte *src, size_t n, const CFloatType *t, UInt32 *hist)
{
  size_t i;
  const unsigned w = t->w, s = t->shift;
  memset(hist, 0, kNumSyms * sizeof(hist[0]));
  for (i = 0; i < n; i++)
    hist[SPLIT_E(LoadElem(src + i * w, w), s)]++;
}

static void Canonical_Codes(const Byte *lens, UInt32 *codes)
{
  unsigned counts[kMaxLen + 1];
  UInt32 next[kMaxLen + 1];
  UInt32 code = 0;
  unsigned i;
  memset(counts, 0, sizeof(counts));
  for (i = 0; i < kNumSyms; i++)
    counts[lens[i]]++;
  counts[0] = 0;
  for (i = 1; i <= kMaxLen; i++)
  {
    code = (code + counts[i - 1]) << 1;
    next[i] = code;
  }
  for (i = 0; i < kNumSyms; i++)
    codes[i] = lens[i] ? next[lens[i]]++ : 0;
}

size_t FloatPlane_EncodeBlock(const Byte *src, size_t srcLen, Byte *dest)
{
  UInt32 hist[kNumSyms];
  unsigned bestType = 0, bestPhase = 0;
  UInt64 bestCost = (UInt64)(Int64)-1;
  unsigned t;

  /* 1. Type and phase from a sample: cost per byte of exponent + raw rest. */
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
        cost = Hist_Cost(hist, (UInt32)n) + ((UInt64)n * (ft->w - 1) * 8 << 16);
        /* normalize to the bytes covered: n * w */
        cost = cost / (n * ft->w);
        if (cost < bestCost)
        {
          bestCost = cost;
          bestType = t;
          bestPhase = phase;
        }
      }
    }
  }

  /* 2. Exact size of the split block, with the Huffman code of the whole block. */
  if (bestCost != (UInt64)(Int64)-1)
  {
    const CFloatType *ft = &g_Types[bestType];
    const unsigned w = ft->w, s = ft->shift;
    const size_t n = (srcLen - bestPhase) / w;
    const size_t tail = srcLen - bestPhase - n * w;
    UInt32 p[kNumSyms];
    UInt32 codes[kNumSyms];
    Byte lens[kNumSyms];
    UInt64 numBits = 0;
    size_t huffSize, total;
    unsigned i;

    Histogram(src + bestPhase, n, ft, hist);
    Huffman_Generate(hist, p, lens, kNumSyms, kMaxLen);
    for (i = 0; i < kNumSyms; i++)
      numBits += (UInt64)hist[i] * lens[i];
    huffSize = (size_t)((numBits + 7) >> 3);
    total = 7 + bestPhase + tail + kLensSize + 4 + huffSize + n * (w - 1);

    if (total < FLOAT_PLANE_ENCODED_MAX(srcLen))
    {
      Byte *d = dest;
      const Byte *e = src + bestPhase;
      UInt64 acc = 0;
      unsigned cnt = 0;
      size_t k;

      Canonical_Codes(lens, codes);
      *d++ = MODE_SPLIT;
      SetUi32(d, (UInt32)srcLen)  d += 4;
      *d++ = (Byte)bestType;
      *d++ = (Byte)bestPhase;
      memcpy(d, src, bestPhase);                    d += bestPhase;
      memcpy(d, src + srcLen - tail, tail);         d += tail;
      for (i = 0; i < kNumSyms; i += 2)
        *d++ = (Byte)(lens[i] | (lens[i + 1] << 4));
      SetUi32(d, (UInt32)huffSize)  d += 4;

      /* Huffman codes of e, MSB first */
      for (k = 0; k < n; k++)
      {
        const unsigned sym = SPLIT_E(LoadElem(e + k * w, w), s);
        acc = (acc << lens[sym]) | codes[sym];
        cnt += lens[sym];
        if (cnt >= 32)
        {
          cnt -= 32;
          SetBe32(d, (UInt32)(acc >> cnt))
          d += 4;
        }
      }
      while (cnt >= 8)
      {
        cnt -= 8;
        *d++ = (Byte)(acc >> cnt);
      }
      if (cnt != 0)
        *d++ = (Byte)(acc << (8 - cnt));

      /* r, w - 1 bytes per element */
      if (w == 2)
        for (k = 0; k < n; k++)
          *d++ = (Byte)SPLIT_R(GetUi16(e + k * 2), s);
      else
        for (k = 0; k < n; k++)
        {
          const UInt32 r = SPLIT_R(GetUi32(e + k * 4), s);
          d[0] = (Byte)r;
          d[1] = (Byte)(r >> 8);
          d[2] = (Byte)(r >> 16);
          d += 3;
        }
      return (size_t)(d - dest);
    }
  }

  dest[0] = MODE_STORED;
  SetUi32(dest + 1, (UInt32)srcLen)
  memcpy(dest + 5, src, srcLen);
  return srcLen + 5;
}


/* ---------- Decoder ---------- */

SRes FloatPlane_GetBlockOutLen(const Byte *src, size_t srcLen, size_t *outLen)
{
  UInt32 len;
  if (srcLen < 5 || src[0] > MODE_SPLIT)
    return SZ_ERROR_DATA;
  len = GetUi32(src + 1);
  if (len == 0 || len > FLOAT_PLANE_BLOCK_MAX)
    return SZ_ERROR_DATA;
  *outLen = len;
  return SZ_OK;
}

SRes FloatPlane_GetBlockSize(const Byte *src, size_t srcLen, size_t *blockSize)
{
  size_t outLen, n, tail, headerSize;
  unsigned w, phase;
  if (srcLen < 5)
    return SZ_ERROR_INPUT_EOF;
  RINOK(FloatPlane_GetBlockOutLen(src, srcLen, &outLen))
  if (src[0] == MODE_STORED)
  {
    *blockSize = 5 + outLen;
    return SZ_OK;
  }
  if (srcLen < 7)
    return SZ_ERROR_INPUT_EOF;
  if (src[5] >= kNumTypes)
    return SZ_ERROR_DATA;
  w = g_Types[src[5]].w;
  phase = src[6];
  if (phase >= w || outLen < phase)
    return SZ_ERROR_DATA;
  n = (outLen - phase) / w;
  tail = outLen - phase - n * w;
  headerSize = 7 + phase + tail + kLensSize + 4;
  if (srcLen < headerSize)
    return SZ_ERROR_INPUT_EOF;
  /* the same limits as FloatPlane_DecodeBlock() */
  {
    const size_t huffSize = GetUi32(src + headerSize - 4);
    if (huffSize > ((n * kMaxLen + 7) >> 3) || huffSize < ((n + 7) >> 3))
      return SZ_ERROR_DATA;
    *blockSize = headerSize + huffSize + n * (w - 1);
  }
  return SZ_OK;
}

/* table[v] for the next kTableBits bits v: symbol | (length << 8); 0 = not a code */
static int Build_Table(const Byte *packedLens, UInt16 *table)
{
  Byte lens[kNumSyms];
  UInt32 codes[kNumSyms];
  unsigned counts[kMaxLen + 1];
  UInt32 kraft = 0;
  unsigned i;
  memset(counts, 0, sizeof(counts));
  for (i = 0; i < kNumSyms; i += 2)
  {
    lens[i] = (Byte)(packedLens[i / 2] & 15);
    lens[i + 1] = (Byte)(packedLens[i / 2] >> 4);
  }
  for (i = 0; i < kNumSyms; i++)
  {
    if (lens[i] > kMaxLen)
      return 0;
    counts[lens[i]]++;
  }
  for (i = 1; i <= kMaxLen; i++)
    kraft += (UInt32)counts[i] << (kMaxLen - i);
  if (kraft > ((UInt32)1 << kMaxLen))
    return 0;   /* over-subscribed */
  Canonical_Codes(lens, codes);
  memset(table, 0, sizeof(UInt16) << kTableBits);
  for (i = 0; i < kNumSyms; i++)
  {
    const unsigned len = lens[i];
    if (len != 0)
    {
      const UInt32 first = codes[i] << (kMaxLen - len);
      const UInt32 num = (UInt32)1 << (kMaxLen - len);
      const UInt16 v = (UInt16)(i | (len << 8));
      UInt32 k;
      for (k = 0; k < num; k++)
        table[first + k] = v;
    }
  }
  return 1;
}

SRes FloatPlane_DecodeBlock(const Byte *src, size_t srcLen, Byte *dest, size_t destLen, size_t *srcUsed)
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

  {
    UInt16 table[(size_t)1 << kTableBits];
    const CFloatType *ft;
    unsigned w, s, phase;
    size_t n, tail, huffSize, k;
    const Byte *h, *hEnd, *r;
    Byte *e;
    UInt64 bits = 0;
    unsigned cnt = 0;      /* valid bits in (bits), MSB aligned */
    size_t padBytes = 0;   /* zero bytes added after the end of the Huffman stream */

    if (srcEnd - p < 2)
      return SZ_ERROR_INPUT_EOF;
    if (p[0] >= kNumTypes)
      return SZ_ERROR_DATA;
    ft = &g_Types[p[0]];
    w = ft->w;
    s = ft->shift;
    phase = p[1];
    p += 2;
    if (phase >= w || outLen < phase)
      return SZ_ERROR_DATA;
    n = (outLen - phase) / w;
    tail = outLen - phase - n * w;

    if ((size_t)(srcEnd - p) < phase + tail + kLensSize + 4)
      return SZ_ERROR_INPUT_EOF;
    memcpy(dest, p, phase);                    p += phase;
    memcpy(dest + outLen - tail, p, tail);     p += tail;
    if (!Build_Table(p, table))
      return SZ_ERROR_DATA;
    p += kLensSize;
    huffSize = GetUi32(p);
    p += 4;
    /* the stream needs at least 1 bit per code, at most kMaxLen */
    if (huffSize > ((n * kMaxLen + 7) >> 3) || huffSize < ((n + 7) >> 3))
      return SZ_ERROR_DATA;
    if ((size_t)(srcEnd - p) < huffSize || (size_t)(srcEnd - p) - huffSize < n * (w - 1))
      return SZ_ERROR_INPUT_EOF;
    h = p;
    hEnd = p + huffSize;
    r = hEnd;
    e = dest + phase;

    #define REFILL \
      if (hEnd - h >= 8) \
      { \
        bits |= GetBe64(h) >> cnt; \
        h += (63 - cnt) >> 3; \
        cnt |= 56; \
      } \
      else \
        while (cnt <= 56) \
        { \
          if (h != hEnd) \
            bits |= (UInt64)*h++ << (56 - cnt); \
          else \
            padBytes++; \
          cnt += 8; \
        }

    #define DECODE_SYM(sym) \
    { \
      const unsigned v = table[(size_t)(bits >> (64 - kTableBits))]; \
      const unsigned len = v >> 8; \
      if (len == 0) \
        return SZ_ERROR_DATA; \
      sym = v & 0xFF; \
      bits <<= len; \
      cnt -= len; \
    }

    /* separate loops with a constant shift: JOIN() becomes a few instructions */
    #define DECODE_LOOP_2(S) \
    { \
      for (k = 0; k + 4 <= n; k += 4) \
      { \
        unsigned s0, s1, s2, s3; \
        REFILL \
        DECODE_SYM(s0) \
        DECODE_SYM(s1) \
        DECODE_SYM(s2) \
        DECODE_SYM(s3) \
        SetUi16(e + k * 2 + 0, (UInt16)JOIN(s0, (UInt32)r[k + 0], S)) \
        SetUi16(e + k * 2 + 2, (UInt16)JOIN(s1, (UInt32)r[k + 1], S)) \
        SetUi16(e + k * 2 + 4, (UInt16)JOIN(s2, (UInt32)r[k + 2], S)) \
        SetUi16(e + k * 2 + 6, (UInt16)JOIN(s3, (UInt32)r[k + 3], S)) \
      } \
      for (; k < n; k++) \
      { \
        unsigned s0; \
        REFILL \
        DECODE_SYM(s0) \
        SetUi16(e + k * 2, (UInt16)JOIN(s0, (UInt32)r[k], S)) \
      } \
    }

    if (s == 7)
      DECODE_LOOP_2(7)
    else if (s == 8)
      DECODE_LOOP_2(8)
    else
      for (k = 0; k < n; k++)
      {
        unsigned s0;
        UInt32 rv;
        REFILL
        DECODE_SYM(s0)
        rv = (UInt32)r[k * 3] | ((UInt32)r[k * 3 + 1] << 8) | ((UInt32)r[k * 3 + 2] << 16);
        SetUi32(e + k * 4, JOIN(s0, rv, 23))
      }

    {
      /* bits used by the n codes: everything loaded minus what is left in (bits) */
      const UInt64 used = ((UInt64)(size_t)(h - (hEnd - huffSize)) + padBytes) * 8 - cnt;
      if (((used + 7) >> 3) != huffSize)
        return SZ_ERROR_DATA;
    }
    *srcUsed = (size_t)(r + n * (w - 1) - src);
    return SZ_OK;
  }
}
