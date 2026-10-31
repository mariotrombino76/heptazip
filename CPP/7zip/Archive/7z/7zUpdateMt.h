// 7zUpdateMt.h
// 7zip-modern: building blocks for the parallel compression of 7z folders.
// See docs/design/0003-compressione-parallela-folder-7z.md

#ifndef ZIP7_INC_7Z_UPDATE_MT_H
#define ZIP7_INC_7Z_UPDATE_MT_H

#include "../../../Common/MyCom.h"
#include "../../../Common/MyVector.h"

#include "../../IStream.h"

#include "7zCompressionMode.h"

namespace NArchive {
namespace N7z {

// ---------------------------------------------------------------------------
// Cost of the compression of one folder (design 0003, §5.6 and §5.7)
// ---------------------------------------------------------------------------

/* A folder with more data than this is compressed inline (by the encoder with
   its own threads), and not by a worker thread: same rule and same reasons
   as CMemoryBudget::kMaxParallelFolderUnpackSize for the extraction. */
const UInt64 kMaxParallelEncodeFolderSize = (UInt64)64 << 20;

/* The read ahead of a folder stops at this size: files that grew after the
   folder was planned. The rest of the folder is then compressed inline. */
const UInt64 kMaxParallelEncodeReadSize = kMaxParallelEncodeFolderSize * 2;

struct CFolderEncodeCost
{
  UInt32 NumThreads;     // threads that the coders use for this folder (>= 1)
  UInt64 EncoderMemory;  // memory of the coders, without the input/output buffers
};

/* Estimates the threads and the memory used by the coders of (mode) to compress
   a folder of (folderSize) bytes, when the coder properties are set with
   (reduceSize) (the kReduceSize of the whole operation, see CEncoder).
   The coders keep all their properties, also the number of threads: the
   estimate models how many threads they really use for such a folder.
   Returns false if the parallel compression doesn't support (mode): unknown
   method, BCJ2 (it reads the sizes of the files, see §3.5), Copy (nothing to
   gain). Such folders are compressed inline. */
bool EstimateFolderEncodeCost(const CCompressionMethodMode &mode,
    UInt64 reduceSize, UInt64 folderSize, CFolderEncodeCost &cost);

/* Capacity reserved for the compressed data of a folder of (folderSize) bytes:
   the coders supported by EstimateFolderEncodeCost() expand incompressible
   data by much less than this margin. The buffer grows if needed. */
inline UInt64 EstimateFolderEncodeOutSize(UInt64 folderSize)
{
  return folderSize + (folderSize >> 4) + ((UInt32)1 << 16);
}


// ---------------------------------------------------------------------------
// CFolderReplayInStream
// ---------------------------------------------------------------------------

/* Reads the data of a folder that was read ahead into memory by
   CFolderInStream, with the same granularity as CFolderInStream::Read():
   at most kMaxReadSize bytes per call, and never beyond the end of a file
   (fileSizes: sizes of the files in the folder, also the ones that weren't
   opened, with size 0).
   (data) can be longer than the sum of (fileSizes): the rest is the beginning
   of a file that is still being read; then reading continues from (rest),
   the CFolderInStream itself (read ahead stopped at kMaxParallelEncodeReadSize). */
Z7_CLASS_IMP_COM_1(
  CFolderReplayInStream
  , ISequentialInStream
)
  const Byte *_data;
  size_t _size;
  size_t _pos;
  const UInt64 *_fileSizes;
  unsigned _numFiles;
  unsigned _fileIndex;    // next file
  UInt64 _fileRem;        // bytes of the current file that were not read
  CMyComPtr<ISequentialInStream> _rest;
public:
  static const UInt32 kMaxReadSize = (UInt32)1 << 20; // as CFolderInStream::Read()

  CFolderReplayInStream(): _data(NULL), _size(0), _pos(0),
      _fileSizes(NULL), _numFiles(0), _fileIndex(0), _fileRem(0) {}
  void Init(const Byte *data, size_t size,
      const UInt64 *fileSizes, unsigned numFiles,
      ISequentialInStream *rest)
  {
    _data = data;
    _size = size;
    _pos = 0;
    _fileSizes = fileSizes;
    _numFiles = numFiles;
    _fileIndex = 0;
    _fileRem = 0;
    _rest = rest;
  }
};

}}

#endif
