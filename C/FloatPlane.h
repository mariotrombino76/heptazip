/* FloatPlane.h -- lossless coder for arrays of floating-point numbers (AI model weights)
7zip-modern (design 0008) : Public domain

A block of bytes is read as little-endian elements of a float type (BF16, FP16 or FP32,
at a byte phase chosen by the encoder). Each element x is split into
  e = (x >> s) & 0xFF                                (exponent byte, Huffman coded)
  r = (x & ((1 << s) - 1)) | ((x >> (s + 8)) << s)   (sign and mantissa, stored raw)
with s = 7 (BF16), 8 (FP16: e is the high byte) or 23 (FP32). In model weights the
exponent byte has 2.5-3 bits of entropy and the mantissa is close to random: the block
shrinks to about 66% for BF16, at the speed of a Huffman coder.

Model files also contain data that are not floats (masks, padding, text metadata):
the encoder can code such a block as one zstd frame (mode 2), if the caller gives it a
zstd compressor (CFloatPlaneLz), and a sample shows that zstd wins.

Block format (all integers little-endian):
  u8  mode                   0 = stored, 1 = split, 2 = zstd
  u32 outLen                 decoded size of the block, 1 .. FLOAT_PLANE_BLOCK_MAX
  mode 0:  outLen raw bytes
  mode 2:
    u32 lzSize               size of the frame
    lzSize bytes             one zstd frame (RFC 8878) that decodes to exactly outLen bytes
  mode 1:
    u8  type                 0 = BF16, 1 = FP16, 2 = FP32 (element size w = 2, 2, 4)
    u8  phase                0 .. w-1: raw bytes before the first element
    n = (outLen - phase) / w elements, tail = outLen - phase - n * w raw bytes
    phase raw bytes, tail raw bytes
    128 bytes                Huffman code lengths of e (0..12), symbol 2k in the low nibble
    u32 huffSize[4]          sizes of the 4 Huffman bit streams in bytes
    4 streams                stream j: the codes of the elements k with k % 4 == j,
                             canonical, MSB first, zero padding to a byte
    n * (w - 1) bytes        r, little-endian, w - 1 bytes per element
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
#define FLOAT_PLANE_HEADER_MAX     (1 + 4 + 1 + 1 + 3 + 3 + 128 + 4 * 4)

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

EXTERN_C_END

#endif
