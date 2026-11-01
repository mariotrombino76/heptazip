/* Lzma2Enc.h -- LZMA2 Encoder
2023-04-13 : Igor Pavlov : Public domain */

#ifndef ZIP7_INC_LZMA2_ENC_H
#define ZIP7_INC_LZMA2_ENC_H

#include "LzmaEnc.h"

EXTERN_C_BEGIN

#define LZMA2_ENC_PROPS_BLOCK_SIZE_AUTO   0
#define LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID  ((UInt64)(Int64)-1)

/* 7zip-modern: size of the independent LZMA2 blocks for LZMA2_ENC_PROPS_BLOCK_SIZE_AUTO
   (with more than one block thread): LZMA2_ENC_AUTO_BLOCK_DICT_MULT x dictionary,
   from 1 to 256 MiB, not smaller than the dictionary, aligned to 1 MiB.
   Upstream used 4 x dictionary. Each block starts with an empty dictionary: smaller
   blocks give more parallelism (compression and decompression of big data) for a
   little ratio. Silesia, -mx5 (32 MiB dictionary on 64-bit): 64 MiB blocks instead
   of 128 MiB, +0.06% size, decompression with 20 threads 1.77 times faster,
   compression 1.18 times (docs/benchmark-mercato.md). The block is never smaller
   than the dictionary: with -mx9 (256 MiB) it stays 256 MiB, so long-distance
   matches, the reason for a big dictionary, are kept.
   The only definition of the rule: Lzma2EncProps_Normalize() (also for xz) and
   CMethodProps::Get_Xz_BlockSize() (7z handler, memory estimates) use it. */
#define LZMA2_ENC_AUTO_BLOCK_DICT_MULT  2

static Z7_FORCE_INLINE UInt64 Lzma2Enc_GetAutoBlockSize(UInt64 dictSize)
{
  const UInt32 kMinSize = (UInt32)1 << 20;
  const UInt32 kMaxSize = (UInt32)1 << 28;
  UInt64 blockSize = dictSize * LZMA2_ENC_AUTO_BLOCK_DICT_MULT;
  if (blockSize < kMinSize) blockSize = kMinSize;
  if (blockSize > kMaxSize) blockSize = kMaxSize;
  if (blockSize < dictSize) blockSize = dictSize;
  blockSize += (kMinSize - 1);
  blockSize &= ~(UInt64)(kMinSize - 1);
  return blockSize;
}

typedef struct
{
  CLzmaEncProps lzmaProps;
  UInt64 blockSize;
  int numBlockThreads_Reduced;
  int numBlockThreads_Max;
  int numTotalThreads;
  unsigned numThreadGroups; // 0 : no groups
} CLzma2EncProps;

void Lzma2EncProps_Init(CLzma2EncProps *p);
void Lzma2EncProps_Normalize(CLzma2EncProps *p);

/* ---------- CLzmaEnc2Handle Interface ---------- */

/* Lzma2Enc_* functions can return the following exit codes:
SRes:
  SZ_OK           - OK
  SZ_ERROR_MEM    - Memory allocation error
  SZ_ERROR_PARAM  - Incorrect paramater in props
  SZ_ERROR_WRITE  - ISeqOutStream write callback error
  SZ_ERROR_OUTPUT_EOF - output buffer overflow - version with (Byte *) output
  SZ_ERROR_PROGRESS - some break from progress callback
  SZ_ERROR_THREAD - error in multithreading functions (only for Mt version)
*/

typedef struct CLzma2Enc CLzma2Enc;
typedef CLzma2Enc * CLzma2EncHandle;
// Z7_DECLARE_HANDLE(CLzma2EncHandle)

CLzma2EncHandle Lzma2Enc_Create(ISzAllocPtr alloc, ISzAllocPtr allocBig);
void Lzma2Enc_Destroy(CLzma2EncHandle p);
SRes Lzma2Enc_SetProps(CLzma2EncHandle p, const CLzma2EncProps *props);
void Lzma2Enc_SetDataSize(CLzma2EncHandle p, UInt64 expectedDataSiize);
Byte Lzma2Enc_WriteProperties(CLzma2EncHandle p);
SRes Lzma2Enc_Encode2(CLzma2EncHandle p,
    ISeqOutStreamPtr outStream,
    Byte *outBuf, size_t *outBufSize,
    ISeqInStreamPtr inStream,
    const Byte *inData, size_t inDataSize,
    ICompressProgressPtr progress);

EXTERN_C_END

#endif
