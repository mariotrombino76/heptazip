/* SPDX-License-Identifier: BSD-2-Clause
   Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip) */
/* FloatPlane.h -- lossless coder for arrays of floating-point numbers (AI model weights)
Heptazip (design 0008)

A block of bytes is read as little-endian elements of a float type (BF16, FP16 or FP32,
at a byte phase chosen by the encoder). With s = 7 (BF16), 8 (FP16: the high byte) or
23 (FP32), the position of the exponent byte, and k = 0..2 mantissa bits chosen by the
encoder, SK = s - k, each element x is split into
  e = (x >> SK) & ((256 << k) - 1)                         (exponent and k mantissa bits:
                                                            Huffman coded, 8 + k bits)
  r = (x & ((1 << SK) - 1)) | ((x >> (SK + 8 + k)) << SK)  (the other bits, packed)
In model weights the exponent byte has 2.5-3 bits of entropy, the top mantissa bits are
slightly skewed given the exponent, the rest is close to random: a BF16 block shrinks to
about 66%, at the speed of a Huffman coder.

Model files also contain data that are not floats (masks, padding, text metadata):
the encoder can code such a block as one zstd frame (mode 2), if the caller gives it a
zstd compressor (CFloatPlaneLz), and a sample shows that zstd wins.

Quantized models (GGUF of llama.cpp) store the weights in structs of 32 or 256 weights:
fp16 scales, packed scales of sub-blocks, quantized values of 4-8 bits. Mode 3 codes
each kind of field of the structs (e.g. all the high bytes of the scales, all the
quantized values) as its own Huffman sequence: 2-3 points better than LZMA2/zstd, at
the speed of a Huffman coder. The structs (types of ggml-common.h):
  qtype  struct  bytes  fields (symbols per struct: alphabet)
  0      Q4_0    18     d low byte (1: 256), d high byte (1: 256), qs[16] (16: 256)
  1      Q8_0    34     d low byte (1), d high byte (1), qs[32] (32)
  2      Q4_K    144    low bytes of d, dmin (2), high bytes of d, dmin (2),
                        scales[12] (12), qs[128] (128)
  3      Q5_K    176    low bytes of d, dmin (2), high bytes (2), scales[12] (12),
                        the 256 values of 5 bits from qs[128] and qh[32] (256: 32)
  4      Q6_K    210    the 256 values of 6 bits from ql[128] and qh[64] (256: 64),
                        scales[16] (16), d low byte (1), d high byte (1)
  5      Q2_K    84     low bytes of d, dmin (2), high bytes (2), scales[16] (16),
                        qs[64] (64)
  6      Q3_K    110    d low byte (1), d high byte (1), scales[12] (12), hmask[32] and
                        qs[64] (96)
  Value v of Q5_K: c = v / 64, h = (v / 32) % 2, l = v % 32; low 4 bits in qs[32c + l] at
  bit 4h, bit 4 in qh[l] at bit 2c + h. Value v of Q6_K: h = v / 128, c = (v / 32) % 4,
  l = v % 32; low 4 bits in ql[64h + 32(c % 2) + l] at bit 4(c / 2), bits 4-5 in
  qh[32h + l] at bit 2c (as dequantize_row_q5_K / q6_K of ggml). Value v = 128n + 32c + l
  (l < 32) of Q2_K / Q3_K: 2 low bits at bit 2c of qs[32n + l]; in Q3_K the third bit at
  bit 4n + c of hmask[l]; the sub-block (scale) of v is v / 16.
Mode 4 codes the structs with a context model: rANS with one
frequency table per context, where the context of a quantized value is known from the
fields decoded before it: in Q4_K / Q5_K the zero point of its sub-block (the quantized
value of the weight 0: the values cluster around it), in Q6_K the scale of its sub-block.
in Q4_0 / Q8_0 the exponent of d.  Q2_K / Q3_K: about 3 points smaller than LZMA2. About 1.7 points smaller than mode 3 on Q4_K / Q5_K
models, 0.7 on Q6_K, 0.3 on Q4_0, 0.1 on Q8_0 (C/FloatQuant.c).

Block format (all integers little-endian):
  u8  mode                   0 = stored, 1 = split, 2 = zstd, 3 = quantized,
                             4 = quantized with contexts
  u32 outLen                 decoded size of the block, 1 .. FLOAT_PLANE_BLOCK_MAX
  mode 0:  outLen raw bytes
  mode 2:
    u32 lzSize               size of the frame
    lzSize bytes             one zstd frame (RFC 8878) that decodes to exactly outLen bytes
  mode 1:
    u8  type                 0 = BF16, 1 = FP16, 2 = FP32 (element size w = 2, 2, 4)
    u8  phase                0 .. w-1: raw bytes before the first element
    u8  k                    0 .. 2: mantissa bits in the Huffman symbol
    n = (outLen - phase) / w elements, tail = outLen - phase - n * w raw bytes
    phase raw bytes, tail raw bytes
    (256 << k) / 2 bytes     Huffman code lengths of e (0..12), symbol 2i in the low nibble
    u32 huffSize[4]          sizes of the 4 Huffman bit streams in bytes
    4 streams                stream j: the codes of the elements k with k % 4 == j,
                             canonical, MSB first, zero padding to a byte
    ceil(n * rb / 8) bytes   r, rb = 8 * w - 8 - k bits per element, packed
                             little-endian (element i at bit i * rb)
  mode 3:
    u8  qtype                0 .. 4 (above), struct size S
    u8  phase                0 .. S-1: raw bytes before the first struct
    n = (outLen - phase) / S structs (n >= 1), tail = outLen - phase - n * S raw bytes
    phase raw bytes, tail raw bytes
    for each field: numSyms / 2 bytes of code lengths (as mode 1), u32 huffSize[4]
    for each field: 4 streams, as mode 1: symbol i of the field (struct i / w, symbol
                             i % w of the struct) in stream i % 4
  mode 4:
    u8  qtype                0 .. 6 (as mode 3), struct size S
    u8  phase                as mode 3; n structs (n >= 1), tail
    phase raw bytes, tail raw bytes
    for each field: u32 tabSize, u32 streamSize
    for each field: tabSize bytes of frequency tables, then streamSize bytes of rANS
  The fields, in coding order (symbols per struct, alphabet, precision P, contexts):
    Q4_K, Q5_K: low bytes of d, dmin (2, 256, 12, ctx = which of the two); high bytes of
      d, dmin (2, 256, 12, which); the scales sc_j of scales[12] (8, 64, 10, 1); the mins
      m_j (8, 64, 10, sc_j >> 2); the values (256: Q4_K 16, P 9; Q5_K 32, P 9), value v
      in the sub-block j = v / 32 with context 4 * b + (sc_j >> 4), b = min(31,
      floor(k * q0)), q0 = dmin * m_j / (d * sc_j), k = 2 (Q4_K) or 1 (Q5_K), b = 31 if
      d * sc_j = 0; q0 from the fp16 bits with exact integer arithmetic (ZeroBucket in
      FloatQuant.c). Value v of Q4_K: low or high nibble (h = (v / 32) % 2) of
      qs[32(v / 64) + v % 32]; of Q5_K as in mode 3. sc_j, m_j: get_scale_min_k4 of ggml.
    Q6_K: scales[16] (16, 256, 12, 1); low byte of d (1, 256, 12, 1); high byte of d
      (1, 256, 12, 1); the values (256, 64, 11, min(15, |scales[v / 16]| >> 3)).
    Q4_0: low byte of d (1, 256, 12, 1); high byte of d (1, 256, 12, 1); the values (32,
      16, 9, (d >> 10) & 31: the exponent of d), value v the low nibble of qs[v] (v < 16)
      or the high nibble of qs[v - 16].
    Q8_0: d as Q4_0; the values qs[32] (32, 256, 12, (d >> 11) & 15).
    Q2_K: low bytes of d, dmin (2, 256, 12, which); high bytes (2, 256, 12, which);
      scales[16] (16, 256, 12, 1); the values (256, 4, 9, 4 * b + (sc >> 2)) with
      sc = scales[v / 16] & 15, m = scales[v / 16] >> 4, b = min(15, floor(4 * q0)) and
      q0 = dmin * m / (d * sc) as for Q4_K (15 if d * sc = 0).
    Q3_K: low byte of d (1, 256, 12, 1); high byte (1, 256, 12, 1); the 16 scales s_j
      (6 bits) of scales[12] (16, 64, 10, 1): s_j = lo | hi << 4, lo = scales[j] & 15
      (j < 8) or scales[j - 8] >> 4, hi = (scales[8 + j % 4] >> (2 * (j / 4))) & 3 (as
      dequantize_row_q3_K); the values (3 bits: 2 low bits | third bit << 2; 256, 8, 9,
      min(15, |s_{v / 16} - 32| >> 1)).
  Frequency tables of a field: per context one bit (1 = used), and if used, for each
  symbol its level L (0 = absent, else a frequency of about 2^((L - 1) / 8)): L minus the
  level of the previous symbol (0 before the first), zigzag (d >= 0: 2d, else -2d - 1),
  plus 1 as Elias gamma (n - 1 zero bits, a 1, the n - 1 low bits; at most 9 bits); bits
  LSB first, zero padding to a byte. L <= 185. The frequencies f (sum 2^P) of a context:
  a = mant[(L - 1) % 8] << ((L - 1) / 8) with mant = 256 279 304 332 362 395 431 470,
  f = max(1, floor(a * 2^P / sum(a))) for the present symbols; then, while the sum is not
  2^P, the first symbol with the largest f takes the whole difference if the sum is
  smaller, else gives back as much as it can keeping f >= 1. A context that is not used
  decodes symbol 0 with f = 2^P.
  rANS stream of a field: N states (u32, in [2^15, 2^31)): N = 64 for the values, 32
  for the other fields with one context, 8 for the rest; then 16-bit words. The symbols of the field, struct by struct, in the
  order above: symbol i is decoded by state x = state[i % N]: slot = x % 2^P, s the symbol
  with cum[s] <= slot < cum[s] + f[s] (cum: the sum of the frequencies of the symbols
  before s), x = f[s] * (x >> P) + slot - cum[s], then if x < 2^15, x = (x << 16) | the
  next word. Decoding rejects a stream that does not end with every state at 2^15 and all
  its words used.
  Canonical codes: shorter codes first, then by symbol value (as in Deflate).
  Decoding rejects lengths > 12, an over-subscribed code, a bit pattern that is not a
  code, and a huffSize[j] different from the bytes the codes of stream j actually use.
  The 4 streams are independent dependency chains: a decoder can run them in parallel
  (instruction-level parallelism), as huff0 in zstd. */

