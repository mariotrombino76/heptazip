// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
// fuzz_codec.cpp -- harness libFuzzer per i decoder senza gestore di archivio (design 0007)
//
// fuzz_archive arriva ai decoder solo attraverso un archivio valido fino all'intestazione:
// qui l'input va direttamente al decoder scelto, con le proprieta' prese dall'input.
//
//   FUZZ_CODEC=LZMA2 ./fuzz_codec -malloc_limit_mb=4200 corpus/LZMA2
//
// FUZZ_CODEC: nome del metodo come per -m (LZMA, LZMA2, PPMD, Deflate, Deflate64, BZip2,
// ZSTD, BCJ, BCJ2, ARM64, Delta, Rar1..Rar5, ...).
//
// Formato dell'input:
//   byte 0           flag: bit 0-4 = numero di byte di proprieta' P, bit 5 = dimensione di
//                    uscita nota, bit 6 = finish mode, bit 7 = dimensione d'ingresso nota
//   P byte           proprieta' (SetDecoderProperties2), se il decoder le accetta
//   4 byte           dimensione di uscita (little endian, al piu' kMaxOutput), se bit 5
//   resto            dati compressi; per i decoder a piu' stream (BCJ2) il resto e'
//                    diviso in stream con una lunghezza di 2 byte prima di ognuno
//                    tranne l'ultimo
//
// Senza libFuzzer (FUZZ_REPLAY definito) diventa fuzz_codec_replay, che riesegue gli
// input salvati in tests/fuzz/regress-codec/<metodo>/ (test CTest fuzz_codec_regress).

#include "../../CPP/Common/Common.h"
#include "../../C/CpuArch.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef FUZZ_REPLAY
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#endif

#include "../../CPP/Common/MyWindows.h"
#include "../../CPP/Common/MyInitGuid.h"

#include "../../CPP/Common/MyCom.h"
#include "../../CPP/Common/MyString.h"

#include "../../CPP/7zip/Common/CreateCoder.h"
#include "../../CPP/7zip/Common/StreamObjects.h"
#include "../../CPP/7zip/ICoder.h"
// Definiscono (con MyInitGuid.h) i GUID usati dagli oggetti dei gestori, linkati con i codec.
#include "../../CPP/7zip/Archive/IArchive.h"
#include "../../CPP/7zip/IPassword.h"
#include "../../CPP/7zip/UI/Common/LoadCodecs.h"
#include "../../CPP/7zip/UI/Common/IFileExtractCallback.h"

namespace {

// Uscita massima per input: oltre, la scrittura fallisce e il decoder si ferma.
const UInt32 kMaxOutput = (UInt32)64 << 20;
const unsigned kMaxStreams = 8;
UInt64 g_OutputTotal;

Z7_CLASS_IMP_COM_1(
  CNullOutStream
  , ISequentialOutStream
)
};

Z7_COM7F_IMF(CNullOutStream::Write(const void *, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (g_OutputTotal + size > kMaxOutput)
    return E_FAIL;
  g_OutputTotal += size;
  if (processedSize)
    *processedSize = size;
  return S_OK;
}

CMethodId g_MethodId;
UInt32 g_NumStreams = 1;

// Dizionario massimo accettato dalle proprieta'. Come per LZMA (vedi design 0007, casi
// attesi) il decoder alloca il dizionario dichiarato: nell'uso reale il gestore di archivio
// lo confronta prima con il limite di memoria. Solo RAR5 arriva oltre -malloc_limit_mb
// (fino a 64 GiB): qui le sue proprieta' oltre 1 GiB si scartano.
const UInt64 kMaxRar5Dict = (UInt64)1 << 30;
const CMethodId kRar5Id = 0x40305;

}


static bool SelectCodec(const char *name)
{
  CMethodId id;
  UInt32 numStreams = 0;
  bool isFilter = false;
  if (FindMethod_Index(AString(name), false, id, numStreams, isFilter) < 0)
  {
    fprintf(stderr, "metodo sconosciuto: %s\n", name);
    return false;
  }
  if (numStreams == 0 || numStreams > kMaxStreams)
  {
    fprintf(stderr, "metodo %s: %u stream non supportati\n", name, (unsigned)numStreams);
    return false;
  }
  g_MethodId = id;
  g_NumStreams = numStreams;
  return true;
}

#ifndef FUZZ_REPLAY
extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
  const char *name = getenv("FUZZ_CODEC");
  if (!name)
  {
    fprintf(stderr, "FUZZ_CODEC non impostata (es. FUZZ_CODEC=LZMA2)\n");
    exit(2);
  }
  if (!SelectCodec(name))
    exit(2);
  return 0;
}
#endif


extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  if (size < 1)
    return 0;
  const unsigned flags = data[0];
  data++;
  size--;

  const unsigned propsSize = flags & 31;
  if (size < propsSize)
    return 0;
  const Byte *props = data;
  data += propsSize;
  size -= propsSize;

  UInt64 outSize = 0;
  const bool outSizeDefined = (flags & 0x20) != 0;
  if (outSizeDefined)
  {
    if (size < 4)
      return 0;
    outSize = GetUi32(data) % (kMaxOutput + 1);
    data += 4;
    size -= 4;
  }

  if (g_MethodId == kRar5Id && propsSize == 2
      && (props[0] > 30 || ((UInt64)((props[1] >> 3) + 32) << (props[0] + 12)) > kMaxRar5Dict))
    return 0;

  CCreatedCoder cod;
  if (CreateCoder_Id(g_MethodId, false, cod) != S_OK)
    return 0;
  IUnknown *unk = cod.Coder ? (IUnknown *)cod.Coder : (IUnknown *)cod.Coder2;
  if (!unk)
    return 0;

  {
    CMyComPtr<ICompressSetDecoderProperties2> setProps;
    unk->QueryInterface(IID_ICompressSetDecoderProperties2, (void **)&setProps);
    if (setProps && setProps->SetDecoderProperties2(props, propsSize) != S_OK)
      return 0;
  }
  {
    CMyComPtr<ICompressSetFinishMode> setFinish;
    unk->QueryInterface(IID_ICompressSetFinishMode, (void **)&setFinish);
    if (setFinish)
      setFinish->SetFinishMode((flags & 0x40) ? 1 : 0);
  }

  // Divide il resto negli stream d'ingresso.
  const unsigned numStreams = cod.Coder ? 1 : (unsigned)cod.NumStreams;
  if (numStreams == 0 || numStreams > kMaxStreams)
    return 0;
  CMyComPtr<ISequentialInStream> inStreams[kMaxStreams];
  ISequentialInStream *inPtrs[kMaxStreams];
  UInt64 inSizes[kMaxStreams];
  const UInt64 *inSizePtrs[kMaxStreams];
  for (unsigned i = 0; i < numStreams; i++)
  {
    size_t len = size;
    if (i + 1 < numStreams)
    {
      if (size < 2)
        return 0;
      len = GetUi16(data);
      data += 2;
      size -= 2;
      if (len > size)
        len = size;
    }
    CBufInStream *spec = new CBufInStream;
    inStreams[i] = spec;
    spec->Init(data, len);
    inPtrs[i] = inStreams[i];
    inSizes[i] = len;
    inSizePtrs[i] = (flags & 0x80) ? &inSizes[i] : NULL;
    data += len;
    size -= len;
  }

  g_OutputTotal = 0;
  CMyComPtr<ISequentialOutStream> outStream = new CNullOutStream;
  const UInt64 *outSizePtr = outSizeDefined ? &outSize : NULL;
  if (cod.Coder)
    cod.Coder->Code(inPtrs[0], outStream, inSizePtrs[0], outSizePtr, NULL);
  else
  {
    ISequentialOutStream *outPtr = outStream;
    cod.Coder2->Code(inPtrs, inSizePtrs, numStreams, &outPtr, &outSizePtr, 1, NULL);
  }
  return 0;
}


#ifdef FUZZ_REPLAY

static std::vector<std::string> ListDir(const std::string &dir, bool wantDirs)
{
  std::vector<std::string> names;
  DIR *d = opendir(dir.c_str());
  if (!d)
    return names;
  while (const struct dirent *e = readdir(d))
  {
    if (e->d_name[0] == '.')
      continue;
    struct stat st;
    if (stat((dir + "/" + e->d_name).c_str(), &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode) == wantDirs)
      names.push_back(e->d_name);
  }
  closedir(d);
  return names;
}

static bool ReadFile(const std::string &path, std::vector<uint8_t> &data)
{
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  data.clear();
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) != 0)
    data.insert(data.end(), buf, buf + n);
  fclose(f);
  return true;
}

// Riesegue <dir>/<metodo>/<file>: un crash (o un errore dei sanitizer) fa fallire il test.
int main(int argc, char **argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "uso: %s <dir_regress>\n", argv[0]);
    return 2;
  }
  const std::string root = argv[1];
  unsigned numFiles = 0;
  for (const std::string &method : ListDir(root, true))
  {
    if (!SelectCodec(method.c_str()))
      return 1;
    for (const std::string &name : ListDir(root + "/" + method, false))
    {
      std::vector<uint8_t> data;
      if (!ReadFile(root + "/" + method + "/" + name, data))
        return 1;
      LLVMFuzzerTestOneInput(data.data(), data.size());
      numFiles++;
    }
  }
  printf("fuzz_codec_replay: %u input rieseguiti\n", numFiles);
  return numFiles != 0 ? 0 : 1;
}

#endif
