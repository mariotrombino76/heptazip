// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
// ListJson.cpp
// Heptazip: JSON output of the "l" (list) command, switch -json.
// See docs/json-output.md.

#include "StdAfx.h"

#include "../../../Common/IntToString.h"
#include "../../../Common/MyCom.h"
#include "../../../Common/StdOutStream.h"

#include "../../../Windows/ErrorMsg.h"
#include "../../../Windows/FileFind.h"
#include "../../../Windows/PropVariant.h"
#include "../../../Windows/PropVariantConv.h"

#include "../Common/OpenArchive.h"
#include "../Common/PropIDUtils.h"

#include "ConsoleClose.h"
#include "ListJson.h"
#include "OpenCallbackConsole.h"

using namespace NWindows;
using namespace NCOM;

extern CStdOutStream *g_ErrStream;

// defined in List.cpp / ArchiveExtractCallback.cpp
int Find_FileName_InSortedVector(const UStringVector &fileName, const UString& name);
bool CensorNode_CheckPath(const NWildcard::CCensorNode &node, const CReadArcItem &item);

namespace {

// ---------------------------------------------------------------------------
// JSON text
// ---------------------------------------------------------------------------

void AppendHex4(AString &s, UInt32 v)
{
  const char *k = "0123456789abcdef";
  s += "\\u";
  s.Add_Char(k[(v >> 12) & 15]);
  s.Add_Char(k[(v >> 8) & 15]);
  s.Add_Char(k[(v >> 4) & 15]);
  s.Add_Char(k[v & 15]);
}

void AppendUtf8(AString &s, UInt32 c)
{
  if (c < 0x80)
    s.Add_Char((char)c);
  else if (c < 0x800)
  {
    s.Add_Char((char)(0xC0 | (c >> 6)));
    s.Add_Char((char)(0x80 | (c & 0x3F)));
  }
  else if (c < 0x10000)
  {
    s.Add_Char((char)(0xE0 | (c >> 12)));
    s.Add_Char((char)(0x80 | ((c >> 6) & 0x3F)));
    s.Add_Char((char)(0x80 | (c & 0x3F)));
  }
  else
  {
    s.Add_Char((char)(0xF0 | (c >> 18)));
    s.Add_Char((char)(0x80 | ((c >> 12) & 0x3F)));
    s.Add_Char((char)(0x80 | ((c >> 6) & 0x3F)));
    s.Add_Char((char)(0x80 | (c & 0x3F)));
  }
}

/* JSON string from wide chars. The output is always valid UTF-8:
   control chars and lone surrogates (possible in file names) are escaped. */
void AppendJsonString(AString &s, const wchar_t *p, unsigned len)
{
  s.Add_Char('"');
  for (unsigned i = 0; i < len; i++)
  {
    UInt32 c = (UInt32)p[i];
    if (sizeof(wchar_t) == 2 && c >= 0xD800 && c < 0xDC00 && i + 1 < len)
    {
      const UInt32 c2 = (UInt32)p[i + 1];
      if (c2 >= 0xDC00 && c2 < 0xE000)
      {
        c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
        i++;
      }
    }
    switch (c)
    {
      case '"':  s += "\\\""; continue;
      case '\\': s += "\\\\"; continue;
      case '\b': s += "\\b"; continue;
      case '\f': s += "\\f"; continue;
      case '\n': s += "\\n"; continue;
      case '\r': s += "\\r"; continue;
      case '\t': s += "\\t"; continue;
      default: break;
    }
    if (c < 0x20 || (c >= 0xD800 && c < 0xE000))
      AppendHex4(s, c);
    else if (c > 0x10FFFF)
      AppendHex4(s, 0xFFFD);
    else
      AppendUtf8(s, c);
  }
  s.Add_Char('"');
}

void AppendJsonString(AString &s, const UString &u)
{
  AppendJsonString(s, u.Ptr(), u.Len());
}

void AppendJsonString(AString &s, const char *a)
{
  UString u;
  for (; *a; a++)
    u.Add_Char(*a); // ASCII only (internal strings)
  AppendJsonString(s, u);
}


/* Pretty-printed JSON writer (2 spaces). Buffers text; Flush() writes it. */
class CJsonWriter
{
  CStdOutStream &_so;
  AString _out;
  unsigned _depth;
  bool _hasValue;   // the current object/array has a value already
  bool _afterKey;
  AString _open;    // '{' or '[' for each open level

