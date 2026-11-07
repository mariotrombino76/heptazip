// IsoHandlerOut.cpp
// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
//
// IOutArchive of the ISO handler: collects the items from the update callback (and
// from the open image, when an existing one is rewritten) and hands them to the
// writer of IsoOut.cpp. An image is always written whole: there is no in-place update.

#include "StdAfx.h"

#include "../../../Common/ComTry.h"
#include "../../../Common/StringConvert.h"
#include "../../../Common/UTFConvert.h"

#include "../../../Windows/PropVariant.h"
#include "../../../Windows/TimeUtils.h"

#include "../../Common/MethodProps.h"

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

Z7_COM7F_IMF(CHandler::SetProperties(const wchar_t * const *names, const PROPVARIANT *values, UInt32 numProps))
{
  _volumeId.Empty();
  _joliet = true;
  _rockRidge = true;
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
  NItemName::ReplaceSlashes_OsToUnix(item.Path);
  while (!item.Path.IsEmpty() && item.Path.Back() == L'/')
    item.Path.DeleteBack();

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

Z7_COM7F_IMF(CHandler::UpdateItems(ISequentialOutStream *outStream, UInt32 numItems,
    IArchiveUpdateCallback *callback))
{
  COM_TRY_BEGIN

  if (!callback)
    return E_FAIL;
  if (_stream)
  {
    // a rewrite of an open image: only if it was read cleanly and has no boot entries
    // (El Torito is not written yet)
    if (!_archive.IsArc || _archive.UnexpectedEnd || _archive.HeadersError
        || _archive.BootEntries.Size() != 0)
      return E_NOTIMPL;
  }

  CObjectVector<COutItem> items;
  for (UInt32 i = 0; i < numItems; i++)
  {
    Int32 newData, newProps;
    UInt32 indexInArc;
    RINOK(callback->GetUpdateItemInfo(i, &newData, &newProps, &indexInArc))
    COutItem item;
    item.NewData = IntToBool(newData);
    item.IndexInClient = i;
    item.IndexInArc = indexInArc;
    if (IntToBool(newProps))
    {
      RINOK(GetItemFromProps([callback, i](PROPID pid, PROPVARIANT *v) { return callback->GetProperty(i, pid, v); },
          item, item.NewData))
    }
    else
    {
      if (!_stream || indexInArc >= (UInt32)_archive.Refs.Size())
        return E_NOTIMPL;
      RINOK(GetItemFromProps([this, indexInArc](PROPID pid, PROPVARIANT *v) { return GetProperty(indexInArc, pid, v); },
          item, false))
    }
    if (!item.NewData && !item.IsDir && item.SymLink.IsEmpty())
    {
      // old data (new properties or not): the size is the one of the image
      if (!_stream || indexInArc >= (UInt32)_archive.Refs.Size())
        return E_NOTIMPL;
      item.Size = _archive.Refs[indexInArc].TotalSize;
    }
    items.Add(item);
  }

  COutOptions options;
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
