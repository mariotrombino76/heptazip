// BitlEncoder.h -- the Least Significant Bit of byte is First

#ifndef ZIP7_INC_BITL_ENCODER_H
#define ZIP7_INC_BITL_ENCODER_H

#include "../Common/OutBuffer.h"

/* Tensor7: the bits are collected in a 64-bit buffer and written 4 bytes at a time,
   instead of one byte at a time with a loop. Same output; the greedy Deflate -mx1 of
   7-Zip 26.05 was 16% faster with one thread (docs/design/0010). */
class CBitlEncoder
{
  COutBuffer _stream;
  UInt64 _bits;       // the pending bits: the first one is bit 0
  unsigned _numBits;  // < 32 between the calls
public:
  bool Create(UInt32 bufSize) { return _stream.Create(bufSize); }
  void SetStream(ISequentialOutStream *outStream) { _stream.SetStream(outStream); }
  UInt64 GetProcessedSize() const { return _stream.GetProcessedSize() + ((_numBits + 7) >> 3); }
  void Init()
  {
    _stream.Init();
    _bits = 0;
    _numBits = 0;
  }
  HRESULT Flush()
  {
    FlushByte();
    return _stream.Flush();
  }
  // writes the pending bits, the last byte padded with zero bits
  void FlushByte()
  {
    for (; _numBits != 0; _numBits = (_numBits > 8 ? _numBits - 8 : 0))
    {
      _stream.WriteByte((Byte)_bits);
      _bits >>= 8;
    }
    _bits = 0;
  }
  // (numBits <= 32); the bits of (value) above (numBits) are ignored
  Z7_FORCE_INLINE
  void WriteBits(UInt32 value, unsigned numBits)
  {
    _bits |= ((UInt64)value & (((UInt64)1 << numBits) - 1)) << _numBits;
    _numBits += numBits;
    if (_numBits >= 32)
    {
      _stream.WriteUInt32_LE((UInt32)_bits);
      _bits >>= 32;
      _numBits -= 32;
    }
  }
  // the stream must be at a byte boundary (after FlushByte())
  void WriteByte(Byte b) { WriteBits(b, 8); }
  void WriteBytes(const Byte *data, size_t size)
  {
    if (_numBits == 0)
      _stream.WriteBytes(data, size);
    else
      for (size_t i = 0; i < size; i++)
        WriteBits(data[i], 8);
  }
};

#endif
