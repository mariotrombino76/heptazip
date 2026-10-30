// 7zExtract.cpp

#include "StdAfx.h"

#include "../../../../C/7zCrc.h"

#include "../../../Common/ComTry.h"

#include "../../Common/ProgressUtils.h"

#include "7zDecode.h"
#include "7zHandler.h"

// EXTERN_g_ExternalCodecs

namespace NArchive {
namespace N7z {

Z7_CLASS_IMP_COM_1(
  CFolderOutStream
  , ISequentialOutStream
  /* , ICompressGetSubStreamSize */
)
  CMyComPtr<ISequentialOutStream> _stream;
public:
  bool TestMode;
  bool CheckCrc;
private:
  bool _fileIsOpen;
  bool _calcCrc;
  UInt32 _crc;
  UInt64 _rem;

  const UInt32 *_indexes;
  // unsigned _startIndex;
  unsigned _numFiles;
  unsigned _fileIndex;

  HRESULT OpenFile(bool isCorrupted = false);
  HRESULT CloseFile_and_SetResult(Int32 res);
  HRESULT CloseFile();
  HRESULT ProcessEmptyFiles();

public:
  const CDbEx *_db;
  CMyComPtr<IArchiveExtractCallback> ExtractCallback;

  bool ExtraWriteWasCut;

  CFolderOutStream():
      TestMode(false),
      CheckCrc(true)
      {}

  HRESULT Init(unsigned startIndex, const UInt32 *indexes, unsigned numFiles);
  HRESULT FlushCorrupted(Int32 callbackOperationResult);