  void NewLine()
  {
    _out.Add_LF();
    for (unsigned i = 0; i < _depth; i++)
      _out += "  ";
  }
  void BeforeValue()
  {
    if (_afterKey)
    {
      _afterKey = false;
      return;
    }
    if (_hasValue)
      _out.Add_Char(',');
    if (_depth != 0)
      NewLine();
  }
  void AfterValue() { _hasValue = true; }

public:
  CJsonWriter(CStdOutStream &so): _so(so), _depth(0), _hasValue(false), _afterKey(false) {}

  void BeginObject() { BeforeValue(); _out.Add_Char('{'); _open.Add_Char('{'); _depth++; _hasValue = false; }
  void BeginArray()  { BeforeValue(); _out.Add_Char('['); _open.Add_Char('['); _depth++; _hasValue = false; }
  void EndObject() { _depth--; _open.DeleteBack(); if (_hasValue) NewLine(); _out.Add_Char('}'); AfterValue(); }
  void EndArray()  { _depth--; _open.DeleteBack(); if (_hasValue) NewLine(); _out.Add_Char(']'); AfterValue(); }
  // an error in the middle of the document: what was written stays valid JSON
  void CloseAll()
  {
    if (_afterKey)
      Raw("null");
    while (_depth != 0)
    {
      if (_open.Back() == '{')
        EndObject();
      else
        EndArray();
    }
  }

  void Key(const char *k)
  {
    BeforeValue();
    AppendJsonString(_out, k);
    _out += ": ";
    _afterKey = true;
    _hasValue = true;
  }

  void String(const UString &v) { BeforeValue(); AppendJsonString(_out, v); AfterValue(); }
  void String(const char *v)    { BeforeValue(); AppendJsonString(_out, v); AfterValue(); }
  void Raw(const char *v)       { BeforeValue(); _out += v; AfterValue(); }
  void Bool(bool v)             { Raw(v ? "true" : "false"); }
  void Number(UInt64 v)
  {
    char s[32];
    ConvertUInt64ToString(v, s);
    Raw(s);
  }
  void Number(Int64 v)
  {
    char s[32];
    ConvertInt64ToString(v, s);
    Raw(s);
  }