#ifndef ZIP7_INC_FLOAT_PLANE_H
#define ZIP7_INC_FLOAT_PLANE_H

#include "7zTypes.h"

EXTERN_C_BEGIN

#define FLOAT_PLANE_BLOCK_MAX      ((size_t)1 << 22)  /* Huffman_Generate: < 4M symbols */
#define FLOAT_PLANE_BLOCK_DEFAULT  ((size_t)1 << 20)
/* largest header: a quantized block (mode 3) with phase and tail of up to 209 bytes and
   4 fields of 256 symbols */
#define FLOAT_PLANE_HEADER_MAX     (1 + 4 + 2 + 209 + 209 + 4 * (128 + 4 * 4))

/* The encoded block is never larger than this (stored mode). */
#define FLOAT_PLANE_ENCODED_MAX(srcLen)  ((srcLen) + 5)

/* Optional general-purpose coder for the blocks that are not floats (mode 2).
   Compress: compresses src into one zstd frame in dest (destCap bytes); returns its size,
     or 0 if it does not fit or fails.
   Decompress: decodes the zstd frame src[0 .. srcLen) into dest; SZ_OK only if the frame
     is whole, alone, and decodes to exactly destLen bytes; it must not write beyond
     dest + destLen.
   (ctx) is the state of the coder (one per thread). */
