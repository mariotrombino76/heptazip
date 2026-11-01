// FloatCoder.h
// 7zip-modern: "FLOAT" method of 7z, lossless coder for arrays of floating-point
// numbers such as the weights of AI models (design 0008, block format in
// C/FloatPlane.h). The encoder chooses the float type (BF16, FP16, FP32) and the
// alignment for each block, so any data can be compressed; other data are stored.
//
// Coder properties: 1 byte, log2 of the block size (16 .. 22).

#ifndef ZIP7_INC_COMPRESS_FLOAT_CODER_H
#define ZIP7_INC_COMPRESS_FLOAT_CODER_H

#include "../../Common/MyBuffer.h"
#include "../../Common/MyCom.h"

#include "../ICoder.h"

namespace NCompress {
namespace NFloat {

const unsigned kBlockLogMin = 16;
const unsigned kBlockLogMax = 22;
const unsigned kBlockLogDefault = 20;

class CDecoder Z7_final:
  public ICompressCoder,
  public ICompressSetDecoderProperties2,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(ICompressCoder, ICompressSetDecoderProperties2)

  unsigned _blockLog;
  CByteBuffer _inBuf;
  CByteBuffer _outBuf;
public:
  CDecoder(): _blockLog(kBlockLogDefault) {}
};

#ifndef Z7_EXTRACT_ONLY

class CEncoder Z7_final:
  public ICompressCoder,
  public ICompressSetCoderProperties,
  public ICompressWriteCoderProperties,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_3(ICompressCoder, ICompressSetCoderProperties, ICompressWriteCoderProperties)

  unsigned _blockLog;
  CByteBuffer _inBuf;
  CByteBuffer _outBuf;
public:
  CEncoder(): _blockLog(kBlockLogDefault) {}
};

#endif

}}

#endif