  void Flush()
  {
    _so << _out;
    _out.Empty();
  }
  void Finish()
  {
    _out.Add_LF();
    Flush();
    _so.Flush();
  }
};


// ---------------------------------------------------------------------------
// Property names and values
// ---------------------------------------------------------------------------

/* -slt name -> lowerCamelCase key: "Packed Size" -> "packedSize", "CRC" -> "crc". */
void MakeKey(PROPID propID, const wchar_t *name, AString &key)
{
  AString nameA;
  UString nameU;
  List_GetPropName(propID, name, nameA, nameU);
  UString src;
  if (!nameA.IsEmpty())
  {
    for (unsigned i = 0; i < nameA.Len(); i++)
      src.Add_Char(nameA[i]);
  }
  else
    src = nameU;

  key.Empty();
  bool newWord = false;
  for (unsigned i = 0; i < src.Len(); i++)
  {
    const wchar_t c = src[i];
    const bool isAlnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!isAlnum)
    {
      newWord = true;
      continue;
    }
    char ch = (char)c;
    if (key.IsEmpty())
      ch = (char)((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    else if (newWord)
      ch = (char)((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
    else
    {
      // inside a word: keep the case, except for all-caps words ("CRC" -> "crc")
      const wchar_t prev = src[i - 1];
      if ((prev >= 'A' && prev <= 'Z') && (ch >= 'A' && ch <= 'Z'))
        ch = (char)(ch - 'A' + 'a');
    }
    key.Add_Char(ch);
    newWord = false;
  }
  if (key.IsEmpty())
  {
    key = "prop";
    key.Add_UInt32(propID);
  }
}


/* Days since 1970-01-01 -> civil date (H. Hinnant's algorithm). */
void CivilFromDays(Int64 z, Int64 &y, unsigned &m, unsigned &d)
{
  z += 719468;
  const Int64 era = (z >= 0 ? z : z - 146096) / 146097;
  const UInt64 doe = (UInt64)(z - era * 146097);
  const UInt64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (Int64)yoe + era * 400;
  const UInt64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const UInt64 mp = (5 * doy + 2) / 153;
  d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
  m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2)
    y++;
}

void Append2(AString &s, unsigned v)
{
  s.Add_Char((char)('0' + v / 10));
  s.Add_Char((char)('0' + v % 10));
}

/* FILETIME (UTC, 100 ns since 1601) -> ISO 8601 "2026-10-30T12:34:56.1234567Z",
   with the number of fractional digits of the precision of the property
   (see PropID.h: wReserved1 = precision, wReserved2 = ns % 100). */
void FileTimeToIso8601(const PROPVARIANT &prop, AString &s)
{
  const UInt64 ft = ((UInt64)prop.filetime.dwHighDateTime << 32) | prop.filetime.dwLowDateTime;
  const unsigned prec = prop.wReserved1;
  unsigned ns100 = prop.wReserved2;
  unsigned numDigits;
  if (prec == 0)
    numDigits = 7;
  else if (prec == 3)  // high precision: 1 ns
    numDigits = 9;
  else if (prec >= 16 && prec <= 16 + 9)
    numDigits = prec - 16;
  else  // Unix, DOS, reserved levels
    numDigits = 0;
  if (numDigits < 8 || ns100 >= 100)
    ns100 = 0;

  const UInt64 secs = ft / 10000000;
  const UInt32 frac7 = (UInt32)(ft % 10000000);
  const Int64 kDays_1601_to_1970 = 134774;
  Int64 y;
  unsigned mon, day;
  CivilFromDays((Int64)(secs / 86400) - kDays_1601_to_1970, y, mon, day);
  const unsigned rem = (unsigned)(secs % 86400);

  s.Empty();
  char ys[32];
  ConvertInt64ToString(y, ys);
  s += ys;
  s.Add_Minus(); Append2(s, mon);
  s.Add_Minus(); Append2(s, day);
  s.Add_Char('T'); Append2(s, rem / 3600);
  s.Add_Char(':'); Append2(s, (rem / 60) % 60);
  s.Add_Char(':'); Append2(s, rem % 60);
  if (numDigits != 0)
  {
    const UInt32 frac9 = frac7 * 100 + ns100;
    char digits[10];
    UInt32 v = frac9;
    for (int i = 8; i >= 0; i--)
    {
      digits[i] = (char)('0' + v % 10);
      v /= 10;
    }
    s.Add_Dot();
    for (unsigned i = 0; i < numDigits; i++)
      s.Add_Char(digits[i]);
  }
  s.Add_Char('Z');
}


bool GetIntegerValue(const PROPVARIANT &prop, bool &isSigned, UInt64 &u, Int64 &i)
{
  isSigned = false;
  switch (prop.vt)
  {
    case VT_UI1: u = prop.bVal; return true;
    case VT_UI2: u = prop.uiVal; return true;
    case VT_UI4: u = prop.ulVal; return true;
    case VT_UI8: u = prop.uhVal.QuadPart; return true;
    case VT_I2:  isSigned = true; i = prop.iVal; return true;
    case VT_I4:  isSigned = true; i = prop.lVal; return true;
    case VT_I8:  isSigned = true; i = prop.hVal.QuadPart; return true;
    default: return false;
  }
}

void WriteProp(CJsonWriter &w, const AString &key, PROPID propID, const PROPVARIANT &prop);

/* Writes "key": value. Integers are JSON numbers when -slt prints them as plain
   decimal numbers; otherwise (CRC in hex, attributes, ...) the -slt text is kept. */
void WriteProp(CJsonWriter &w, PROPID propID, const wchar_t *name, const PROPVARIANT &prop)
{
  if (prop.vt == VT_EMPTY)
    return;
  AString key;
  MakeKey(propID, name, key);
  WriteProp(w, key, propID, prop);
}

/* As above, with the key already made (the keys of the item properties are
   made once per archive, not for every item). */
void WriteProp(CJsonWriter &w, const AString &key, PROPID propID, const PROPVARIANT &prop)
{
  if (prop.vt == VT_EMPTY)
    return;

  if (prop.vt == VT_BOOL)
  {
    w.Key(key);
    w.Bool(prop.boolVal != VARIANT_FALSE);
    return;
  }
  if (prop.vt == VT_FILETIME)
  {
    // a zero timestamp means "not defined" (as in -slt: CListFileTimeDef::IsZero())
    if (prop.filetime.dwLowDateTime == 0 && prop.filetime.dwHighDateTime == 0
        && (prop.wReserved1 == 0 || prop.wReserved2 == 0))
      return;
    AString t;
    FileTimeToIso8601(prop, t);
    w.Key(key);
    w.String(t);
    return;
  }
  if (prop.vt == VT_BSTR)
  {
    UString u;
    u.SetFromBstr(prop.bstrVal);
    w.Key(key);
    w.String(u);
    return;
  }

  UString text;
  ConvertPropertyToString2(text, prop, propID, 9);
  bool isSigned;
  UInt64 u = 0;
  Int64 i = 0;
  if (GetIntegerValue(prop, isSigned, u, i))
  {
    char dec[32];
    if (isSigned)
      ConvertInt64ToString(i, dec);
    else
      ConvertUInt64ToString(u, dec);
    if (text.IsEqualTo(dec))
    {
      w.Key(key);
      if (isSigned)
        w.Number(i);
      else
        w.Number(u);
      return;
    }
  }
  if (text.IsEmpty())
    return;
  w.Key(key);
  w.String(text);
}


// error flags of CArcErrorInfo (PropID.h: kpv_ErrorFlags_*)
static const char * const k_ErrorFlagNames[] =
{
    "isNotArc"
  , "headersError"
  , "encryptedHeadersError"
  , "unavailableStart"
  , "unconfirmedStart"
  , "unexpectedEnd"
  , "dataAfterEnd"
  , "unsupportedMethod"
  , "unsupportedFeature"
  , "dataError"
  , "crcError"
};

void WriteFlags(CJsonWriter &w, const char *key, UInt32 flags)
{
  if (flags == 0)
    return;
  w.Key(key);
  w.BeginArray();
  for (unsigned i = 0; i < 32; i++)
  {
    if (((flags >> i) & 1) == 0)
      continue;
    if (i < Z7_ARRAY_SIZE(k_ErrorFlagNames))
      w.String(k_ErrorFlagNames[i]);
    else
    {
      AString s ("flag");
      s.Add_UInt32(i);
      w.String(s);
    }
  }
  w.EndArray();
}


HRESULT WriteArcLevel(CJsonWriter &w, const CCodecs *codecs, const CArc &arc)
{
  const CArcErrorInfo &er = arc.ErrorInfo;
  w.BeginObject();
  w.Key("path");
  w.String(arc.Path);
  w.Key("type");
  w.String(codecs->GetFormatNamePtr(arc.FormatIndex));
  const Int64 offset = arc.GetGlobalOffset();
  if (offset != 0)
  {
    w.Key("offset");
    w.Number(offset);
  }
  if (er.ErrorFormatIndex >= 0 && er.ErrorFormatIndex != arc.FormatIndex)
  {
    w.Key("openWarningType");
    w.String(codecs->GetFormatNamePtr(er.ErrorFormatIndex));
  }
  WriteFlags(w, "errors", er.GetErrorFlags());
  if (!er.ErrorMessage.IsEmpty())
  {
    w.Key("errorMessage");
    w.String(er.ErrorMessage);
  }
  WriteFlags(w, "warnings", er.GetWarningFlags());
  if (!er.WarningMessage.IsEmpty())
  {
    w.Key("warningMessage");
    w.String(er.WarningMessage);
  }
  if (er.TailSize != 0)
  {
    w.Key("tailSize");
    w.Number(er.TailSize);
  }

  IInArchive *archive = arc.Archive;
  {
    CPropVariant prop;
    RINOK(archive->GetArchiveProperty(kpidPhySize, &prop))
    WriteProp(w, kpidPhySize, NULL, prop);
  }
  UInt32 numProps;
  RINOK(archive->GetNumberOfArchiveProperties(&numProps))
  for (UInt32 j = 0; j < numProps; j++)
  {
    CMyComBSTR name;
    PROPID propID;
    VARTYPE vt;
    RINOK(archive->GetArchivePropertyInfo(j, &name, &propID, &vt))
    if (propID == kpidPhySize)
      continue;
    CPropVariant prop;
    RINOK(archive->GetArchiveProperty(propID, &prop))
    WriteProp(w, propID, name, prop);
  }
  w.EndObject();
  return S_OK;
}


void WriteOpenError(CJsonWriter &w, const UString &path, const UString &message)
{
  w.BeginObject();
  w.Key("path");
  w.String(path);
  w.Key("message");
  w.String(message);
  w.EndObject();
}

} // namespace


HRESULT ListArchives_Json(
    const CListOptions &listOptions,
    CCodecs *codecs,
    const CObjectVector<COpenType> &types,
    const CIntVector &excludedFormats,
    bool stdInMode,
    UStringVector &arcPaths, UStringVector &arcPathsFull,
    bool processAltStreams, bool showAltStreams,
    const NWildcard::CCensorNode &wildcardCensor,
  #ifndef Z7_NO_CRYPTO
    bool &passwordEnabled, UString &password,
  #endif
  #ifndef Z7_SFX
    const CObjectVector<CProperty> *props,
  #endif
    UInt64 &numErrors,
    UInt64 &numWarnings)
{
  const bool allFilesAreAllowed = wildcardCensor.AreAllAllowed();
  numErrors = 0;
  numWarnings = 0;

  CJsonWriter w(g_StdOut);
  CObjectVector<UString> openErrorPaths;
  CObjectVector<UString> openErrorMessages;

  CBoolArr skipArcs(arcPaths.Size());
  unsigned arcIndex;
  for (arcIndex = 0; arcIndex < arcPaths.Size(); arcIndex++)
    skipArcs[arcIndex] = false;

  HRESULT lastError = 0;

  w.BeginObject();
  // an early return (error, Ctrl-C) leaves a complete document
  struct CCloser
  {
    CJsonWriter &W;
    bool Armed;
    CCloser(CJsonWriter &w): W(w), Armed(true) {}
    ~CCloser() { if (Armed) { W.CloseAll(); W.Finish(); } }
  } closer(w);
  w.Key("archives");
  w.BeginArray();

  for (arcIndex = 0; arcIndex < arcPaths.Size(); arcIndex++)
  {
    if (skipArcs[arcIndex])
      continue;
    const UString &arcPath = arcPaths[arcIndex];

    if (!stdInMode)
    {
      NFile::NFind::CFileInfo fi;
      if (!fi.Find_FollowLink(us2fs(arcPath)))
      {
        DWORD errorCode = GetLastError();
        if (errorCode == 0)
          errorCode = ERROR_FILE_NOT_FOUND;
        lastError = HRESULT_FROM_WIN32(errorCode);
        openErrorPaths.Add(arcPath);
        openErrorMessages.Add(NError::MyFormatMessage(errorCode));
        numErrors++;
        continue;
      }
      if (fi.IsDir())
      {
        openErrorPaths.Add(arcPath);
        openErrorMessages.Add(UString("is not a file"));
        numErrors++;
        continue;
      }
    }

    CArchiveLink arcLink;
    COpenCallbackConsole openCallback;
    // messages of the open callback go to stderr: stdout contains only JSON
    openCallback.Init(g_ErrStream, g_ErrStream, NULL, true);
    #ifndef Z7_NO_CRYPTO
    openCallback.PasswordIsDefined = passwordEnabled;
    openCallback.Password = password;
    #endif

    COpenOptions options;
    #ifndef Z7_SFX
    options.props = props;
    #endif
    options.codecs = codecs;
    options.types = &types;
    options.excludedFormats = &excludedFormats;
    options.stdInMode = stdInMode;
    options.stream = NULL;
    options.filePath = arcPath;

    const HRESULT result = arcLink.Open_Strict(options, &openCallback);
    if (result != S_OK)
    {
      if (result == E_ABORT)
        return result;
      if (result != S_FALSE)
        lastError = result;
      UString message;
      if (result == S_FALSE)
      {
        #ifndef Z7_NO_CRYPTO
        if (arcLink.PasswordWasAsked)
          message = "Cannot open encrypted archive. Wrong password?";
        else
        #endif
          message = "Cannot open the file as archive";
      }
      else if (result == E_OUTOFMEMORY)
        message = "Can't allocate required memory";
      else
        message = NError::MyFormatMessage(result);
      openErrorPaths.Add(arcPath);
      openErrorMessages.Add(message);
      numErrors++;
      continue;
    }

    FOR_VECTOR (r, arcLink.Arcs)
    {
      const CArcErrorInfo &arcErr = arcLink.Arcs[r].ErrorInfo;
      if (!arcErr.WarningMessage.IsEmpty())
        numWarnings++;
      if (arcErr.AreThereWarnings())
        numWarnings++;
      if (arcErr.ErrorFormatIndex >= 0)
        numWarnings++;
      if (arcErr.AreThereErrors())
        numErrors++;
      if (!arcErr.ErrorMessage.IsEmpty())
        numErrors++;
    }

    if (!stdInMode)
    {
      FOR_VECTOR (v, arcLink.VolumePaths)
      {
        const int index = Find_FileName_InSortedVector(arcPathsFull, arcLink.VolumePaths[v]);
        if (index >= 0 && (unsigned)index > arcIndex)
          skipArcs[(unsigned)index] = true;
      }
    }

    w.BeginObject();
    w.Key("path");
    w.String(arcPath);
    if (!arcLink.VolumePaths.IsEmpty())
    {
      w.Key("volumes");
      w.BeginArray();
      FOR_VECTOR (v, arcLink.VolumePaths)
        w.String(arcLink.VolumePaths[v]);
      w.EndArray();
    }
    // nested archives (e.g. tar in gz): one entry per level, the last one is listed
    w.Key("levels");
    w.BeginArray();
    FOR_VECTOR (r, arcLink.Arcs)
    {
      RINOK(WriteArcLevel(w, codecs, arcLink.Arcs[r]))
    }
    w.EndArray();

    const CArc &arc = arcLink.Arcs.Back();
    IInArchive *archive = arc.Archive;

    // properties of the items: the same list as -slt (raw properties are not included)
    UInt32 numProps;
    RINOK(archive->GetNumberOfProperties(&numProps))
    CRecordVector<PROPID> propIDs;
    AStringVector propKeys;   // the JSON keys, made once for all the items
    for (UInt32 j = 0; j < numProps; j++)
    {
      CMyComBSTR name;
      PROPID propID;
      VARTYPE vt;
      RINOK(archive->GetPropertyInfo(j, &name, &propID, &vt))
      if (propID == kpidPath || propID == kpidIsDir)
        continue; // written as "path" and "isDir"
      propIDs.Add(propID);
      AString key;
      MakeKey(propID, name ? (const wchar_t *)name : NULL, key);
      propKeys.Add(key);
    }

    w.Key("items");
    w.BeginArray();

    UInt32 numItems;
    RINOK(archive->GetNumberOfItems(&numItems))
    CReadArcItem item;
    UStringVector pathParts;
    UString filePath;

    for (UInt32 i = 0; i < numItems; i++)
    {
      if (NConsoleClose::TestBreakSignal())
        return E_ABORT;

      HRESULT res = arc.GetItem_Path2(i, filePath);
      if (stdInMode && res == E_INVALIDARG)
        break;
      RINOK(res)

      if (arc.Ask_Aux)
      {
        bool isAux;
        RINOK(Archive_IsItem_Aux(archive, i, isAux))
        if (isAux)
          continue;
      }

      bool isAltStream = false;
      if (arc.Ask_AltStream)
      {
        RINOK(Archive_IsItem_AltStream(archive, i, isAltStream))
        if (isAltStream && !processAltStreams)
          continue;
      }

      bool isDir;
      RINOK(Archive_IsItem_Dir(archive, i, isDir))
      if (isDir ? listOptions.ExcludeDirItems : listOptions.ExcludeFileItems)
        continue;

      if (!allFilesAreAllowed)
      {
        if (isAltStream)
        {
          RINOK(arc.GetItem(i, item))
          if (!CensorNode_CheckPath(wildcardCensor, item))
            continue;
        }
        else
        {
          SplitPathToParts(filePath, pathParts);
          bool include;
          if (!wildcardCensor.CheckPathVect(pathParts, !isDir, include))
            continue;
          if (!include)
            continue;
        }
      }

      if (isAltStream && !showAltStreams)
        continue;

      w.BeginObject();
      w.Key("path");
      w.String(filePath);
      w.Key("isDir");
      w.Bool(isDir);
      FOR_VECTOR (k, propIDs)
      {
        CPropVariant prop;
        RINOK(archive->GetProperty(i, propIDs[k], &prop))
        WriteProp(w, propKeys[k], propIDs[k], prop);
      }
      w.EndObject();
      w.Flush();
    }
    w.EndArray();  // items
    w.EndObject(); // archive
    w.Flush();
  }

  w.EndArray(); // archives

  w.Key("errors");
  w.BeginArray();
  FOR_VECTOR (e, openErrorPaths)
    WriteOpenError(w, openErrorPaths[e], openErrorMessages[e]);
  w.EndArray();

  w.EndObject();
  closer.Armed = false;
  w.Finish();

  if (numErrors == 1 && lastError != 0)
    return lastError;
  return S_OK;
}