  bool WasWritingFinished() const { return _numFiles == 0; }
};


HRESULT CFolderOutStream::Init(unsigned startIndex, const UInt32 *indexes, unsigned numFiles)
{
  // _startIndex = startIndex;
  _fileIndex = startIndex;
  _indexes = indexes;
  _numFiles = numFiles;
  
  _fileIsOpen = false;
  ExtraWriteWasCut = false;
  
  return ProcessEmptyFiles();
}

HRESULT CFolderOutStream::OpenFile(bool isCorrupted)
{
  const CFileItem &fi = _db->Files[_fileIndex];
  const UInt32 nextFileIndex = (_indexes ? *_indexes : _fileIndex);
  Int32 askMode = (_fileIndex == nextFileIndex) ? TestMode ?
      NExtract::NAskMode::kTest :
      NExtract::NAskMode::kExtract :
      NExtract::NAskMode::kSkip;

  if (isCorrupted
      && askMode == NExtract::NAskMode::kExtract
      && !_db->IsItemAnti(_fileIndex)
      && !fi.IsDir)
    askMode = NExtract::NAskMode::kTest;
  
  CMyComPtr<ISequentialOutStream> realOutStream;
  RINOK(ExtractCallback->GetStream(_fileIndex, &realOutStream, askMode))
  
  _stream = realOutStream;
  _crc = CRC_INIT_VAL;
  _calcCrc = (CheckCrc && fi.CrcDefined && !fi.IsDir);

  _fileIsOpen = true;
  _rem = fi.Size;
  
  if (askMode == NExtract::NAskMode::kExtract
      && !realOutStream
      && !_db->IsItemAnti(_fileIndex)
      && !fi.IsDir)
    askMode = NExtract::NAskMode::kSkip;
  return ExtractCallback->PrepareOperation(askMode);
}

HRESULT CFolderOutStream::CloseFile_and_SetResult(Int32 res)
{
  _stream.Release();
  _fileIsOpen = false;
  
  if (!_indexes)
    _numFiles--;
  else if (*_indexes == _fileIndex)
  {
    _indexes++;
    _numFiles--;
  }

  _fileIndex++;
  return ExtractCallback->SetOperationResult(res);
}

HRESULT CFolderOutStream::CloseFile()
{
  const CFileItem &fi = _db->Files[_fileIndex];
  return CloseFile_and_SetResult((!_calcCrc || fi.Crc == CRC_GET_DIGEST(_crc)) ?
      NExtract::NOperationResult::kOK :
      NExtract::NOperationResult::kCRCError);
}

HRESULT CFolderOutStream::ProcessEmptyFiles()
{
  while (_numFiles != 0 && _db->Files[_fileIndex].Size == 0)
  {
    RINOK(OpenFile())
    RINOK(CloseFile())
  }
  return S_OK;
}

Z7_COM7F_IMF(CFolderOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  
  while (size != 0)
  {
    if (_fileIsOpen)
    {
      UInt32 cur = (size < _rem ? size : (UInt32)_rem);
      if (_calcCrc)
      {
        const UInt32 k_Step = (UInt32)1 << 20;
        if (cur > k_Step)
          cur = k_Step;
      }
      HRESULT result = S_OK;
      if (_stream)
        result = _stream->Write(data, cur, &cur);
      if (_calcCrc)
        _crc = CrcUpdate(_crc, data, cur);
      if (processedSize)
        *processedSize += cur;
      data = (const Byte *)data + cur;
      size -= cur;
      _rem -= cur;
      if (_rem == 0)
      {
        RINOK(CloseFile())
        RINOK(ProcessEmptyFiles())
      }
      RINOK(result)
      if (cur == 0)
        break;
      continue;
    }
  
    RINOK(ProcessEmptyFiles())
    if (_numFiles == 0)
    {
      // we support partial extracting
      /*
      if (processedSize)
        *processedSize += size;
      break;
      */
      ExtraWriteWasCut = true;
      // return S_FALSE;
      return k_My_HRESULT_WritingWasCut;
    }
    RINOK(OpenFile())
  }
  
  return S_OK;
}

HRESULT CFolderOutStream::FlushCorrupted(Int32 callbackOperationResult)
{
  while (_numFiles != 0)
  {
    if (_fileIsOpen)
    {
      RINOK(CloseFile_and_SetResult(callbackOperationResult))
    }
    else
    {
      RINOK(OpenFile(true))
    }
  }
  return S_OK;
}

/*
Z7_COM7F_IMF(CFolderOutStream::GetSubStreamSize(UInt64 subStream, UInt64 *value))
{
  *value = 0;
  // const unsigned numFiles_Original = _numFiles + _fileIndex - _startIndex;
  const unsigned numFiles_Original = _numFiles;
  if (subStream >= numFiles_Original)
    return S_FALSE; // E_FAIL;
  *value = _db->Files[_startIndex + (unsigned)subStream].Size;
  return S_OK;
}
*/


/* 7zip-modern: the planning of one extraction step and the handling of the
   folder decoding result were moved out of CHandler::Extract() into the two
   functions below, without behavior changes, so that the parallel folder
   extraction (docs/design/0001-estrazione-parallela-folder-7z.md) can reuse
   exactly the same logic. */

/* One step of extraction: a run of consecutive requested items that belong to
   the same folder (or one item without folder: empty file, dir, anti-item). */
struct CExtractJob
{
  UInt32 NumItems;        // number of requested items processed by this step
  UInt32 StartFileIndex;  // first file passed to CFolderOutStream::Init()
  CNum FolderIndex;       // kNumNoIndex, if the item has no data stream
  UInt64 UnpackSize;      // bytes to unpack: up to the end of the last requested file
  UInt64 PackSize;
};

static void GetExtractJob(const CDbEx &db,
    const UInt32 *indices, UInt32 numItems, bool allFilesMode,
    UInt32 i, CExtractJob &job)
{
  job.UnpackSize = 0;
  job.PackSize = 0;

  UInt32 fileIndex = allFilesMode ? i : indices[i];
  const CNum folderIndex = db.FileIndexToFolderIndexMap[fileIndex];

  UInt32 numSolidFiles = 1;

  if (folderIndex != kNumNoIndex)
  {
    job.PackSize = db.GetFolderFullPackSize(folderIndex);
    UInt32 nextFile = fileIndex + 1;
    fileIndex = db.FolderStartFileIndex[folderIndex];
    UInt32 k;

    for (k = i + 1; k < numItems; k++)
    {
      const UInt32 fileIndex2 = allFilesMode ? k : indices[k];
      if (db.FileIndexToFolderIndexMap[fileIndex2] != folderIndex
          || fileIndex2 < nextFile)
        break;
      nextFile = fileIndex2 + 1;
    }

    numSolidFiles = k - i;

    for (k = fileIndex; k < nextFile; k++)
      job.UnpackSize += db.Files[k].Size;
  }

  job.NumItems = numSolidFiles;
  job.StartFileIndex = fileIndex;
  job.FolderIndex = folderIndex;
}

/* Processes the result of CDecoder::Decode() for one folder.
   Returns S_OK, if extraction must continue with the next step. */
static HRESULT ProcessFolderDecodeResult(
    CFolderOutStream *folderOutStream,
    IArchiveExtractCallbackMessage2 *callbackMessage,
    CNum folderIndex, HRESULT result, bool dataAfterEnd_Error)
{
  if (result == S_FALSE || result == E_NOTIMPL || dataAfterEnd_Error)
  {
    const bool wasFinished = folderOutStream->WasWritingFinished();

    int resOp = NExtract::NOperationResult::kDataError;

    if (result != S_FALSE)
    {
      if (result == E_NOTIMPL)
        resOp = NExtract::NOperationResult::kUnsupportedMethod;
      else if (wasFinished && dataAfterEnd_Error)
        resOp = NExtract::NOperationResult::kDataAfterEnd;
    }

    RINOK(folderOutStream->FlushCorrupted(resOp))

    if (wasFinished)
    {
      // we don't show error, if it's after required files
      if (/* !folderOutStream->ExtraWriteWasCut && */ callbackMessage)
      {
        RINOK(callbackMessage->ReportExtractResult(NEventIndexType::kBlockIndex, folderIndex, resOp))
      }
    }
    return S_OK;
  }

  if (result != S_OK)
    return result;

  return folderOutStream->FlushCorrupted(NExtract::NOperationResult::kDataError);
}


Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testModeSpec, IArchiveExtractCallback *extractCallbackSpec))
{
  // for GCC
  // CFolderOutStream *folderOutStream = new CFolderOutStream;
  // CMyComPtr<ISequentialOutStream> outStream(folderOutStream);

  COM_TRY_BEGIN
  
  CMyComPtr<IArchiveExtractCallback> extractCallback = extractCallbackSpec;
  
  UInt64 importantTotalUnpacked = 0;

  // numItems = (UInt32)(Int32)-1;

  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _db.Files.Size();

  if (numItems == 0)
    return S_OK;

  {
    CNum prevFolder = kNumNoIndex;
    UInt32 nextFile = 0;
    
    UInt32 i;
    
    for (i = 0; i < numItems; i++)
    {
      const UInt32 fileIndex = allFilesMode ? i : indices[i];
      const CNum folderIndex = _db.FileIndexToFolderIndexMap[fileIndex];
      if (folderIndex == kNumNoIndex)
        continue;
      if (folderIndex != prevFolder || fileIndex < nextFile)
        nextFile = _db.FolderStartFileIndex[folderIndex];
      for (CNum index = nextFile; index <= fileIndex; index++)
        importantTotalUnpacked += _db.Files[index].Size;
      nextFile = fileIndex + 1;
      prevFolder = folderIndex;
    }
  }

  RINOK(extractCallback->SetTotal(importantTotalUnpacked))

  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(extractCallback, false);

  CDecoder decoder(
    #if !defined(USE_MIXER_MT)
      false
    #elif !defined(USE_MIXER_ST)
      true
    #elif !defined(Z7_7Z_SET_PROPERTIES)
      #ifdef Z7_ST
        false
      #else
        true
      #endif
    #else
      _useMultiThreadMixer
    #endif
    );

  UInt64 curPacked, curUnpacked;

  CMyComPtr<IArchiveExtractCallbackMessage2> callbackMessage;
  extractCallback.QueryInterface(IID_IArchiveExtractCallbackMessage2, &callbackMessage);

  CFolderOutStream *folderOutStream = new CFolderOutStream;
  CMyComPtr<ISequentialOutStream> outStream(folderOutStream);

  folderOutStream->_db = &_db;
  folderOutStream->ExtractCallback = extractCallback;
  folderOutStream->TestMode = (testModeSpec != 0);
  folderOutStream->CheckCrc = (_crcSize != 0);

  for (UInt32 i = 0;; lps->OutSize += curUnpacked, lps->InSize += curPacked)
  {
    RINOK(lps->SetCur())

    if (i >= numItems)
      break;

    CExtractJob job;
    GetExtractJob(_db, indices, numItems, allFilesMode, i, job);
    curUnpacked = job.UnpackSize;
    curPacked = job.PackSize;
    const CNum folderIndex = job.FolderIndex;

    {
      const HRESULT result = folderOutStream->Init(job.StartFileIndex,
          allFilesMode ? NULL : indices + i,
          job.NumItems);

      i += job.NumItems;

      RINOK(result)
    }

    if (folderOutStream->WasWritingFinished())
    {
      // for debug: to test zero size stream unpacking
      // if (folderIndex == kNumNoIndex)  // enable this check for debug
      continue;
    }

    if (folderIndex == kNumNoIndex)
      return E_FAIL;

    #ifndef Z7_NO_CRYPTO
    CMyComPtr<ICryptoGetTextPassword> getTextPassword;
    if (extractCallback)
      extractCallback.QueryInterface(IID_ICryptoGetTextPassword, &getTextPassword);
    #endif

    try
    {
      #ifndef Z7_NO_CRYPTO
        bool isEncrypted = false;
        bool passwordIsDefined = false;
        UString_Wipe password;
      #endif

      bool dataAfterEnd_Error = false;

      const HRESULT result = decoder.Decode(
          EXTERNAL_CODECS_VARS
          _inStream,
          _db.ArcInfo.DataStartPosition,
          _db, folderIndex,
          &curUnpacked,

          outStream,
          lps,
          NULL // *inStreamMainRes
          , dataAfterEnd_Error
          
          Z7_7Z_DECODER_CRYPRO_VARS
          #if !defined(Z7_ST)
            , true, _numThreads, _memUsage_Decompress
          #endif
          );

      RINOK(ProcessFolderDecodeResult(folderOutStream, callbackMessage,
          folderIndex, result, dataAfterEnd_Error))
      continue;
    }
    catch(...)
    {
      RINOK(folderOutStream->FlushCorrupted(NExtract::NOperationResult::kDataError))
      // continue;
      // return E_FAIL;
      throw;
    }
  }

  return S_OK;

  COM_TRY_END
}

}}
