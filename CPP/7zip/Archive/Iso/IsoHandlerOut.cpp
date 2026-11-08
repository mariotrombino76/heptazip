// IsoHandlerOut.cpp
// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
//
// IOutArchive of the ISO handler: collects the items from the update callback (and
// from the open image, when an existing one is rewritten) and hands them to the
// writer of IsoOut.cpp. An image is always written whole: there is no in-place update.
// Boot entries (El Torito) come from the -mboot / -mefi options or, on a rewrite,
// from the open image (design note 0020).

#include "StdAfx.h"

#include "../../../../C/CpuArch.h"

#include "../../../Common/ComTry.h"
#include "../../../Common/StringConvert.h"
#include "../../../Common/UTFConvert.h"

#include "../../../Windows/FileIO.h"
#include "../../../Windows/PropVariant.h"
#include "../../../Windows/TimeUtils.h"

#include "../../Common/MethodProps.h"
#include "../../Common/StreamUtils.h"

#include "../Common/ItemNameUtils.h"

#include "IsoHandler.h"
#include "IsoOut.h"

using namespace NWindows;

namespace NArchive {
namespace NIso {

Z7_COM7F_IMF(CHandler::GetFileTimeType(UInt32 *type))
{
  *type = NFileTimeType::kUnix;   // the records keep whole seconds
  return S_OK;
}

static void NormalizePath(UString &s)
{
  NItemName::ReplaceSlashes_OsToUnix(s);
  s.Replace(L'\\', L'/');
  while (!s.IsEmpty() && s[0] == L'/')
    s.Delete(0);
  while (!s.IsEmpty() && s.Back() == L'/')
    s.DeleteBack();
}

Z7_COM7F_IMF(CHandler::SetProperties(const wchar_t * const *names, const PROPVARIANT *values, UInt32 numProps))
{
  _volumeId.Empty();
  _joliet = true;
  _rockRidge = true;
  _bootPath.Empty();
  _efiPath.Empty();
  _catalogPath.Empty();
  _bootLoad = 4;
  _bootInfo = -1;
  _hybrid = -1;
  _mbrCode.Free();
  for (UInt32 i = 0; i < numProps; i++)
  {
    UString name = names[i];
    name.MakeLower_Ascii();
    if (name.IsEmpty())
      return E_INVALIDARG;
    const PROPVARIANT &prop = values[i];
    if (name.IsEqualTo("vol"))
    {
      if (prop.vt != VT_BSTR)
        return E_INVALIDARG;
      ConvertUnicodeToUTF8(prop.bstrVal, _volumeId);
    }
    else if (name.IsEqualTo("joliet"))
    {
      RINOK(PROPVARIANT_to_bool(prop, _joliet))
    }
    else if (name.IsEqualTo("rr"))
    {
      RINOK(PROPVARIANT_to_bool(prop, _rockRidge))
    }
    else if (name.IsEqualTo("boot") || name.IsEqualTo("efi") || name.IsEqualTo("catalog"))
    {
      // paths inside the image
      if (prop.vt != VT_BSTR)
        return E_INVALIDARG;
      UString s = prop.bstrVal;
      NormalizePath(s);
      if (s.IsEmpty())
        return E_INVALIDARG;
      (name[0] == L'b' ? _bootPath : name[0] == L'e' ? _efiPath : _catalogPath) = s;
    }
    else if (name.IsEqualTo("bootload"))
    {
      UInt32 v;
      RINOK(ParsePropToUInt32(UString(), prop, v))
      if (v > 0xFFFF)
        return E_INVALIDARG;
      _bootLoad = v;
    }
    else if (name.IsEqualTo("bootinfo"))
    {
      if (prop.vt == VT_BSTR && UString(prop.bstrVal).IsEqualTo_Ascii_NoCase("auto"))
        _bootInfo = -1;
      else
      {
        bool b;
        RINOK(PROPVARIANT_to_bool(prop, b))
        _bootInfo = b ? 1 : 0;
      }
    }
    else if (name.IsEqualTo("hybrid"))
    {
      bool b;
      RINOK(PROPVARIANT_to_bool(prop, b))
      _hybrid = b ? 1 : 0;
    }
    else if (name.IsEqualTo("mbr"))
    {
      // a file of the local file system with the boot code of the MBR (isohdpfx.bin
      // of syslinux: 432 bytes; a 512-byte sector is accepted, only its code is used)
      if (prop.vt != VT_BSTR)
        return E_INVALIDARG;
      NFile::NIO::CInFile file;
      if (!file.Open(us2fs(UString(prop.bstrVal))))
        return GetLastError_noZero_HRESULT();
      Byte buf[512];
      size_t got = 0;
      if (!file.ReadFull(buf, sizeof(buf), got) || got == 0)
        return E_INVALIDARG;
      if (got > 432)
        got = 432;
      _mbrCode.CopyFrom(buf, got);
    }
    else if (name[0] == L'x' || name.IsPrefixedBy_Ascii_NoCase("mt") || name.IsPrefixedBy_Ascii_NoCase("memuse"))
    {
      // level, threads, memory: accepted and ignored (an image is not compressed)
    }
    else
      return E_INVALIDARG;
  }
  return S_OK;
}

namespace {

// data of the items: new ones from the callback, the others from the open image
class CDataSource Z7_final: public IOutDataSource
{
  IArchiveUpdateCallback *_callback;
  IInArchiveGetStream *_arc;
  CMyComPtr<IArchiveUpdateCallbackFile> _opCallback;
public:
  CDataSource(IArchiveUpdateCallback *callback, IInArchiveGetStream *arc):
      _callback(callback), _arc(arc)
  {
    callback->QueryInterface(IID_IArchiveUpdateCallbackFile, (void **)&_opCallback);
  }

