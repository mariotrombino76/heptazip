// 7zExtractMt.h
// 7zip-modern: building blocks for parallel extraction of 7z folders.
// See docs/design/0001-estrazione-parallela-folder-7z.md

#ifndef ZIP7_INC_7Z_EXTRACT_MT_H
#define ZIP7_INC_7Z_EXTRACT_MT_H

#include "../../../Common/MyCom.h"
#include "../../../Common/MyString.h"
#include "../../../Common/MyVector.h"

#ifndef Z7_ST
#include "../../Common/MtTaskPool.h"
#endif

#include "../../IStream.h"

#ifndef Z7_NO_CRYPTO
#include "../../IPassword.h"
#endif

#include "7zItem.h"

namespace NArchive {
namespace N7z {

// ---------------------------------------------------------------------------
// Memory estimation
// ---------------------------------------------------------------------------

/* Estimates the memory used by the decoder of one coder (dictionary, model,
   internal buffers), when the decoder runs single-threaded.
   Returns false for unknown methods or malformed properties: the caller must
   not decode such folder in parallel (it's processed by the sequential path). */
bool EstimateCoderDecodeMemory(CMethodId methodId, const Byte *props, size_t propsSize, UInt64 &memUsage);

/* Sum of EstimateCoderDecodeMemory() for all coders of the folder. */
bool EstimateFolderDecodeMemory(const CFolder &folder, UInt64 &memUsage);


// ---------------------------------------------------------------------------
// Memory budget for admission of parallel jobs.
// Used only by the extraction thread: no synchronization.
// ---------------------------------------------------------------------------

class CMemoryBudget
{
  UInt64 _limit;
  UInt64 _used;
public:
  /* A job is "large" if its cost is more than (limit / kLargeJobDivisor).
     Large jobs are not decoded ahead into memory. */
  static const unsigned kLargeJobDivisor = 4;

  /* A folder that unpacks more than this is decoded inline (with all threads),
     and not by one worker thread: the decoder of a big folder can be faster
     with its own multithreading (LZMA2 streams with independent blocks),
     that can't be known from coder properties. Measured: a 202 MiB LZMA2
     folder with 16 MiB blocks was 2-3 times slower in one worker thread.
     A folder below this limit takes at most ~0.5 s in one thread, while other
     workers decode other folders. */
  static const UInt64 kMaxParallelFolderUnpackSize = (UInt64)64 << 20;

  CMemoryBudget(): _limit(0), _used(0) {}
  void Init(UInt64 limit) { _limit = limit; _used = 0; }

  UInt64 GetLimit() const { return _limit; }
  UInt64 GetUsed() const { return _used; }

  bool IsLargeJob(UInt64 cost) const { return cost > _limit / kLargeJobDivisor; }
  static bool IsBigFolder(UInt64 unpackSize) { return unpackSize > kMaxParallelFolderUnpackSize; }
  // (_used > _limit) is possible: Reserve() doesn't check the limit (the
  // compression reserves the real size of a folder that grew while it was read)
  bool CanReserve(UInt64 cost) const { return _used <= _limit && cost <= _limit - _used; }
  void Reserve(UInt64 cost) { _used += cost; }
  void Release(UInt64 cost) { _used -= cost; }
};


// ---------------------------------------------------------------------------
// Output stream that stores decoded folder data in a memory buffer.
//
// When the buffer is full, Write() stores the bytes that fit and returns
// k_My_HRESULT_WritingWasCut: the same result and (*processedSize) that
// CFolderOutStream::Write() returns when data exceeds the requested files.
// So the decoder and the mixer behave exactly as in sequential extraction.
// ---------------------------------------------------------------------------

Z7_CLASS_IMP_COM_1(
  CFolderBufOutStream
  , ISequentialOutStream
)
  Byte *_buf;
  size_t _size;
  size_t _pos;
public:
  bool WritingWasCut;

  CFolderBufOutStream(): _buf(NULL), _size(0), _pos(0), WritingWasCut(false) {}
  void Init(Byte *buf, size_t size)
  {
    _buf = buf;
    _size = size;
    _pos = 0;
    WritingWasCut = false;
  }
  size_t GetPos() const { return _pos; }
};


// ---------------------------------------------------------------------------
// Input stream over packed data that was read ahead into memory.
//
// The data is exposed at its original position (base) in the archive stream,
// so CDecoder::Decode() can be called with the usual (startPos) and
// PackPositions[], exactly as for the real archive stream.
// If reading of the archive stream was stopped by an error (or by the end of
// stream), Read() at the end of the data returns (endRes): the decoder sees
// the same data and the same result at the same position as in sequential
// extraction.
// ---------------------------------------------------------------------------

Z7_CLASS_IMP_IInStream(
  CBufInStreamAt
)
  const Byte *_data;
  size_t _size;
  UInt64 _base;
  UInt64 _pos;     // absolute position in the archive stream
  HRESULT _endRes;
public:
  CBufInStreamAt(): _data(NULL), _size(0), _base(0), _pos(0), _endRes(S_OK) {}
  void Init(const Byte *data, size_t size, UInt64 base, HRESULT endRes)
  {
    _data = data;
    _size = size;
    _base = base;
    _pos = base;
    _endRes = endRes;
  }
};


#ifndef Z7_NO_CRYPTO

// ---------------------------------------------------------------------------
// Password provider for worker threads.
//
// The extraction thread calls Fetch() once, before the first encrypted folder
// is decoded: so the user interface is asked from the extraction thread only.
// After that the object is read-only and can be used from any thread.
// ---------------------------------------------------------------------------

Z7_CLASS_IMP_COM_1(
  CCachedPassword
  , ICryptoGetTextPassword
)
  bool _fetched;
  HRESULT _res;
  UString_Wipe _password;
public:
  CCachedPassword(): _fetched(false), _res(S_OK) {}

  bool WasFetched() const { return _fetched; }

  /* Asks the password from (callback) and stores the result.
     Returns the result of callback->CryptoGetTextPassword(). */
  HRESULT Fetch(ICryptoGetTextPassword *callback);
};

#endif

}}

#endif