typedef struct
{
  void *ctx;
  size_t (*Compress)(void *ctx, const Byte *src, size_t srcLen, Byte *dest, size_t destCap);
  SRes (*Decompress)(void *ctx, const Byte *src, size_t srcLen, Byte *dest, size_t destLen);
} CFloatPlaneLz;

/* Encodes src[0 .. srcLen) (1 <= srcLen <= FLOAT_PLANE_BLOCK_MAX) into dest, which must
   have FLOAT_PLANE_ENCODED_MAX(srcLen) bytes. Returns the encoded size.
   lz: NULL or a coder with Compress (then mode 2 can be chosen). */
size_t FloatPlane_EncodeBlock(const Byte *src, size_t srcLen, Byte *dest, const CFloatPlaneLz *lz);

/* Reads the block header: SZ_OK and *outLen, or SZ_ERROR_DATA if src does not start with
   a valid header (srcLen < 5, unknown mode, outLen == 0 or > FLOAT_PLANE_BLOCK_MAX). */
SRes FloatPlane_GetBlockOutLen(const Byte *src, size_t srcLen, size_t *outLen);

/* Size of the encoded block that starts at src, from its header only: SZ_OK and
   *blockSize, SZ_ERROR_INPUT_EOF if srcLen does not contain the whole header (up to
   FLOAT_PLANE_HEADER_MAX bytes), SZ_ERROR_DATA if the header is not valid. Lets a
   multi-threaded decoder hand whole blocks to its threads. */
SRes FloatPlane_GetBlockSize(const Byte *src, size_t srcLen, size_t *blockSize);

/* Decodes one block from src[0 .. srcLen) into dest (dest has room for the outLen of the
   header). *srcUsed = size of the block. SZ_ERROR_INPUT_EOF if srcLen is shorter than the
   block, SZ_ERROR_DATA for corrupted data, SZ_ERROR_UNSUPPORTED for a mode 2 block without
   lz->Decompress. Never reads src or writes dest out of bounds. */
SRes FloatPlane_DecodeBlock(const Byte *src, size_t srcLen, Byte *dest, size_t destLen, size_t *srcUsed,
    const CFloatPlaneLz *lz);

/* Fast paths of the decoder: on by default (SIMD where the CPU has it, two symbols per
   table lookup). Tests and benchmarks can turn them off to get the reference decoder:
   scalar code, one symbol per lookup (not thread-safe: set it before decoding). */
void FloatPlane_SetSimd(BoolInt enable);

EXTERN_C_END

#endif
