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
#define kSampleSize  ((size_t)1 << 14)   /* to choose type and phase: 8192 BF16 elements */
#define kNumStreams  4     /* element k is in Huffman stream k % 4 */

#define MODE_STORED  0
#define MODE_SPLIT   1
#define MODE_LZ      2

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

/* histogram of each stream: hist4[j * 256 + sym] for the elements k % 4 == j */
static void Histogram4(const Byte *src, size_t n, const CFloatType *t, UInt32 *hist4)
{
  size_t i;
  const unsigned w = t->w, s = t->shift;
  memset(hist4, 0, kNumStreams * kNumSyms * sizeof(hist4[0]));
  for (i = 0; i < n; i++)
    hist4[(i & 3) * kNumSyms + SPLIT_E(LoadElem(src + i * w, w), s)]++;
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

/* Writes the split block (mode 1) whose stream sizes were computed by the caller. */
static size_t Write_Split(const Byte *src, size_t srcLen, Byte *dest,
    unsigned type, unsigned phase, const Byte *lens, const size_t *huffSizes)
{
  const CFloatType *ft = &g_Types[type];
  const unsigned w = ft->w, s = ft->shift;
  const size_t n = (srcLen - phase) / w;
  const size_t tail = srcLen - phase - n * w;
  const Byte *e = src + phase;
  UInt32 codes[kNumSyms];
  Byte *d = dest;
  Byte *ds[kNumStreams];
  UInt64 acc[kNumStreams];
  unsigned cnt[kNumStreams];
  size_t k;
  unsigned i, j;

  Canonical_Codes(lens, codes);
  *d++ = MODE_SPLIT;
  SetUi32(d, (UInt32)srcLen)  d += 4;
  *d++ = (Byte)type;
  *d++ = (Byte)phase;
  memcpy(d, src, phase);                    d += phase;
  memcpy(d, src + srcLen - tail, tail);     d += tail;
  for (i = 0; i < kNumSyms; i += 2)
    *d++ = (Byte)(lens[i] | (lens[i + 1] << 4));
  for (j = 0; j < kNumStreams; j++)
  {
    SetUi32(d, (UInt32)huffSizes[j])  d += 4;
  }
  for (j = 0; j < kNumStreams; j++)
  {
    ds[j] = d;
    d += huffSizes[j];
    acc[j] = 0;
    cnt[j] = 0;
  }
  /* d: the r plane, written in the same pass as the codes */

  /* Huffman codes of e, MSB first, element k in stream k % 4: 4 elements per iteration
     with the writers in local variables, then the last elements */
  {
    Byte *d0 = ds[0], *d1 = ds[1], *d2 = ds[2], *d3 = ds[3];
    UInt64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    #define PUT(A, C, D, i) \
    { \
      const UInt32 x = LoadElem(e + (i) * w, w); \
      const unsigned sym = SPLIT_E(x, s); \
      PUT_R(i, x) \
      A = (A << lens[sym]) | codes[sym]; \
      C += lens[sym]; \
      if (C >= 32) \
      { \
        C -= 32; \
        SetBe32(D, (UInt32)(A >> C)) \
        D += 4; \
      } \
    }
    /* r of element i: w - 1 bytes, little-endian */
    #define PUT_R(i, x) \
    { \
      const UInt32 rv = SPLIT_R(x, s); \
      if (w == 2) \
        d[i] = (Byte)rv; \
      else \
      { \
        d[(i) * 3] = (Byte)rv; \
        d[(i) * 3 + 1] = (Byte)(rv >> 8); \
        d[(i) * 3 + 2] = (Byte)(rv >> 16); \
      } \
    }
    for (k = 0; k + 4 <= n; k += 4)
    {
      PUT(a0, c0, d0, k + 0)
      PUT(a1, c1, d1, k + 1)
      PUT(a2, c2, d2, k + 2)
      PUT(a3, c3, d3, k + 3)
    }
    ds[0] = d0; ds[1] = d1; ds[2] = d2; ds[3] = d3;
    acc[0] = a0; acc[1] = a1; acc[2] = a2; acc[3] = a3;
    cnt[0] = c0; cnt[1] = c1; cnt[2] = c2; cnt[3] = c3;
  }
  for (; k < n; k++)
  {
    const UInt32 x = LoadElem(e + k * w, w);
    const unsigned sym = SPLIT_E(x, s);
    PUT_R(k, x)
    j = (unsigned)k & 3;
    acc[j] = (acc[j] << lens[sym]) | codes[sym];
    cnt[j] += lens[sym];
    if (cnt[j] >= 32)
    {
      cnt[j] -= 32;
      SetBe32(ds[j], (UInt32)(acc[j] >> cnt[j]))
      ds[j] += 4;
    }
  }
  for (j = 0; j < kNumStreams; j++)
  {
    while (cnt[j] >= 8)
    {
      cnt[j] -= 8;
      *ds[j]++ = (Byte)(acc[j] >> cnt[j]);
    }
    if (cnt[j] != 0)
      *ds[j]++ = (Byte)(acc[j] << (8 - cnt[j]));
  }

  d += n * (w - 1);
  return (size_t)(d - dest);
}

#define kLzProbeSize  ((size_t)1 << 14)
#define kLzHeaderSize 9

size_t FloatPlane_EncodeBlock(const Byte *src, size_t srcLen, Byte *dest, const CFloatPlaneLz *lz)
{
  UInt32 hist[kNumSyms];
  unsigned bestType = 0, bestPhase = 0;
  UInt64 bestCost = (UInt64)8 << 16;   /* bits per byte (16.16) of the best candidate; 8 = stored */
  const size_t storedTotal = FLOAT_PLANE_ENCODED_MAX(srcLen);
  size_t splitTotal = storedTotal, lzTotal = storedTotal;
  size_t huffSizes[kNumStreams];
  Byte lens[kNumSyms];
  unsigned t;
  int haveCandidate = 0;

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

  /* 2. Exact size of the split block, with the Huffman code of the whole block. */
  if (haveCandidate)
  {
    const CFloatType *ft = &g_Types[bestType];
    const unsigned w = ft->w;
    const size_t n = (srcLen - bestPhase) / w;
    const size_t tail = srcLen - bestPhase - n * w;
    UInt32 p[kNumSyms];
    UInt32 hist4[kNumStreams * kNumSyms];
    size_t huffTotal = 0;
    unsigned i, j;
    Histogram4(src + bestPhase, n, ft, hist4);
    for (i = 0; i < kNumSyms; i++)
      hist[i] = hist4[i] + hist4[kNumSyms + i] + hist4[2 * kNumSyms + i] + hist4[3 * kNumSyms + i];
    Huffman_Generate(hist, p, lens, kNumSyms, kMaxLen);
    for (j = 0; j < kNumStreams; j++)
    {
      UInt64 numBits = 0;
      for (i = 0; i < kNumSyms; i++)
        numBits += (UInt64)hist4[j * kNumSyms + i] * lens[i];
      huffSizes[j] = (size_t)((numBits + 7) >> 3);
      huffTotal += huffSizes[j];
    }
    splitTotal = 7 + bestPhase + tail + kLensSize + 4 * kNumStreams + huffTotal + n * (w - 1);
  }

  /* 3. zstd (mode 2), if a probe of the block start compresses better than the float
     estimate: then the whole block, and the smaller result wins. The frame is written in
     place (dest + 9); a split or stored block overwrites it. */
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

  if (lzTotal < storedTotal && lzTotal <= splitTotal)
  {
    dest[0] = MODE_LZ;
    SetUi32(dest + 1, (UInt32)srcLen)
    SetUi32(dest + 5, (UInt32)(lzTotal - kLzHeaderSize))
    return lzTotal;
  }
  if (splitTotal < storedTotal)
    return Write_Split(src, srcLen, dest, bestType, bestPhase, lens, huffSizes);

  dest[0] = MODE_STORED;
  SetUi32(dest + 1, (UInt32)srcLen)
  memcpy(dest + 5, src, srcLen);
  return srcLen + 5;
}


/* ---------- Decoder ---------- */

SRes FloatPlane_GetBlockOutLen(const Byte *src, size_t srcLen, size_t *outLen)
{
  UInt32 len;
  if (srcLen < 5 || src[0] > MODE_LZ)
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
  headerSize = 7 + phase + tail + kLensSize + 4 * kNumStreams;
  if (srcLen < headerSize)
    return SZ_ERROR_INPUT_EOF;
  {
    size_t huffTotal;
    RINOK(Get_StreamSizes(src + headerSize - 4 * kNumStreams, n, NULL, &huffTotal))
    *blockSize = headerSize + huffTotal + n * (w - 1);
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

  {
    UInt16 table[(size_t)1 << kTableBits];
    const CFloatType *ft;
    unsigned w, s, phase, j;
    size_t n, tail, k, huffTotal;
    size_t sizes[kNumStreams];
    const Byte *r;
    Byte *e;
    /* the 4 bit readers: (bits) has (cnt) valid bits, MSB aligned; pad = zero bytes
       read after the end of the stream (an error if they are consumed) */
    const Byte *hp[kNumStreams], *hEnd[kNumStreams], *hStart[kNumStreams];
    UInt64 bits[kNumStreams];
    unsigned cnt[kNumStreams];
    size_t pad[kNumStreams];

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

    if ((size_t)(srcEnd - p) < phase + tail + kLensSize + 4 * kNumStreams)
      return SZ_ERROR_INPUT_EOF;
    memcpy(dest, p, phase);                    p += phase;
    memcpy(dest + outLen - tail, p, tail);     p += tail;
    if (!Build_Table(p, table))
      return SZ_ERROR_DATA;
    p += kLensSize;
    RINOK(Get_StreamSizes(p, n, sizes, &huffTotal))
    p += 4 * kNumStreams;
    if ((size_t)(srcEnd - p) < huffTotal || (size_t)(srcEnd - p) - huffTotal < n * (w - 1))
      return SZ_ERROR_INPUT_EOF;
    for (j = 0; j < kNumStreams; j++)
    {
      hStart[j] = hp[j] = p;
      p += sizes[j];
      hEnd[j] = p;
      bits[j] = 0;
      cnt[j] = 0;
      pad[j] = 0;
    }
    r = p;
    e = dest + phase;
    k = 0;

    #define DECODE_SYM(B, C, sym) \
    { \
      const unsigned v = table[(size_t)((B) >> (64 - kTableBits))]; \
      const unsigned len = v >> 8; \
      if (len == 0) \
        return SZ_ERROR_DATA; \
      sym = v & 0xFF; \
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

    /* Main loop: 16 elements per iteration, 4 from each stream (<= 48 bits per refill).
       The 4 streams are independent chains: the CPU decodes them in parallel. */
    #define DECODE_MAIN(STORE) \
    { \
      const Byte *h0 = hp[0], *h1 = hp[1], *h2 = hp[2], *h3 = hp[3]; \
      UInt64 b0 = 0, b1 = 0, b2 = 0, b3 = 0; \
      unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0; \
      for (; k + 16 <= n; k += 16) \
      { \
        unsigned rr; \
        if (hEnd[0] - h0 < 8 || hEnd[1] - h1 < 8 || hEnd[2] - h2 < 8 || hEnd[3] - h3 < 8) \
          break; \
        REFILL_FAST(h0, b0, c0) \
        REFILL_FAST(h1, b1, c1) \
        REFILL_FAST(h2, b2, c2) \
        REFILL_FAST(h3, b3, c3) \
        for (rr = 0; rr < 4; rr++) \
        { \
          unsigned s0, s1, s2, s3; \
          const size_t kk = k + rr * 4; \
          DECODE_SYM(b0, c0, s0) \
          DECODE_SYM(b1, c1, s1) \
          DECODE_SYM(b2, c2, s2) \
          DECODE_SYM(b3, c3, s3) \
          STORE(kk + 0, s0) \
          STORE(kk + 1, s1) \
          STORE(kk + 2, s2) \
          STORE(kk + 3, s3) \
        } \
      } \
      hp[0] = h0; hp[1] = h1; hp[2] = h2; hp[3] = h3; \
      bits[0] = b0; bits[1] = b1; bits[2] = b2; bits[3] = b3; \
      cnt[0] = c0; cnt[1] = c1; cnt[2] = c2; cnt[3] = c3; \
    }

    /* the last elements, one at a time, with a careful refill near the stream end */
    #define DECODE_TAIL(STORE) \
      for (; k < n; k++) \
      { \
        unsigned s0; \
        j = (unsigned)k & 3; \
        if (hEnd[j] - hp[j] >= 8) \
          REFILL_FAST(hp[j], bits[j], cnt[j]) \
        else \
          while (cnt[j] <= 56) \
          { \
            if (hp[j] != hEnd[j]) \
              bits[j] |= (UInt64)*hp[j]++ << (56 - cnt[j]); \
            else \
              pad[j]++; \
            cnt[j] += 8; \
          } \
        DECODE_SYM(bits[j], cnt[j], s0) \
        STORE(k, s0) \
      }

    #define STORE_BF16(i, sym)  SetUi16(e + (i) * 2, (UInt16)JOIN(sym, (UInt32)r[i], 7))
    #define STORE_FP16(i, sym)  SetUi16(e + (i) * 2, (UInt16)JOIN(sym, (UInt32)r[i], 8))
    #define STORE_FP32(i, sym) \
      SetUi32(e + (i) * 4, JOIN(sym, (UInt32)r[(i) * 3] | ((UInt32)r[(i) * 3 + 1] << 8) \
          | ((UInt32)r[(i) * 3 + 2] << 16), 23))

    /* separate loops with a constant shift: JOIN() becomes a few instructions */
    if (s == 7)
    {
      DECODE_MAIN(STORE_BF16)
      DECODE_TAIL(STORE_BF16)
    }
    else if (s == 8)
    {
      DECODE_MAIN(STORE_FP16)
      DECODE_TAIL(STORE_FP16)
    }
    else
    {
      DECODE_MAIN(STORE_FP32)
      DECODE_TAIL(STORE_FP32)
    }

    /* each stream: the bits used by its codes (all loaded minus those left) fill exactly
       its declared size */
    for (j = 0; j < kNumStreams; j++)
    {
      const UInt64 used = ((UInt64)(size_t)(hp[j] - hStart[j]) + pad[j]) * 8 - cnt[j];
      if (((used + 7) >> 3) != sizes[j])
        return SZ_ERROR_DATA;
    }
    *srcUsed = (size_t)(r + n * (w - 1) - src);
    return SZ_OK;
  }
}