  HRESULT GetStream(const COutItem &item, ISequentialInStream **stream) Z7_override
  {
    *stream = NULL;
    if (item.NewData)
      return _callback->GetStream(item.IndexInClient, stream);
    return _arc->GetStream(item.IndexInArc, stream);
  }

  HRESULT ReportResult(const COutItem &item, bool changed) Z7_override
  {
    if (!item.NewData)
      return S_OK;
    if (changed && _opCallback)
    {
      RINOK(_opCallback->ReportOperation(NEventIndexType::kOutArcIndex, item.IndexInClient,
          NUpdateNotifyOp::kInFileChanged))
    }
    return _callback->SetOperationResult(NArchive::NUpdate::NOperationResult::kOK);
  }
};

}

// fills an item from the properties returned by `get(propId, &prop)`: the update
// callback for new items, this handler's GetProperty for the items of the open image
template <class TGet>
static HRESULT GetItemFromProps(TGet get, COutItem &item, bool getSize)
{
  NCOM::CPropVariant prop;
  RINOK(get(kpidPath, &prop))
  if (prop.vt != VT_BSTR)
    return E_INVALIDARG;
  item.Path = prop.bstrVal;
  NormalizePath(item.Path);

  prop.Clear();
  RINOK(get(kpidIsDir, &prop))
  if (prop.vt == VT_BOOL)
    item.IsDir = (prop.boolVal != VARIANT_FALSE);
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;

  prop.Clear();
  RINOK(get(kpidMTime, &prop))
  if (prop.vt == VT_FILETIME)
    item.MTime = ((UInt64)prop.filetime.dwHighDateTime << 32) | prop.filetime.dwLowDateTime;
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;

  prop.Clear();
  RINOK(get(kpidPosixAttrib, &prop))
  if (prop.vt == VT_UI4)
    item.Mode = prop.ulVal;
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;

  prop.Clear();
  RINOK(get(kpidSymLink, &prop))
  if (prop.vt == VT_BSTR)
  {
    UString s = prop.bstrVal;
    NItemName::ReplaceSlashes_OsToUnix(s);
    ConvertUnicodeToUTF8(s, item.SymLink);
  }
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;

  if (getSize && !item.IsDir && item.SymLink.IsEmpty())
  {
    prop.Clear();
    RINOK(get(kpidSize, &prop))
    if (prop.vt != VT_UI8)
      return E_INVALIDARG;
    item.Size = prop.uhVal.QuadPart;
  }
  return S_OK;
}

// the file of the open image whose data starts at `block`, -1 if none
int CHandler::FindRefByBlock(UInt32 block) const
{
  FOR_VECTOR (i, _archive.Refs)
  {
    const CRef &ref = _archive.Refs[i];
    const CDir &item = ref.Dir->_subItems[ref.Index];
    if (!item.IsDir() && item.ExtentLocation == block && ref.TotalSize != 0)
      return (int)i;
  }
  return -1;
}

Z7_COM7F_IMF(CHandler::UpdateItems(ISequentialOutStream *outStream, UInt32 numItems,
    IArchiveUpdateCallback *callback))
{
  COM_TRY_BEGIN

  if (!callback)
    return E_FAIL;
  if (_stream)
  {
    // a rewrite of an open image: only if it was read cleanly
    if (!_archive.IsArc || _archive.UnexpectedEnd || _archive.HeadersError)
      return E_NOTIMPL;
  }
  const unsigned numRefs = _stream ? _archive.Refs.Size() : 0;

  COutOptions options;

  // the boot catalog of the open image: regenerated, its old content is dropped
  int oldCatalogRef = -1;
  if (_stream && !_archive.BootEntries.IsEmpty())
    oldCatalogRef = FindRefByBlock(_archive.BootCatalogBlock);

  CObjectVector<COutItem> items;
  // index in `items` of each item of the open image, -1 if not kept
  CIntVector arcToItem;
  for (unsigned i = 0; i < numRefs; i++)
    arcToItem.Add(-1);
  // boot pseudo-items of the open image kept by the caller, in catalog order
  CUIntVector keptBootEntries;

  for (UInt32 i = 0; i < numItems; i++)
  {
    Int32 newData, newProps;
    UInt32 indexInArc;
    RINOK(callback->GetUpdateItemInfo(i, &newData, &newProps, &indexInArc))
    COutItem item;
    item.NewData = IntToBool(newData);
    item.IndexInClient = i;
    item.IndexInArc = indexInArc;
    if (!item.NewData && _stream && indexInArc >= numRefs)
    {
      // an El Torito image of the open image ("[BOOT]/..."): handled below
      if (indexInArc - numRefs >= _archive.BootEntries.Size())
        return E_NOTIMPL;
      keptBootEntries.Add(indexInArc - numRefs);
      continue;
    }
    if (IntToBool(newProps))
    {
      RINOK(GetItemFromProps([callback, i](PROPID pid, PROPVARIANT *v) { return callback->GetProperty(i, pid, v); },
          item, item.NewData))
    }
    else
    {
      if (!_stream || indexInArc >= numRefs)
        return E_NOTIMPL;
      RINOK(GetItemFromProps([this, indexInArc](PROPID pid, PROPVARIANT *v) { return GetProperty(indexInArc, pid, v); },
          item, false))
    }
    if (!item.NewData && !item.IsDir && item.SymLink.IsEmpty())
    {
      // old data (new properties or not): the size is the one of the image
      if (!_stream || indexInArc >= numRefs)
        return E_NOTIMPL;
      item.Size = _archive.Refs[indexInArc].TotalSize;
    }
    if (!item.NewData && (int)indexInArc == oldCatalogRef)
    {
      // the old catalog: its path is reused for the new one, unless -mcatalog says otherwise
      if (_catalogPath.IsEmpty())
        options.CatalogPath = item.Path;
      continue;
    }
    if (!item.NewData && indexInArc < numRefs)
      arcToItem[indexInArc] = (int)items.Size();
    items.Add(item);
  }

  // boot entries of the open image: each maps to the file of the tree that starts at
  // its sector, or stays as hidden data; the new -mboot / -mefi replace the old
  // entries of their platform
  FOR_VECTOR (k, keptBootEntries)
  {
    const unsigned bootIndex = keptBootEntries[k];
    const CBootInitialEntry &be = _archive.BootEntries[bootIndex];
    if (!be.Bootable)
      continue;
    if ((be.PlatformId == 0 && !_bootPath.IsEmpty()) || (be.PlatformId == 0xEF && !_efiPath.IsEmpty()))
      continue;
    COutBootEntry e;
    e.PlatformId = be.PlatformId;
    e.MediaType = be.BootMediaType;
    e.LoadSegment = be.LoadSegment;
    e.SystemType = be.SystemType;
    e.SectorCount = be.SectorCount;
    e.BootInfoTable = (be.PlatformId == 0) ? -1 : 0;
    const int ref = FindRefByBlock(be.LoadRBA);
    if (ref >= 0 && arcToItem[(unsigned)ref] >= 0)
      e.Path = items[(unsigned)arcToItem[(unsigned)ref]].Path;
    else
    {
      // not a file of the tree (or deleted): its data travels as a hidden item
      const UInt32 indexInArc = numRefs + bootIndex;
      COutItem item;
      item.NewData = false;
      item.IndexInArc = indexInArc;
      item.Hidden = true;
      RINOK(GetItemFromProps([this, indexInArc](PROPID pid, PROPVARIANT *v) { return GetProperty(indexInArc, pid, v); },
          item, false))
      item.Size = _archive.GetBootItemSize(bootIndex);
      if (item.Size == 0)
        continue;
      e.Path = item.Path;
      items.Add(item);
    }
    options.Boot.Add(e);
  }

  if (!_bootPath.IsEmpty())
  {
    // the BIOS image is the default entry: first
    COutBootEntry e;
    e.Path = _bootPath;
    e.PlatformId = 0;
    e.SectorCount = (UInt16)_bootLoad;
    e.BootInfoTable = _bootInfo;
    options.Boot.Insert(0, e);
  }
  if (!_efiPath.IsEmpty())
  {
    COutBootEntry e;
    e.Path = _efiPath;
    e.PlatformId = 0xEF;
    e.SectorCount = 0;   // the whole image
    e.BootInfoTable = 0;
    options.Boot.Add(e);
  }
  if (!_catalogPath.IsEmpty())
    options.CatalogPath = _catalogPath;

  // the system area: the MBR code given, or the one of the open image
  options.MbrCode = _mbrCode;
  bool oldMbr = false;
  if (_stream)
  {
    Byte sector[512];
    RINOK(InStream_SeekSet(_stream, 0))
    RINOK(ReadStream_FALSE(_stream, sector, sizeof(sector)))
    oldMbr = (sector[510] == 0x55 && sector[511] == 0xAA);
    if (oldMbr && options.MbrCode.Size() == 0)
      options.MbrCode.CopyFrom(sector, 432);
  }
  options.Hybrid = (_hybrid >= 0) ? (_hybrid != 0) : (!_stream || oldMbr || !_bootPath.IsEmpty() || !_efiPath.IsEmpty());

  options.VolumeId = _volumeId;
  if (options.VolumeId.IsEmpty() && _stream)
  {
    // a rewrite keeps the label of the image, unless -mvol says otherwise
    FOR_VECTOR (k, _archive.VolDescs)
    {
      const CVolumeDescriptor &vd = _archive.VolDescs[k];
      if (vd.IsJoliet())
        continue;
      AString id;
      for (unsigned j = 0; j < 32; j++)
        id.Add_Char((char)vd.VolumeId[j]);
      id.TrimRight();
      options.VolumeId = id;
      break;
    }
  }
  options.Joliet = _joliet;
  options.RockRidge = _rockRidge;
  {
    FILETIME ft;
    NTime::GetCurUtcFileTime(ft);
    options.Now = ((UInt64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  }

  CMyComPtr<IInArchiveGetStream> arcStream;
  if (_stream)
    arcStream = this;
  CDataSource source(callback, arcStream);
  return WriteIsoImage(outStream, items, options, source, callback);

  COM_TRY_END
}

}}
