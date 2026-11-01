/* FloatPlane.c -- lossless coder for arrays of floating-point numbers (AI model weights)
7zip-modern (design 0008) : Public domain
The block format is described in FloatPlane.h. */

#include "Precomp.h"

#include <string.h>

#include "CpuArch.h"
#include "FloatPlane.h"
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
  UInt64 acc[kNumStreams], racc = 0;
  unsigned cnt[kNumStreams], rcnt = 0;
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
    acc[j] = 0;
    cnt[j] = 0;
  }
  dr = d;   /* the packed r plane, written in the same pass as the codes */

  #define PUT_CODE(A, C, D, sym) \
  { \
    A = (A << lens[sym]) | codes[sym]; \
    C += lens[sym]; \
    if (C >= 32) \
    { \
      C -= 32; \
      SetBe32(D, (UInt32)(A >> C)) \
      D += 4; \
    } \
  }
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

  /* 4 elements per iteration with the code writers in local variables */
  {
    Byte *d0 = ds[0], *d1 = ds[1], *d2 = ds[2], *d3 = ds[3];
    UInt64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (i = 0; i + 4 <= n; i += 4)
    {
      const UInt32 x0 = LoadElem(e + (i + 0) * w, w);
      const UInt32 x1 = LoadElem(e + (i + 1) * w, w);
      const UInt32 x2 = LoadElem(e + (i + 2) * w, w);
      const UInt32 x3 = LoadElem(e + (i + 3) * w, w);
      PUT_CODE(a0, c0, d0, SPLIT_E(x0, SK, K))
      PUT_CODE(a1, c1, d1, SPLIT_E(x1, SK, K))
      PUT_CODE(a2, c2, d2, SPLIT_E(x2, SK, K))
      PUT_CODE(a3, c3, d3, SPLIT_E(x3, SK, K))
      PUT_R(x0)
      PUT_R(x1)
      PUT_R(x2)
      PUT_R(x3)
    }
    ds[0] = d0; ds[1] = d1; ds[2] = d2; ds[3] = d3;
    acc[0] = a0; acc[1] = a1; acc[2] = a2; acc[3] = a3;
    cnt[0] = c0; cnt[1] = c1; cnt[2] = c2; cnt[3] = c3;
  }
  for (; i < n; i++)
  {
    const UInt32 x = LoadElem(e + i * w, w);
    j = (unsigned)i & 3;
    PUT_CODE(acc[j], cnt[j], ds[j], SPLIT_E(x, SK, K))
    PUT_R(x)
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
  while (rcnt > 0)
  {
    *dr++ = (Byte)racc;
    racc >>= 8;
    rcnt = rcnt > 8 ? rcnt - 8 : 0;
  }
  return (size_t)(dr - dest);
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

  /* 2. k = 0, 1 or 2 mantissa bits in the symbol, by estimated size; then the Huffman code
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

/* state of the 4 bit readers and of the output: (bits) has (cnt) valid bits, MSB aligned;
   pad = zero bytes read after the end of a stream (an error if they are consumed) */
typedef struct
{
  const UInt16 *table;
  const Byte *r;       /* the packed r plane */
  size_t rBytes;
  size_t n;            /* elements */
  size_t nSafe;        /* the fast loop stops before this element (8-byte r loads) */
  Byte *e;             /* the first element in dest */
  const Byte *hp[kNumStreams];
  const Byte *hEnd[kNumStreams];
  UInt64 bits[kNumStreams];
  unsigned cnt[kNumStreams];
  size_t pad[kNumStreams];
} CDecState;

#define LOAD_R64(i, RB)  (GetUi64(r + (((i) * (RB)) >> 3)) >> (((i) * (RB)) & 7))
#define FIELD(v, t, RB)  ((UInt32)((v) >> ((t) * (RB))) & (((UInt32)1 << (RB)) - 1))

#define DECODE_SYM(B, C, sym) \
{ \
  const unsigned v = table[(size_t)((B) >> (64 - kTableBits))]; \
  const unsigned len = v >> 10; \
  if (len == 0) \
    return SZ_ERROR_DATA; \
  sym = v & 0x3FF; \
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
/* one element from each stream: elements ii .. ii + 3 */
#define DECODE_ROUND(STORE4, SK, K, ii) \
{ \
  unsigned s0, s1, s2, s3; \
  DECODE_SYM(b0, c0, s0) \
  DECODE_SYM(b1, c1, s1) \
  DECODE_SYM(b2, c2, s2) \
  DECODE_SYM(b3, c3, s3) \
  STORE4(ii, s0, s1, s2, s3, SK, K) \
}

#define DECODE_MAIN(STORE4, SK, K) \
{ \
  const Byte *h0 = hp[0], *h1 = hp[1], *h2 = hp[2], *h3 = hp[3]; \
  UInt64 b0 = 0, b1 = 0, b2 = 0, b3 = 0; \
  unsigned c0 = 0, c1 = 0, c2 = 0, c3 = 0; \
  for (; i + 16 <= nSafe; i += 16) \
  { \
    if (hEnd[0] - h0 < 8 || hEnd[1] - h1 < 8 || hEnd[2] - h2 < 8 || hEnd[3] - h3 < 8) \
      break; \
    REFILL_FAST(h0, b0, c0) \
    REFILL_FAST(h1, b1, c1) \
    REFILL_FAST(h2, b2, c2) \
    REFILL_FAST(h3, b3, c3) \
    /* 4 rounds written out: GCC -O2 does not unroll this loop by itself */ \
    DECODE_ROUND(STORE4, SK, K, i) \
    DECODE_ROUND(STORE4, SK, K, i + 4) \
    DECODE_ROUND(STORE4, SK, K, i + 8) \
    DECODE_ROUND(STORE4, SK, K, i + 12) \
  } \
  hp[0] = h0; hp[1] = h1; hp[2] = h2; hp[3] = h3; \
  bits[0] = b0; bits[1] = b1; bits[2] = b2; bits[3] = b3; \
  cnt[0] = c0; cnt[1] = c1; cnt[2] = c2; cnt[3] = c3; \
}

/* the last elements, one at a time, with careful reads near the ends */
#define DECODE_TAIL(STORE) \
  for (; i < n; i++) \
  { \
    unsigned s0; \
    j = (unsigned)i & 3; \
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
    STORE(i, s0, Read_R_Safe) \
  }

/* 4 elements from i (i % 4 == 0): w = 2, 4 * RB <= 32 bits from one 8-byte load
   (bytes when RB = 8); w = 4, two loads of 2 * RB <= 48 bits */
#define STORE4_2(i, a, b, c, d, SK, K) \
{ \
  UInt32 r0, r1, r2, r3; \
  if ((K) == 0) \
  { \
    r0 = r[(i) + 0]; r1 = r[(i) + 1]; r2 = r[(i) + 2]; r3 = r[(i) + 3]; \
  } \
  else \
  { \
    const UInt64 v = LOAD_R64(i, R_BITS(2, K)); \
    r0 = FIELD(v, 0, R_BITS(2, K)); r1 = FIELD(v, 1, R_BITS(2, K)); \
    r2 = FIELD(v, 2, R_BITS(2, K)); r3 = FIELD(v, 3, R_BITS(2, K)); \
  } \
  SetUi16(e + (i) * 2 + 0, (UInt16)JOIN(a, r0, SK, K)) \
  SetUi16(e + (i) * 2 + 2, (UInt16)JOIN(b, r1, SK, K)) \
  SetUi16(e + (i) * 2 + 4, (UInt16)JOIN(c, r2, SK, K)) \
  SetUi16(e + (i) * 2 + 6, (UInt16)JOIN(d, r3, SK, K)) \
}
#define STORE4_4(i, a, b, c, d, SK, K) \
{ \
  const UInt64 v0 = LOAD_R64(i, R_BITS(4, K)); \
  const UInt64 v1 = LOAD_R64((i) + 2, R_BITS(4, K)); \
  SetUi32(e + (i) * 4 + 0,  JOIN(a, FIELD(v0, 0, R_BITS(4, K)), SK, K)) \
  SetUi32(e + (i) * 4 + 4,  JOIN(b, FIELD(v0, 1, R_BITS(4, K)), SK, K)) \
  SetUi32(e + (i) * 4 + 8,  JOIN(c, FIELD(v1, 0, R_BITS(4, K)), SK, K)) \
  SetUi32(e + (i) * 4 + 12, JOIN(d, FIELD(v1, 1, R_BITS(4, K)), SK, K)) \
}

#define STORE_2(i, sym, READ, SK, K) \
  SetUi16(e + (i) * 2, (UInt16)JOIN(sym, READ(i, R_BITS(2, K)), SK, K))
#define STORE_4(i, sym, READ, SK, K) \
  SetUi32(e + (i) * 4, JOIN(sym, READ(i, R_BITS(4, K)), SK, K))

/* r read byte by byte, without passing rBytes */
#define Read_R_Safe(i, RB)  Read_R(r, rBytes, (i), (RB))

/* separate loops with constant shifts: JOIN() becomes a few instructions */
#define STORE_2_7_0(i, sym, READ)   STORE_2(i, sym, READ, 7, 0)
#define STORE_2_6_1(i, sym, READ)   STORE_2(i, sym, READ, 6, 1)
#define STORE_2_5_2(i, sym, READ)   STORE_2(i, sym, READ, 5, 2)
#define STORE_2_8_0(i, sym, READ)   STORE_2(i, sym, READ, 8, 0)
#define STORE_2_7_1(i, sym, READ)   STORE_2(i, sym, READ, 7, 1)
#define STORE_2_6_2(i, sym, READ)   STORE_2(i, sym, READ, 6, 2)
#define STORE_4_23_0(i, sym, READ)  STORE_4(i, sym, READ, 23, 0)
#define STORE_4_22_1(i, sym, READ)  STORE_4(i, sym, READ, 22, 1)
#define STORE_4_21_2(i, sym, READ)  STORE_4(i, sym, READ, 21, 2)


/* One function per (element size, SK, k): constant shifts and widths, and small
   functions that the compiler keeps in registers (GCC allocated them worse when all the
   9 loops were in one function). */
#define DEFINE_DECODE_LOOP(W, SK, K) \
static Z7_NO_INLINE SRes DecodeLoop_##W##_##SK##_##K(CDecState *st) \
{ \
  const UInt16 * const table = st->table; \
  const Byte * const r = st->r; \
  const size_t rBytes = st->rBytes, n = st->n, nSafe = st->nSafe; \
  Byte * const e = st->e; \
  const Byte **hp = st->hp; \
  const Byte * const *hEnd = st->hEnd; \
  UInt64 *bits = st->bits; \
  unsigned *cnt = st->cnt; \
  size_t *pad = st->pad; \
  size_t i = 0; \
  unsigned j; \
  DECODE_MAIN(STORE4_##W, SK, K) \
  DECODE_TAIL(STORE_##W##_##SK##_##K) \
  return SZ_OK; \
}

DEFINE_DECODE_LOOP(2, 7, 0)    /* BF16 */
DEFINE_DECODE_LOOP(2, 6, 1)
DEFINE_DECODE_LOOP(2, 5, 2)
DEFINE_DECODE_LOOP(2, 8, 0)    /* FP16 */
DEFINE_DECODE_LOOP(2, 7, 1)
DEFINE_DECODE_LOOP(2, 6, 2)
DEFINE_DECODE_LOOP(4, 23, 0)   /* FP32 */
DEFINE_DECODE_LOOP(4, 22, 1)
DEFINE_DECODE_LOOP(4, 21, 2)


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
    st.table = table;
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

    switch (hd.type * 3 + hd.k)
    {
      case 0: res = DecodeLoop_2_7_0(&st); break;    /* BF16 */
      case 1: res = DecodeLoop_2_6_1(&st); break;
      case 2: res = DecodeLoop_2_5_2(&st); break;
      case 3: res = DecodeLoop_2_8_0(&st); break;    /* FP16 */
      case 4: res = DecodeLoop_2_7_1(&st); break;
      case 5: res = DecodeLoop_2_6_2(&st); break;
      case 6: res = DecodeLoop_4_23_0(&st); break;   /* FP32 */
      case 7: res = DecodeLoop_4_22_1(&st); break;
      default: res = DecodeLoop_4_21_2(&st); break;
    }
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
