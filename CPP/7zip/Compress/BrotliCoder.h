// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// BrotliCoder.h
// Heptazip: brotli for the 7z format (design 0014), with the library of
// third_party/brotli.
//
// The format of 7-Zip-zstd (method 0x4F71102 "BROTLI"), so archives are
// compatible in both directions:
// - coder properties: 3 bytes: major, minor version of brotli, level (informative);
// - the stream is a sequence of frames, each one a skippable frame header of 16
//   bytes (LE32 0x184D2A50, LE32 8, LE32 compressed size, LE16 0x5242 "BR", LE16
//   number of 64 KiB blocks of the uncompressed frame) followed by an independent
//   brotli stream of up to (level * 1 MiB) bytes (1 MiB for level 0).
// The frames are independent: they are compressed and decompressed in parallel; the
// size of the frames depends only on the level, so the output is the same for any
// number of threads. The decoder also accepts a single brotli stream without frames.

#ifndef ZIP7_INC_BROTLI_CODER_H
#define ZIP7_INC_BROTLI_CODER_H

#include "../../Common/MyCom.h"

#include "../ICoder.h"

namespace NCompress {
namespace NBrotli {

const unsigned kPropsSize = 3;
const unsigned kLevelMax = 11;
const unsigned kLevelDefault = 5;

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

  unsigned _level;
  UInt32 _numThreads;

public:
  CEncoder();
};

class CDecoder Z7_final:
  public ICompressCoder,
  public ICompressSetDecoderProperties2,
  #ifndef Z7_ST
  public ICompressSetCoderMt,
  #endif
  public CMyUnknownImp
{
  Z7_COM_QI_BEGIN2(ICompressCoder)
  Z7_COM_QI_ENTRY(ICompressSetDecoderProperties2)
  #ifndef Z7_ST
  Z7_COM_QI_ENTRY(ICompressSetCoderMt)
  #endif
  Z7_COM_QI_END
  Z7_COM_ADDREF_RELEASE

  Z7_IFACE_COM7_IMP(ICompressCoder)
  Z7_IFACE_COM7_IMP(ICompressSetDecoderProperties2)
  #ifndef Z7_ST
  Z7_IFACE_COM7_IMP(ICompressSetCoderMt)
  #endif

  UInt32 _numThreads;

public:
  CDecoder();
};

}}

#endif
