/* Lzma2Enc.h -- LZMA2 Encoder
2023-04-13 : Igor Pavlov : Public domain */

#ifndef ZIP7_INC_LZMA2_ENC_H
#define ZIP7_INC_LZMA2_ENC_H

#include "LzmaEnc.h"

EXTERN_C_BEGIN

#define LZMA2_ENC_PROPS_BLOCK_SIZE_AUTO   0
#define LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID  ((UInt64)(Int64)-1)

/* Heptazip: size of the independent LZMA2 blocks for LZMA2_ENC_PROPS_BLOCK_SIZE_AUTO
   (with more than one block thread): LZMA2_ENC_AUTO_BLOCK_DICT_MULT x dictionary,
   from 1 to 256 MiB, aligned to 1 MiB.
   Upstream used 4 x dictionary. Each block starts with an empty dictionary: smaller
   blocks give more parallelism (compression and decompression of big data) for a
   little ratio. Since 2026-11-03 the block is the dictionary itself (before: 2 x,
   from 2026-11-01), together with fb = 64 at level 5 (LzmaEncProps_Normalize): on
   Silesia, -mx5 (32 MiB dictionary on 64-bit), 32 MiB blocks instead of 64 MiB and
   fb 64 give 23.19% instead of 23.42%, decompression with 20 threads 1.6 times and
   compression 1.4 times faster (docs/benchmark-mercato.md); with a 64 MiB
   dictionary the block of one dictionary costs +0.015%. With -mx9 (256 MiB) the
   block stays 256 MiB, so long-distance matches, the reason for a big dictionary,
   are kept.
   The only definition of the rule: Lzma2EncProps_Normalize() (also for xz) and
   CMethodProps::Get_Xz_BlockSize() (7z handler, memory estimates) use it. */
#define LZMA2_ENC_AUTO_BLOCK_DICT_MULT  1

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

/* Heptazip (issue #75): dictionary of the block encoders when the dictionary was not
   given (dictSize == 0), the blocks are automatic (LZMA2_ENC_PROPS_BLOCK_SIZE_AUTO,
   the block is Lzma2Enc_GetAutoBlockSize() of the dictionary of the level) and the
   data spans more than one block. Inside a 32 MiB block the history available to a
   match is on average 16 MiB, so at level 5 the dictionary is half the block:
   16 MiB with 32 MiB blocks. Measured at 20 threads (docs/benchmark-mercato.md):
   Silesia 23.19% -> 23.22%, corpus unchanged (63.93%), same compression and
   decompression speed, compression memory -41% on Silesia (2329 -> 1367 MB, each bt4
   block encoder costs 4 B x 3 x dictionary of tables) and -32% on the corpus;
   decompression memory also halves per thread. The other levels keep the dictionary of
   the level: -mx9 (256 MiB dictionary and block) still produces archives identical to
   7-Zip's; plain LZMA, solid LZMA2 (one block thread, data smaller than a block) and an
   explicit dictionary are not affected.
   The only definition of the rule: Lzma2EncProps_Normalize() (7z), XzProps_Normalize()
   (xz), CMethodProps::Get_Lzma2_BlockDicSize() (memory estimates of the 7z and xz
   handlers) and the Add dialog (CompressDialog.cpp) use it. */
#define LZMA2_ENC_AUTO_BLOCK_HALF_DICT_LEVEL  5

static Z7_FORCE_INLINE UInt32 Lzma2Enc_GetAutoBlockDictSize(unsigned level, UInt32 dictSizeOfLevel)
{
  return level == LZMA2_ENC_AUTO_BLOCK_HALF_DICT_LEVEL ? dictSizeOfLevel >> 1 : dictSizeOfLevel;
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
