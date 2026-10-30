// ZstdLibEncoder.h
// 7zip-modern: Zstandard encoder based on the reference library (third_party/zstd).
//
// It creates standard zstd frames for the 7z method 0x4F71101, the method ID
// and coder properties used by 7-Zip-zstd (5 bytes: major, minor version of
// the library, level, 2 reserved bytes), so archives are compatible with it.
// Decoding uses the 7-Zip decoder (C/ZstdDec.c), see ZstdCodecRegister.cpp.
//
// Names (NZstdLib, ZstdLib*) are distinct from the NCompress::NZstd namespace of
// 7-Zip, so a future upstream zstd encoder can coexist with this one.

#ifndef ZIP7_INC_ZSTD_LIB_ENCODER_H
#define ZIP7_INC_ZSTD_LIB_ENCODER_H

#include "../../Common/MyCom.h"

#include "../ICoder.h"

namespace NCompress {
namespace NZstdLib {

const unsigned kPropsSize = 5;

class CEncoder Z7_final:
  public ICompressCoder,
  public ICompressSetCoderProperties,
  public ICompressWriteCoderProperties,
  #ifndef Z7_ST
  public ICompressSetCoderMt,
  #endif
  public CMyUnknownImp
{
  Z7_COM_QI_BEGIN2(ICompressCoder)
  Z7_COM_QI_ENTRY(ICompressSetCoderProperties)
  Z7_COM_QI_ENTRY(ICompressWriteCoderProperties)
  #ifndef Z7_ST
  Z7_COM_QI_ENTRY(ICompressSetCoderMt)
  #endif
  Z7_COM_QI_END
  Z7_COM_ADDREF_RELEASE

  Z7_IFACE_COM7_IMP(ICompressCoder)
  Z7_IFACE_COM7_IMP(ICompressSetCoderProperties)
  Z7_IFACE_COM7_IMP(ICompressWriteCoderProperties)
  #ifndef Z7_ST
  Z7_IFACE_COM7_IMP(ICompressSetCoderMt)
  #endif

  int _level;            // zstd compression level (1..ZSTD_maxCLevel())
  unsigned _windowLog;   // 0: default of the level
  UInt32 _numThreads;    // zstd worker threads (1: single-threaded)

public:
  CEncoder();
};

}}

#endif
