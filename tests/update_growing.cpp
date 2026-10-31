// update_growing.cpp
//
// Crea un archivio .7z con l'handler 7z (IOutArchive::UpdateItems) e una callback che
// DICHIARA per un file una dimensione diversa da quella dei dati che poi fornisce:
// come un file che cresce (o si accorcia) tra la scansione e la lettura.
// Serve a provare in modo deterministico i rami della compressione parallela per i
// file cresciuti (docs/design/0003, par. 5.3): riserva della dimensione reale, e
// lettura anticipata fermata a kMaxParallelEncodeReadSize con il resto in linea.
//
//   update_growing <archivio.7z> <caso> [nome=valore ...]
//     caso: grow-small | grow-huge | shrink
//     nome=valore: proprieta' dell'handler come -m<nome>=<valore> (es. pfc=on mt=4)
//
// Uscita: 0 se UpdateItems() riesce. Il confronto tra configurazioni e la verifica
// dell'archivio li fa tests/update_growing.py.

#include "../CPP/Common/Common.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "../CPP/Common/MyWindows.h"
#include "../CPP/Common/MyInitGuid.h"

#include "../CPP/Common/MyCom.h"
#include "../CPP/Common/MyString.h"
#include "../CPP/Common/StringConvert.h"

#include "../CPP/7zip/Common/FileStreams.h"
#include "../CPP/7zip/Archive/IArchive.h"
#include "../CPP/7zip/Archive/7z/7zHandler.h"
#include "../CPP/7zip/UI/Common/SetProperties.h"
#include "../CPP/7zip/UI/Common/IFileExtractCallback.h"

#include "../C/7zCrc.h"

namespace {

struct CFileSpec
{
  UInt64 Declared;  // kpidSize
  UInt64 Actual;    // bytes of the stream
};

/* Data of file (index): text-like, compressible (the big case must be fast). */
static Byte DataByte(unsigned index, UInt64 pos)
{
  static const char kWords[] = "alpha beta gamma delta epsilon zeta eta theta iota kappa\n";
  const UInt64 k = pos + index * 7;
  return (Byte)(kWords[(k + (k >> 12)) % (sizeof(kWords) - 1)] ^ (Byte)(((k >> 16) & 1) << 5));
}

Z7_CLASS_IMP_COM_1(
  CGenInStream
  , ISequentialInStream
)
  unsigned _index;
  UInt64 _size;
  UInt64 _pos;
public:
  CGenInStream(unsigned index, UInt64 size): _index(index), _size(size), _pos(0) {}
};

Z7_COM7F_IMF(CGenInStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  const UInt64 rem = _size - _pos;
  if (size > rem)
    size = (UInt32)rem;
  Byte *p = (Byte *)data;
  for (UInt32 i = 0; i < size; i++)
    p[i] = DataByte(_index, _pos + i);
  _pos += size;
  if (processedSize)
    *processedSize = size;
  return S_OK;
}


Z7_CLASS_IMP_COM_1(
  CUpdateCallback
  , IArchiveUpdateCallback
)
  Z7_IFACE_COM7_IMP(IProgress)
public:
  std::vector<CFileSpec> Files;
  unsigned NumResults = 0;
};

Z7_COM7F_IMF(CUpdateCallback::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CUpdateCallback::SetCompleted(const UInt64 *)) { return S_OK; }

Z7_COM7F_IMF(CUpdateCallback::GetUpdateItemInfo(UInt32 /* index */,
    Int32 *newData, Int32 *newProps, UInt32 *indexInArchive))
{
  if (newData) *newData = 1;
  if (newProps) *newProps = 1;
  if (indexInArchive) *indexInArchive = (UInt32)(Int32)-1;
  return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  NWindows::NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPath:
    {
      char name[32];
      snprintf(name, sizeof(name), "f%u.txt", (unsigned)index);
      prop = GetUnicodeString(AString(name));
      break;
    }
    case kpidIsDir: prop = false; break;
    case kpidIsAnti: prop = false; break;
    case kpidSize: prop = Files[index].Declared; break;
    case kpidAttrib: prop = (UInt32)FILE_ATTRIBUTE_ARCHIVE; break;
    case kpidMTime:
    {
      FILETIME ft;
      ft.dwLowDateTime = 0x12345678;
      ft.dwHighDateTime = 0x01D00000;
      prop = ft;
      break;
    }
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::GetStream(UInt32 index, ISequentialInStream **inStream))
{
  CMyComPtr<ISequentialInStream> s = new CGenInStream(index, Files[index].Actual);
  *inStream = s.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::SetOperationResult(Int32 /* operationResult */))
{
  NumResults++;
  return S_OK;
}

} // namespace


int main(int argc, char **argv)
{
  if (argc < 3)
  {
    fprintf(stderr, "uso: update_growing <archivio.7z> grow-small|grow-huge|shrink [nome=valore ...]\n");
    return 2;
  }
  CrcGenerateTable();

  const UInt64 kKiB = 1 << 10;
  const UInt64 kMiB = 1 << 20;
  CUpdateCallback *callbackSpec = new CUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> callback = callbackSpec;
  std::vector<CFileSpec> &files = callbackSpec->Files;
  for (unsigned i = 0; i < 6; i++)
    files.push_back({ 200 * kKiB + i * 10 * kKiB, 200 * kKiB + i * 10 * kKiB });

  const char *testCase = argv[2];
  if (strcmp(testCase, "grow-small") == 0)
    files[2].Actual = 700 * kKiB;           // more than reserved, less than the read limit
  else if (strcmp(testCase, "grow-huge") == 0)
  {
    files[2].Declared = 1 * kMiB;           // planned as a small folder...
    files[2].Actual = 140 * kMiB;           // ...read ahead stops at 128 MiB
  }
  else if (strcmp(testCase, "shrink") == 0)
    files[2].Actual = 10 * kKiB;
  else
  {
    fprintf(stderr, "caso sconosciuto: %s\n", testCase);
    return 2;
  }

  CObjectVector<CProperty> props;
  for (int i = 3; i < argc; i++)
  {
    const AString a(argv[i]);
    const int eq = a.Find('=');
    CProperty &p = props.AddNew();
    if (eq < 0)
      p.Name = GetUnicodeString(a);
    else
    {
      p.Name = GetUnicodeString(a.Left((unsigned)eq));
      p.Value = GetUnicodeString(a.Ptr((unsigned)(eq + 1)));
    }
  }

  CMyComPtr<IOutArchive> archive = new NArchive::N7z::CHandler;
  {
    const HRESULT res = SetProperties(archive, props);
    if (res != S_OK)
    {
      fprintf(stderr, "SetProperties: 0x%08x\n", (unsigned)res);
      return 1;
    }
  }

  COutFileStream *outSpec = new COutFileStream;
  CMyComPtr<IOutStream> outStream = outSpec;
  if (!outSpec->Create_ALWAYS(us2fs(GetUnicodeString(AString(argv[1])))))
  {
    fprintf(stderr, "impossibile creare: %s\n", argv[1]);
    return 1;
  }

  const HRESULT res = archive->UpdateItems(outStream, (UInt32)files.size(), callback);
  outSpec->Close();
  printf("UPDATE 0x%08x results=%u\n", (unsigned)res, callbackSpec->NumResults);
  return res == S_OK ? 0 : 1;
}
