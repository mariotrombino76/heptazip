// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// BscCoder.h
// Heptazip: the "BWT" method of the 7z format (design 0016, issue #81): block-sorting
// compression with libbsc (third_party/libbsc: LZP + Burrows-Wheeler transform with
// libsais + QLFC entropy coder). A method of Heptazip only, like FLOAT: archives that
// use it open only with Heptazip.
//
// Format:
// - method ID 0x3F707F5E728B0002, name "BWT" (-m0=bwt);
// - coder properties: 2 bytes: format version (1) and level (informative);
// - the stream is a sequence of independent libbsc blocks, each one the libbsc block
//   header of 28 bytes (compressed and uncompressed sizes, checksums, parameters) followed
//   by the compressed data: the decoder reads the header, learns the sizes, reads the rest.
// The blocks are independent: they are compressed and decompressed in parallel; the
// block size depends only on the level (or the "c" property), so the output is the same
// for any number of threads.
//
// Properties of the encoder: x (level 1..9: block size and coder), c (block size in
// bytes, 64 KiB .. 1 GiB), a (coder: 0 QLFC fast, 1 QLFC static, 2 QLFC adaptive),
// mt (threads). Memory: about 7 x block size per thread (5 x in libbsc, input and output
// buffers), limited to half of the RAM.

#ifndef ZIP7_INC_BSC_CODER_H
#define ZIP7_INC_BSC_CODER_H

#include "../../Common/MyCom.h"

#include "../ICoder.h"

namespace NCompress {
namespace NBwt {

const unsigned kPropsSize = 2;
const Byte kFormatVersion = 1;
const unsigned kLevelMax = 9;
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
  UInt64 _blockSize;   // 0: from the level
  int _coder;          // -1: from the level
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
