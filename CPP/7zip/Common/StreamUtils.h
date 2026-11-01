// StreamUtils.h

#ifndef ZIP7_INC_STREAM_UTILS_H
#define ZIP7_INC_STREAM_UTILS_H

#include "../../Common/MyBuffer.h"

#include "../IStream.h"

inline HRESULT InStream_SeekSet(IInStream *stream, UInt64 offset) throw()
  {  return stream->Seek((Int64)offset, STREAM_SEEK_SET, NULL); }
inline HRESULT InStream_GetPos(IInStream *stream, UInt64 &curPosRes) throw()
  {  return stream->Seek(0, STREAM_SEEK_CUR, &curPosRes); }
inline HRESULT InStream_GetSize_SeekToEnd(IInStream *stream, UInt64 &sizeRes) throw()
  { return stream->Seek(0, STREAM_SEEK_END, &sizeRes); }

HRESULT InStream_SeekToBegin(IInStream *stream) throw();
HRESULT InStream_AtBegin_GetSize(IInStream *stream, UInt64 &size) throw();
HRESULT InStream_GetPos_GetSize(IInStream *stream, UInt64 &curPosRes, UInt64 &sizeRes) throw();

inline HRESULT InStream_GetSize_SeekToBegin(IInStream *stream, UInt64 &sizeRes) throw()
{
  RINOK(InStream_SeekToBegin(stream))
  return InStream_AtBegin_GetSize(stream, sizeRes);
}


HRESULT ReadStream(ISequentialInStream *stream, void *data, size_t *size) throw();
HRESULT ReadStream_FALSE(ISequentialInStream *stream, void *data, size_t size) throw();
HRESULT ReadStream_FAIL(ISequentialInStream *stream, void *data, size_t size) throw();
HRESULT WriteStream(ISequentialOutStream *stream, const void *data, size_t size) throw();

/* Tensor7: reads (size) bytes into (buf) when (size) comes from the archive and was
   not checked against the real data. The buffer grows (1 MiB, then doubling) only while
   the data are actually there, so a corrupted or truncated file of a few bytes cannot
   make the handler allocate gigabytes first (found by fuzzing). The extra copies cost
   less than one pass over the data.
   (read)(void *data, size_t *processedSize) reads up to *processedSize bytes; fewer
   bytes mean the end of the data. Returns S_FALSE if the data end before (size) bytes;
   then (buf) holds what was read, up to its current size. */
template <class F>
HRESULT ReadToBuffer_Growing(CByteBuffer &buf, size_t size, F read)
{
  const size_t kStartSize = (size_t)1 << 20;
  buf.Free();
  size_t done = 0;
  while (done != size)
  {
    size_t next = done < kStartSize ? kStartSize : done * 2;
    if (next < done || next > size)
      next = size;
    buf.ChangeSize_KeepData(next, done);
    const size_t req = next - done;
    size_t cur = req;
    RINOK(read((Byte *)buf + done, &cur))
    done += cur;
    if (cur != req)
      return S_FALSE;
  }
  return S_OK;
}

#endif
