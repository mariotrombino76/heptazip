// IsoOut.cpp
// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
//
// ISO 9660 (ECMA-119) writer with Joliet, Rock Ridge (SUSP 1.12 / RRIP 1.12) and
// El Torito boot entries. Design notes: docs/design/0019-scrittura-iso.md (the
// file system), docs/design/0020-iso-avviabili.md (boot entries, system area).
//
// Image layout (sectors of 2048 bytes):
//   0..15      system area: zeros, or an MBR with a partition table (isohybrid)
//   16         primary volume descriptor
//   17         El Torito boot record volume descriptor, if there are boot entries
//   next       supplementary volume descriptor (Joliet), if enabled
//   next       volume descriptor set terminator
//   next       path tables: L and M of the 9660 tree, then L and M of the Joliet tree
//   next       directory extents of the 9660 tree (each followed by its Rock Ridge
//              continuation area, if any), then of the Joliet tree
//   next       file data, in the order of the items, each at a sector boundary; the
//              boot catalog and the hidden boot images are files like the others
// Everything before the file data is built in memory, so the output is written once,
// sequentially, with the sizes of the files known in advance.

#include "StdAfx.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../../../C/CpuArch.h"
#include "../../../../C/7zVersion.h"

#include "../../../Common/ComTry.h"
#include "../../../Common/MyLinux.h"
#include "../../../Common/StringConvert.h"
#include "../../../Common/UTFConvert.h"

#include "../../Common/LimitedStreams.h"
#include "../../Common/ProgressUtils.h"
#include "../../Common/StreamUtils.h"

#include "../../Compress/CopyCoder.h"

#include "IsoOut.h"

namespace NArchive {
namespace NIso {

static const unsigned kSector = 2048;
static const unsigned kMaxRecord = 255;
// largest extent of one directory record: 4 GiB - one sector (ECMA-119: 32-bit length)
static const UInt32 kMaxExtentSize = 0xFFFFF800;
static const unsigned kMaxId9660 = 30;      // file name + '.' + extension, without ";1"
static const unsigned kMaxDirId9660 = 31;
static const unsigned kMaxIdJoliet = 103;   // UCS-2 characters ("joliet-long" of mkisofs)
static const unsigned kMaxBootEntries = 30;  // one sector of catalog: validation + default + 30 (header + entry)
static const unsigned kMbrCodeSize = 432;    // bytes of boot code before the isohybrid fields (0x1B0)
// a boot image is read in memory to patch its boot info table: not beyond this size
static const UInt64 kMaxBootImageInMemory = (UInt64)64 << 20;

// ---------- little helpers

static void SetBe16(Byte *p, UInt32 v)
{
  p[0] = (Byte)(v >> 8);
  p[1] = (Byte)v;
}

static void SetBoth16(Byte *p, UInt32 v)
{
  SetUi16(p, (UInt16)v)
  SetBe16(p + 2, v);
}

static void SetBoth32(Byte *p, UInt32 v)
{
  SetUi32(p, v)
  SetBe32(p + 4, v)
}

static UInt32 SectorsOf(UInt64 size)
{
  return (UInt32)((size + kSector - 1) / kSector);
}

static void PadToSector(CByteBuffer &buf)
{
  const size_t rem = buf.Size() % kSector;
  if (rem != 0)
  {
    const size_t old = buf.Size();
    buf.ChangeSize_KeepData(old + kSector - rem, old);
    memset(buf + old, 0, kSector - rem);
  }
}

static void Append(CByteBuffer &buf, const void *data, size_t size)
{
  const size_t old = buf.Size();
  buf.ChangeSize_KeepData(old + size, old);
  memcpy(buf + old, data, size);
}

// a d-character string padded with spaces
static void SetPadded(Byte *dest, unsigned size, const AString &s)
{
  memset(dest, ' ', size);
  const unsigned n = s.Len() < size ? s.Len() : size;
  memcpy(dest, s.Ptr(), n);
}

// a Joliet (UCS-2 big endian) string padded with spaces
static void SetPaddedUcs2(Byte *dest, unsigned size, const UString &s)
{
  for (unsigned i = 0; i < size / 2; i++)
  {
    const wchar_t c = i < s.Len() ? s[i] : L' ';
    SetBe16(dest + i * 2, (UInt16)c);
  }
}

// ---------- dates

struct CCivil
{
  unsigned Year, Month, Day, Hour, Minute, Second, Hundredths;
};

// FILETIME (100 ns since 1601) to civil time, UTC
static void FileTimeToCivil(UInt64 ft, CCivil &c)
{
  const UInt64 secs = ft / 10000000;
  c.Hundredths = (unsigned)((ft % 10000000) / 100000);
  const Int64 days1970 = (Int64)(secs / 86400) - 134774;   // days from 1601 to 1970
  const unsigned rem = (unsigned)(secs % 86400);
  c.Hour = rem / 3600;
  c.Minute = (rem / 60) % 60;
  c.Second = rem % 60;
  // civil_from_days (H. Hinnant)
  const Int64 z = days1970 + 719468;
  const Int64 era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  c.Day = doy - (153 * mp + 2) / 5 + 1;
  c.Month = mp < 10 ? mp + 3 : mp - 9;
  c.Year = (unsigned)(yoe + era * 400 + (c.Month <= 2 ? 1 : 0));
}

// 7-byte "recording date and time" of the directory records (ECMA-119 9.1.5)
static void SetRecordingDate(Byte *p, UInt64 ft)
{
  memset(p, 0, 7);
  if (ft == 0)
    return;
  CCivil c;
  FileTimeToCivil(ft, c);
  if (c.Year < 1900 || c.Year > 2155)
    return;
  p[0] = (Byte)(c.Year - 1900);
  p[1] = (Byte)c.Month;
  p[2] = (Byte)c.Day;
  p[3] = (Byte)c.Hour;
  p[4] = (Byte)c.Minute;
  p[5] = (Byte)c.Second;
  p[6] = 0;   // GMT offset: times are recorded in UTC
}

// 17-byte date of the volume descriptors (ECMA-119 8.4.26.1)
static void SetDescriptorDate(Byte *p, UInt64 ft)
{
  if (ft == 0)
  {
    memset(p, '0', 16);
    p[16] = 0;
    return;
  }
  CCivil c;
  FileTimeToCivil(ft, c);
  char s[24];
  snprintf(s, sizeof(s), "%04u%02u%02u%02u%02u%02u%02u",
      c.Year % 10000, c.Month, c.Day, c.Hour, c.Minute, c.Second, c.Hundredths);
  memcpy(p, s, 16);
  p[16] = 0;
}

// ---------- the tree

struct CNode
{
  UString Name;         // real name (last path component)
  AString Id;           // ISO 9660 identifier (files: "NAME.EXT", the ";1" is added when written)
  UString IdJoliet;     // Joliet identifier
  AString SymLink;      // UTF-8 target
  UInt64 Size;
  UInt64 MTime;
  UInt32 Mode;
  int Item;             // index into the items, -1 for an implicit directory or the boot catalog
  unsigned Parent;
  bool IsDir;
  bool Hidden;          // a file outside the trees (boot image not present as a file): data only

  std::vector<unsigned> Children;      // sorted by Id (9660 order)
  std::vector<unsigned> ChildrenJoliet;   // sorted by IdJoliet

  // layout, in sectors
  UInt32 DataExtent;    // files: first sector of the data
  UInt32 DirExtent, DirSize;              // directories, 9660 tree (size in bytes)
  UInt32 CeExtent, CeSize;                // Rock Ridge continuation area of the 9660 directory
  UInt32 DirExtentJoliet, DirSizeJoliet;
  UInt32 DirNum, DirNumJoliet;            // path table numbers (1 = root)

  CNode(): Size(0), MTime(0), Mode(0), Item(-1), Parent(0), IsDir(false), Hidden(false),
      DataExtent(0), DirExtent(0), DirSize(0), CeExtent(0), CeSize(0),
      DirExtentJoliet(0), DirSizeJoliet(0), DirNum(0), DirNumJoliet(0) {}
};

static void UpperAscii(AString &s)
{
  for (unsigned i = 0; i < s.Len(); i++)
    if (s[i] >= 'a' && s[i] <= 'z')
      s.ReplaceOneCharAtPos(i, (char)(s[i] - 'a' + 'A'));
}

static bool IsDChar(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static AString ToDChars(const UString &s, unsigned maxLen)
{
  AString r;
  for (unsigned i = 0; i < s.Len() && r.Len() < maxLen; i++)
  {
    wchar_t c = s[i];
    if (c >= 'a' && c <= 'z')
      c = (wchar_t)(c - 'a' + 'A');
    r.Add_Char(c < 0x80 && IsDChar((char)c) ? (char)c : '_');
  }
  return r;
}

// ISO 9660 identifier of a name: files "BASE.EXT" (level 2: up to 30 characters, the
// extension keeps at most 3), directories up to 31 characters without dots.
static AString MakeId9660(const UString &name, bool isDir)
{
  if (isDir)
  {
    AString r = ToDChars(name, kMaxDirId9660);
    if (r.IsEmpty())
      r = "_";
    return r;
  }
  const int dot = name.ReverseFind_Dot();
  UString base, ext;
  if (dot > 0)
  {
    base = name.Left((unsigned)dot);
    ext = name.Ptr((unsigned)dot + 1);
  }
  else
    base = name;
  AString e = ToDChars(ext, 3);
  AString b = ToDChars(base, kMaxId9660 - 1 - e.Len());
  if (b.IsEmpty())
    b = "_";
  b.Add_Dot();
  b += e;
  return b;
}

static bool IsJolietChar(wchar_t c)
{
  if (c < 0x20)
    return false;
  switch (c)
  {
    case L'*': case L'/': case L':': case L';': case L'?': case L'\\':
      return false;
  }
  // characters outside the BMP arrive as surrogate pairs (7-Zip keeps UTF-16 units in
  // UString on every platform) and are written as such: Windows reads them
  return true;
}

static UString MakeIdJoliet(const UString &name)
{
  UString r;
  for (unsigned i = 0; i < name.Len(); i++)
  {
    const wchar_t c = name[i];
    r += IsJolietChar(c) ? c : L'_';
  }
  if (r.Len() > kMaxIdJoliet)
  {
    // keep the extension, shorten the base
    const int dot = r.ReverseFind_Dot();
    if (dot > 0 && r.Len() - (unsigned)dot <= 8)
    {
      const UString ext = r.Ptr((unsigned)dot);
      r.DeleteFrom(kMaxIdJoliet - ext.Len());
      r += ext;
    }
    else
      r.DeleteFrom(kMaxIdJoliet);
  }
  if (r.IsEmpty())
    r = L"_";
  return r;
}

// makes an identifier unique in its directory by appending _1, _2, ...
template <class TString, class TSet>
static void MakeUnique(TString &id, TSet &used, unsigned maxLen, bool keepExt)
{
  typedef typename TSet::key_type key_t;
  if (used.insert(key_t(id.Ptr(), id.Len())).second)
    return;
  TString base = id, ext;
  if (keepExt)
  {
    const int dot = id.ReverseFind_Dot();
    if (dot >= 0)
    {
      base = id.Left((unsigned)dot);
      ext = id.Ptr((unsigned)dot);
    }
  }
  for (unsigned n = 1;; n++)
  {
    TString suffix;
    suffix.Add_Char('_');
    suffix.Add_UInt32(n);
    TString b = base;
    const unsigned room = maxLen - ext.Len() - suffix.Len();
    if (b.Len() > room)
      b.DeleteFrom(room);
    TString cand = b;
    cand += suffix;
    cand += ext;
    if (used.insert(key_t(cand.Ptr(), cand.Len())).second)
    {
      id = cand;
      return;
    }
  }
}

class CTree
{
public:
  std::vector<CNode> Nodes;   // 0 = root
  bool Joliet;
  bool RockRidge;
  int CatalogNode;            // the boot catalog file, -1 if none

  CTree(): Joliet(true), RockRidge(true), CatalogNode(-1) {}

  // `catalogPath`: the boot catalog file to add (generated content), NULL for none
  HRESULT Build(const CObjectVector<COutItem> &items, UInt64 now, const UString *catalogPath);
  // the node of a path of the tree or of a hidden item, -1 if absent
  int Find(const UString &path) const;

private:
  std::map<std::pair<unsigned, std::wstring>, unsigned> _byName;

  HRESULT Insert(const UString &path, bool isDir, UInt64 now, unsigned &node, bool &existed);

  unsigned AddChild(unsigned parent, const UString &name, bool isDir, UInt64 now)
  {
    const unsigned index = (unsigned)Nodes.size();
    Nodes.emplace_back();
    CNode &n = Nodes.back();
    n.Name = name;
    n.Parent = parent;
    n.IsDir = isDir;
    n.MTime = now;
    Nodes[parent].Children.push_back(index);
    _byName[std::make_pair(parent, std::wstring(name.Ptr(), name.Len()))] = index;
    return index;
  }

  int FindChild(unsigned parent, const UString &name) const
  {
    const auto it = _byName.find(std::make_pair(parent, std::wstring(name.Ptr(), name.Len())));
    return it == _byName.end() ? -1 : (int)it->second;
  }
};

// walks `path` creating the missing directories; `node` is the last component (a
// directory if `isDir`, else a file), `existed` tells whether it was already there.
// An empty path (or ".") is the root.
HRESULT CTree::Insert(const UString &pathIn, bool isDir, UInt64 now, unsigned &node, bool &existed)
{
  unsigned cur = 0;
  existed = true;
  UString path = pathIn;
  path.Replace(L'\\', L'/');
  unsigned start = 0;
  while (start < path.Len())
  {
    int end = path.Find(L'/', start);
    if (end < 0)
      end = (int)path.Len();
    const UString comp = path.Mid(start, (unsigned)end - start);
    start = (unsigned)end + 1;
    if (comp.IsEmpty() || comp == L".")
      continue;
    if (comp == L"..")
      return E_INVALIDARG;
    const bool last = (start >= path.Len());
    const int found = FindChild(cur, comp);
    if (found >= 0)
    {
      cur = (unsigned)found;
      if (!last && !Nodes[cur].IsDir)
        return E_INVALIDARG;   // a file used as a directory
      existed = true;
      continue;
    }
    cur = AddChild(cur, comp, !last || isDir, now);
    existed = false;
  }
  node = cur;
  return S_OK;
}

int CTree::Find(const UString &pathIn) const
{
  UString path = pathIn;
  path.Replace(L'\\', L'/');
  for (unsigned i = 1; i < Nodes.size(); i++)
    if (Nodes[i].Hidden && Nodes[i].Name == path)
      return (int)i;
  unsigned cur = 0;
  unsigned start = 0;
  while (start < path.Len())
  {
    int end = path.Find(L'/', start);
    if (end < 0)
      end = (int)path.Len();
    const UString comp = path.Mid(start, (unsigned)end - start);
    start = (unsigned)end + 1;
    if (comp.IsEmpty() || comp == L".")
      continue;
    const int found = FindChild(cur, comp);
    if (found < 0)
      return -1;
    cur = (unsigned)found;
  }
  return (int)cur;
}

HRESULT CTree::Build(const CObjectVector<COutItem> &items, UInt64 now, const UString *catalogPath)
{
  Nodes.clear();
  _byName.clear();
  CatalogNode = -1;
  Nodes.emplace_back();
  Nodes[0].IsDir = true;
  Nodes[0].MTime = now;
  Nodes[0].Mode = MY_LIN_S_IFDIR | 0755;

  FOR_VECTOR (i, items)
  {
    const COutItem &item = items[i];
    if (item.Hidden)
    {
      // not in any directory: only its data is written
      Nodes.emplace_back();
      CNode &n = Nodes.back();
      n.Name = item.Path;
      n.Hidden = true;
      n.Item = (int)i;
      n.Size = item.Size;
      n.MTime = item.MTime != 0 ? item.MTime : now;
      n.Mode = MY_LIN_S_IFREG | 0644;
      continue;
    }
    unsigned cur;
    bool existed;
    RINOK(Insert(item.Path, item.IsDir, now, cur, existed))
    CNode &n = Nodes[cur];
    if (existed)
    {
      // the same path twice (a directory given explicitly after its files, or
      // a duplicate): the explicit item wins for the properties
      if (!n.IsDir || !item.IsDir)
        return E_INVALIDARG;
      n.Item = (int)i;
      if (item.MTime != 0)
        n.MTime = item.MTime;
      if (item.Mode != 0)
        n.Mode = item.Mode;
    }
    else
    {
      n.Item = (int)i;
      n.Size = item.IsDir ? 0 : item.Size;
      n.SymLink = item.SymLink;
      if (item.MTime != 0)
        n.MTime = item.MTime;
      n.Mode = item.Mode;
    }
  }

  if (catalogPath)
  {
    // the boot catalog: one sector of generated content. A file already there with
    // that name (the old catalog of an extracted image) is replaced, as xorriso does
    unsigned cur;
    bool existed;
    RINOK(Insert(*catalogPath, false, now, cur, existed))
    if (cur == 0 || (existed && Nodes[cur].IsDir))
      return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    CNode &n = Nodes[cur];
    n.Size = kSector;
    n.SymLink.Empty();
    if (existed)
      n.MTime = now;
    CatalogNode = (int)cur;
  }

  // modes, identifiers and order of the children
  for (unsigned i = 0; i < Nodes.size(); i++)
  {
    CNode &n = Nodes[i];
    if (n.Mode == 0 || (n.Mode & MY_LIN_S_IFMT) == 0)
    {
      UInt32 type = n.IsDir ? MY_LIN_S_IFDIR : (n.SymLink.IsEmpty() ? MY_LIN_S_IFREG : MY_LIN_S_IFLNK);
      n.Mode = type | (n.Mode == 0 ? (n.IsDir ? 0755u : 0644u) : (n.Mode & 07777));
    }
    if (!n.IsDir)
      continue;
    std::unordered_set<std::string> used;
    std::unordered_set<std::wstring> usedJoliet;
    for (const unsigned c : n.Children)
    {
      CNode &child = Nodes[c];
      child.Id = MakeId9660(child.Name, child.IsDir);
      MakeUnique(child.Id, used, child.IsDir ? kMaxDirId9660 : kMaxId9660, !child.IsDir);
      child.IdJoliet = MakeIdJoliet(child.Name);
      MakeUnique(child.IdJoliet, usedJoliet, kMaxIdJoliet, true);
    }
    n.ChildrenJoliet = n.Children;
    const std::vector<CNode> &nodes = Nodes;
    std::sort(n.Children.begin(), n.Children.end(), [&nodes](unsigned a, unsigned b)
    {
      return strcmp(nodes[a].Id, nodes[b].Id) < 0;
    });
    std::sort(n.ChildrenJoliet.begin(), n.ChildrenJoliet.end(), [&nodes](unsigned a, unsigned b)
    {
      // UCS-2 big endian byte order = code point order
      return wcscmp(nodes[a].IdJoliet, nodes[b].IdJoliet) < 0;
    });
  }
  return S_OK;
}

// ---------- Rock Ridge (SUSP entries)

class CSusp
{
public:
  CByteBuffer Entries;   // the entries of one record, in order

  void Add(const char *sig, const Byte *data, unsigned dataSize, Byte version = 1)
  {
    Byte h[4] = { (Byte)sig[0], (Byte)sig[1], (Byte)(4 + dataSize), version };
    Append(Entries, h, 4);
    if (dataSize)
      Append(Entries, data, dataSize);
  }

  void AddSP()
  {
    const Byte d[3] = { 0xBE, 0xEF, 0 };
    Add("SP", d, 3);
  }

  void AddER()
  {
    // the strings of RRIP 1.12 (as mkisofs and xorriso write them; the readers look for the id)
    static const char k_Id[] = "IEEE_P1282";
    static const char k_Desc[] = "THE IEEE P1282 PROTOCOL PROVIDES SUPPORT FOR POSIX FILE SYSTEM SEMANTICS.";
    static const char k_Src[] = "PLEASE CONTACT THE IEEE STANDARDS DEPARTMENT, PISCATAWAY, NJ, USA FOR THE P1282 SPECIFICATION.";
    CByteBuffer d;
    const Byte lens[4] = { (Byte)(sizeof(k_Id) - 1), (Byte)(sizeof(k_Desc) - 1), (Byte)(sizeof(k_Src) - 1), 1 };
    Append(d, lens, 4);
    Append(d, k_Id, sizeof(k_Id) - 1);
    Append(d, k_Desc, sizeof(k_Desc) - 1);
    Append(d, k_Src, sizeof(k_Src) - 1);
    Add("ER", d, (unsigned)d.Size());
  }

  // RRIP 1.12 form (44 bytes): mode, links, uid, gid, file serial number
  void AddPX(UInt32 mode, UInt32 nlinks, UInt32 serial)
  {
    Byte d[40];
    SetBoth32(d, mode);
    SetBoth32(d + 8, nlinks);
    SetBoth32(d + 16, 0);   // uid
    SetBoth32(d + 24, 0);   // gid
    SetBoth32(d + 32, serial);
    Add("PX", d, 40);
  }

  void AddTF(UInt64 mtime)
  {
    Byte d[8];
    d[0] = 2;   // MODIFY only, 7-byte form
    SetRecordingDate(d + 1, mtime);
    Add("TF", d, 8);
  }

  // NM entries: the name split in pieces of at most 250 bytes, flag CONTINUE on all but the last
  void AddNM(const AString &name)
  {
    unsigned pos = 0;
    do
    {
      const unsigned n = name.Len() - pos < 250 ? name.Len() - pos : 250;
      CByteBuffer d;
      const Byte flags = (pos + n < name.Len()) ? 1 : 0;
      Append(d, &flags, 1);
      Append(d, name.Ptr(pos), n);
      Add("NM", d, (unsigned)d.Size());
      pos += n;
    }
    while (pos < name.Len());
  }

  // SL entries: one component record per path component; an entry holds at most
  // 250 bytes of components, the rest continues in the next entry (flag CONTINUE)
  void AddSL(const AString &target)
  {
    CByteBuffer comps;
    auto flush = [&](bool more)
    {
      CByteBuffer d;
      const Byte flags = more ? 1 : 0;
      Append(d, &flags, 1);
      Append(d, comps, comps.Size());
      Add("SL", d, (unsigned)d.Size());
      comps.Free();
    };
    auto addComp = [&](Byte flags, const char *data, unsigned len)
    {
      if (comps.Size() + 2 + len > 250)
        flush(true);
      const Byte head[2] = { flags, (Byte)len };
      Append(comps, head, 2);
      if (len)
        Append(comps, data, len);
    };
    unsigned pos = 0;
    const bool absolute = !target.IsEmpty() && target[0] == '/';
    if (absolute)
    {
      addComp(8, NULL, 0);   // ROOT
      pos = 1;
    }
    while (pos <= target.Len())
    {
      int end = target.Find('/', pos);
      if (end < 0)
        end = (int)target.Len();
      const unsigned n = (unsigned)end - pos;
      if (n == 1 && target[pos] == '.')
        addComp(2, NULL, 0);   // CURRENT
      else if (n == 2 && target[pos] == '.' && target[pos + 1] == '.')
        addComp(4, NULL, 0);   // PARENT
      else if (n != 0)
      {
        // a component longer than 248 bytes continues in the next component record
        for (unsigned cp = 0; cp < n;)
        {
          const unsigned k = n - cp < 248 ? n - cp : 248;
          addComp((cp + k < n) ? 1 : 0, target.Ptr(pos + cp), k);
          cp += k;
        }
      }
      else if (!absolute || pos != 1)
        addComp(0, NULL, 0);   // an empty component ("a//b", trailing slash)
      pos = (unsigned)end + 1;
    }
    flush(false);
  }
};

// ---------- directory records

class CImage
{
public:
  CTree Tree;
  COutOptions Options;

  UInt32 PvdSector, BootRecordSector, SvdSector, TermSector;
  UInt32 PathTableSize, PathTableSizeJoliet;
  UInt32 PathLSector, PathMSector, PathLSectorJoliet, PathMSectorJoliet;
  UInt32 FirstDataSector;
  UInt32 TotalSectors;
  std::vector<unsigned> DirsBfs;   // directories in path table order (9660 tree)
  std::vector<unsigned> DirsBfsJoliet;
  std::vector<unsigned> BootNodes;   // the node of each entry of Options.Boot
  std::map<unsigned, CByteBuffer> Generated;   // content written from memory, by node: the catalog, patched boot images

  CImage(): PvdSector(0), BootRecordSector(0), SvdSector(0), TermSector(0),
      PathTableSize(0), PathTableSizeJoliet(0), PathLSector(0), PathMSector(0),
      PathLSectorJoliet(0), PathMSectorJoliet(0), FirstDataSector(0), TotalSectors(0) {}

  HRESULT ResolveBoot();   // after Tree.Build: the nodes of the boot entries
  HRESULT Layout();
  void BuildHeaders(CByteBuffer &out);   // everything before the file data
  void BuildCatalog();                   // Generated[CatalogNode], after Layout
  // the boot info table of a BIOS image (isolinux, GRUB): bytes 8..63
  bool PatchBootInfoTable(unsigned node, CByteBuffer &data, int mode) const;
  int BiosBootNode() const;   // the node of the first 80x86 entry, -1 if none
  int EfiBootNode() const;    // the node of the first UEFI entry, -1 if none

private:
  void BuildSystemArea(Byte *area);   // sectors 0..15
  void BuildBootRecord(Byte *d);      // the El Torito boot record volume descriptor
  // the directory extent of `dir` and, for the 9660 tree, its continuation area
  void BuildDir(unsigned dir, bool joliet, CByteBuffer &extent, CByteBuffer &ce, UInt32 ceSector);
  void BuildRecord(CByteBuffer &out, const CNode &node, unsigned extentIndex, bool joliet,
      const Byte *id, unsigned idLen, const CSusp *susp, CByteBuffer *ce, UInt32 ceSector);
  void BuildPathTable(CByteBuffer &out, bool joliet, bool bigEndian);
  void BuildDescriptor(CByteBuffer &out, bool joliet);
  void OrderDirs(std::vector<unsigned> &order, bool joliet);
};

void CImage::OrderDirs(std::vector<unsigned> &order, bool joliet)
{
  order.clear();
  order.push_back(0);
  for (size_t k = 0; k < order.size(); k++)
  {
    const CNode &n = Tree.Nodes[order[k]];
    for (const unsigned c : (joliet ? n.ChildrenJoliet : n.Children))
      if (Tree.Nodes[c].IsDir)
        order.push_back(c);
  }
}

// one directory record (ECMA-119 9.1); `ce` collects the SUSP entries that do not fit
void CImage::BuildRecord(CByteBuffer &out, const CNode &node, unsigned extentIndex, bool joliet,
    const Byte *id, unsigned idLen, const CSusp *susp, CByteBuffer *ce, UInt32 ceSector)
{
  Byte r[kMaxRecord];
  memset(r, 0, sizeof(r));
  unsigned len = 33 + idLen;
  if ((idLen & 1) == 0)
    len++;   // padding byte
  UInt32 extent, size;
  Byte flags = 0;
  if (node.IsDir)
  {
    extent = joliet ? node.DirExtentJoliet : node.DirExtent;
    size = joliet ? node.DirSizeJoliet : node.DirSize;
    flags = 2;
  }
  else
  {
    const UInt64 offset = (UInt64)extentIndex * kMaxExtentSize;
    extent = node.DataExtent + (UInt32)(offset / kSector);
    const UInt64 rest = node.Size - offset;
    if (rest > kMaxExtentSize)
    {
      size = kMaxExtentSize;
      flags = 0x80;   // multi-extent: more records follow
    }
    else
      size = (UInt32)rest;
  }
  SetBoth32(r + 2, extent);
  SetBoth32(r + 10, size);
  SetRecordingDate(r + 18, node.MTime);
  r[25] = flags;
  SetBoth16(r + 28, 1);   // volume sequence number
  r[32] = (Byte)idLen;
  memcpy(r + 33, id, idLen);

  if (susp && susp->Entries.Size() != 0)
  {
    // inline what fits; the rest goes to the continuation area through a CE entry (28 bytes)
    const Byte *e = susp->Entries;
    const size_t total = susp->Entries.Size();
    size_t pos = 0;
    while (pos < total)
    {
      const unsigned elen = e[pos + 2];
      const bool last = (pos + elen >= total);
      const unsigned need = elen + (last ? 0 : 28);
      if (len + need > kMaxRecord)
        break;
      memcpy(r + len, e + pos, elen);
      len += elen;
      pos += elen;
    }
    if (pos < total)
    {
      // CE: location, offset and length of the continuation, all both-endian
      const size_t restLen = total - pos;
      Byte c[28] = { 'C', 'E', 28, 1 };
      SetBoth32(c + 4, ceSector);
      SetBoth32(c + 12, (UInt32)ce->Size());
      SetBoth32(c + 20, (UInt32)restLen);
      memcpy(r + len, c, 28);
      len += 28;
      Append(*ce, e + pos, restLen);
    }
  }
  r[0] = (Byte)len;

  // a record never crosses a sector boundary
  const size_t inSector = out.Size() % kSector;
  if (inSector + len > kSector)
    PadToSector(out);
  Append(out, r, len);
}

static AString NameUtf8(const UString &s)
{
  AString a;
  ConvertUnicodeToUTF8(s, a);
  return a;
}

void CImage::BuildDir(unsigned dir, bool joliet, CByteBuffer &extent, CByteBuffer &ce, UInt32 ceSector)
{
  extent.Free();
  ce.Free();
  const CNode &node = Tree.Nodes[dir];
  const CNode &parent = Tree.Nodes[node.Parent];
  const bool rr = Tree.RockRidge && !joliet;
  const Byte idDot = 0, idDotDot = 1;

  // "." and ".."
  {
    CSusp s;
    if (rr)
    {
      if (dir == 0)
        s.AddSP();
      s.AddPX(node.Mode, 2, dir + 1);
      s.AddTF(node.MTime);
      if (dir == 0)
        s.AddER();
    }
    BuildRecord(extent, node, 0, joliet, &idDot, 1, rr ? &s : NULL, &ce, ceSector);
  }
  {
    CSusp s;
    if (rr)
    {
      s.AddPX(parent.Mode, 2, node.Parent + 1);
      s.AddTF(parent.MTime);
    }
    BuildRecord(extent, parent, 0, joliet, &idDotDot, 1, rr ? &s : NULL, &ce, ceSector);
  }

  for (const unsigned c : (joliet ? node.ChildrenJoliet : node.Children))
  {
    const CNode &child = Tree.Nodes[c];
    CByteBuffer id;
    if (joliet)
    {
      const UString &j = child.IdJoliet;
      id.Alloc(j.Len() * 2);
      for (unsigned i = 0; i < j.Len(); i++)
        SetBe16(id + i * 2, (UInt16)j[i]);
    }
    else
    {
      AString a = child.Id;
      if (!child.IsDir)
        a += ";1";
      id.CopyFrom((const Byte *)a.Ptr(), a.Len());
    }
    CSusp s;
    if (rr)
    {
      s.AddNM(NameUtf8(child.Name));
      s.AddPX(child.Mode, child.IsDir ? 2 : 1, c + 1);
      s.AddTF(child.MTime);
      if (!child.SymLink.IsEmpty())
        s.AddSL(child.SymLink);
    }
    unsigned numExtents = 1;
    if (!child.IsDir && child.Size > kMaxExtentSize)
      numExtents = (unsigned)((child.Size + kMaxExtentSize - 1) / kMaxExtentSize);
    for (unsigned k = 0; k < numExtents; k++)
      BuildRecord(extent, child, k, joliet, id, (unsigned)id.Size(), rr ? &s : NULL, &ce, ceSector);
  }
  PadToSector(extent);
  PadToSector(ce);
}

void CImage::BuildPathTable(CByteBuffer &out, bool joliet, bool bigEndian)
{
  out.Free();
  const std::vector<unsigned> &dirs = joliet ? DirsBfsJoliet : DirsBfs;
  for (const unsigned d : dirs)
  {
    const CNode &n = Tree.Nodes[d];
    CByteBuffer id;
    if (d == 0)
    {
      id.Alloc(1);
      id[0] = 0;
    }
    else if (joliet)
    {
      id.Alloc(n.IdJoliet.Len() * 2);
      for (unsigned i = 0; i < n.IdJoliet.Len(); i++)
        SetBe16(id + i * 2, (UInt16)n.IdJoliet[i]);
    }
    else
      id.CopyFrom((const Byte *)n.Id.Ptr(), n.Id.Len());
    Byte h[8];
    h[0] = (Byte)id.Size();
    h[1] = 0;
    const UInt32 extent = joliet ? n.DirExtentJoliet : n.DirExtent;
    const UInt32 parentNum = joliet ? Tree.Nodes[n.Parent].DirNumJoliet : Tree.Nodes[n.Parent].DirNum;
    if (bigEndian)
    {
      SetBe32(h + 2, extent)
      SetBe16(h + 6, parentNum);
    }
    else
    {
      SetUi32(h + 2, extent)
      SetUi16(h + 6, (UInt16)parentNum)
    }
    Append(out, h, 8);
    Append(out, id, id.Size());
    if (id.Size() & 1)
    {
      const Byte z = 0;
      Append(out, &z, 1);
    }
  }
}

void CImage::BuildDescriptor(CByteBuffer &out, bool joliet)
{
  Byte d[kSector];
  memset(d, 0, sizeof(d));
  d[0] = joliet ? 2 : 1;
  memcpy(d + 1, "CD001", 5);
  d[6] = 1;
  const CNode &root = Tree.Nodes[0];
  SetPadded(d + 8, 32, AString());   // system identifier: a-characters in both descriptors
  if (joliet)
  {
    SetPaddedUcs2(d + 40, 32, MultiByteToUnicodeString(Options.VolumeId, CP_UTF8));
    memcpy(d + 88, "%/E", 3);   // UCS-2 level 3
  }
  else
    SetPadded(d + 40, 32, Options.VolumeId);
  SetBoth32(d + 80, TotalSectors);
  SetBoth16(d + 120, 1);   // volume set size
  SetBoth16(d + 124, 1);   // volume sequence number
  SetBoth16(d + 128, kSector);
  SetBoth32(d + 132, joliet ? PathTableSizeJoliet : PathTableSize);
  SetUi32(d + 140, joliet ? PathLSectorJoliet : PathLSector)
  SetUi32(d + 144, 0)
  SetBe32(d + 148, joliet ? PathMSectorJoliet : PathMSector)
  SetBe32(d + 152, 0)
  {
    // root directory record (34 bytes)
    CByteBuffer r;
    const Byte idDot = 0;
    BuildRecord(r, root, 0, joliet, &idDot, 1, NULL, NULL, 0);
    memcpy(d + 156, r, 34);
  }
  AString app ("HEPTAZIP ");
  app += HZ_VERSION;
  UpperAscii(app);
  if (joliet)
  {
    SetPaddedUcs2(d + 190, 128, UString());   // volume set id
    SetPaddedUcs2(d + 318, 128, UString());   // publisher
    SetPaddedUcs2(d + 446, 128, UString());   // data preparer
    SetPaddedUcs2(d + 574, 128, MultiByteToUnicodeString(app, CP_UTF8));
    SetPaddedUcs2(d + 702, 36, UString());
    d[738] = ' ';
    SetPaddedUcs2(d + 739, 36, UString());
    d[775] = ' ';
    SetPaddedUcs2(d + 776, 36, UString());
    d[812] = ' ';
  }
  else
  {
    SetPadded(d + 190, 128, AString());
    SetPadded(d + 318, 128, AString());
    SetPadded(d + 446, 128, AString());
    SetPadded(d + 574, 128, app);
    SetPadded(d + 702, 37, AString());
    SetPadded(d + 739, 37, AString());
    SetPadded(d + 776, 37, AString());
  }
  SetDescriptorDate(d + 813, Options.Now);   // creation
  SetDescriptorDate(d + 830, Options.Now);   // modification
  SetDescriptorDate(d + 847, 0);             // expiration
  SetDescriptorDate(d + 864, Options.Now);   // effective
  d[881] = 1;   // file structure version
  Append(out, d, kSector);
}

HRESULT CImage::Layout()
{
  OrderDirs(DirsBfs, false);
  OrderDirs(DirsBfsJoliet, true);
  if (DirsBfs.size() > 0xFFFF)
    return E_INVALIDARG;   // path table: 16-bit parent numbers
  for (size_t k = 0; k < DirsBfs.size(); k++)
    Tree.Nodes[DirsBfs[k]].DirNum = (UInt32)k + 1;
  for (size_t k = 0; k < DirsBfsJoliet.size(); k++)
    Tree.Nodes[DirsBfsJoliet[k]].DirNumJoliet = (UInt32)k + 1;

  PvdSector = 16;
  UInt32 sector = 17;
  if (!Options.Boot.IsEmpty())
    BootRecordSector = sector++;   // El Torito wants it at sector 17
  if (Tree.Joliet)
    SvdSector = sector++;
  TermSector = sector++;

  // path tables: the size does not depend on the locations
  {
    CByteBuffer t;
    BuildPathTable(t, false, false);
    PathTableSize = (UInt32)t.Size();
    PathLSector = sector;
    sector += SectorsOf(PathTableSize);
    PathMSector = sector;
    sector += SectorsOf(PathTableSize);
    if (Tree.Joliet)
    {
      BuildPathTable(t, true, false);
      PathTableSizeJoliet = (UInt32)t.Size();
      PathLSectorJoliet = sector;
      sector += SectorsOf(PathTableSizeJoliet);
      PathMSectorJoliet = sector;
      sector += SectorsOf(PathTableSizeJoliet);
    }
  }

  // directory extents: the sizes do not depend on the locations either
  CByteBuffer extent, ce;
  for (const unsigned d : DirsBfs)
  {
    CNode &n = Tree.Nodes[d];
    BuildDir(d, false, extent, ce, 0);
    n.DirExtent = sector;
    n.DirSize = (UInt32)extent.Size();
    sector += SectorsOf(n.DirSize);
    n.CeExtent = sector;
    n.CeSize = (UInt32)ce.Size();
    sector += SectorsOf(n.CeSize);
  }
  if (Tree.Joliet)
    for (const unsigned d : DirsBfsJoliet)
    {
      CNode &n = Tree.Nodes[d];
      BuildDir(d, true, extent, ce, 0);
      n.DirExtentJoliet = sector;
      n.DirSizeJoliet = (UInt32)extent.Size();
      sector += SectorsOf(n.DirSizeJoliet);
    }

  // file data, in the order of the items (= order of the nodes, root excluded)
  FirstDataSector = sector;
  for (CNode &n : Tree.Nodes)
  {
    if (n.IsDir)
      continue;
    n.DataExtent = sector;
    const UInt64 sectors = (n.Size + kSector - 1) / kSector;
    if (sectors > 0xFFFFFFFF - sector)
      return E_INVALIDARG;   // more than 8 TiB: 32-bit sector numbers
    sector += (UInt32)sectors;
  }
  TotalSectors = sector;
  return S_OK;
}

void CImage::BuildHeaders(CByteBuffer &out)
{
  out.Alloc((size_t)FirstDataSector * kSector);
  memset(out, 0, out.Size());
  Byte *const base = out;
  if (Options.Hybrid)
    BuildSystemArea(base);
  CByteBuffer t;
  BuildDescriptor(t, false);
  memcpy(base + (size_t)PvdSector * kSector, t, kSector);
  if (BootRecordSector)
    BuildBootRecord(base + (size_t)BootRecordSector * kSector);
  if (Tree.Joliet)
  {
    t.Free();
    BuildDescriptor(t, true);
    memcpy(base + (size_t)SvdSector * kSector, t, kSector);
  }
  {
    Byte *term = base + (size_t)TermSector * kSector;
    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;
  }
  BuildPathTable(t, false, false);
  memcpy(base + (size_t)PathLSector * kSector, t, t.Size());
  BuildPathTable(t, false, true);
  memcpy(base + (size_t)PathMSector * kSector, t, t.Size());
  if (Tree.Joliet)
  {
    BuildPathTable(t, true, false);
    memcpy(base + (size_t)PathLSectorJoliet * kSector, t, t.Size());
    BuildPathTable(t, true, true);
    memcpy(base + (size_t)PathMSectorJoliet * kSector, t, t.Size());
  }
  CByteBuffer extent, ce;
  for (const unsigned d : DirsBfs)
  {
    const CNode &n = Tree.Nodes[d];
    BuildDir(d, false, extent, ce, n.CeExtent);
    memcpy(base + (size_t)n.DirExtent * kSector, extent, extent.Size());
    if (ce.Size())
      memcpy(base + (size_t)n.CeExtent * kSector, ce, ce.Size());
  }
  if (Tree.Joliet)
    for (const unsigned d : DirsBfsJoliet)
    {
      const CNode &n = Tree.Nodes[d];
      BuildDir(d, true, extent, ce, 0);
      memcpy(base + (size_t)n.DirExtentJoliet * kSector, extent, extent.Size());
    }
}

// ---------- El Torito and the system area (design note 0020)

HRESULT CImage::ResolveBoot()
{
  BootNodes.clear();
  if (Options.Boot.Size() > kMaxBootEntries)
    return E_INVALIDARG;
  FOR_VECTOR (i, Options.Boot)
  {
    const int node = Tree.Find(Options.Boot[i].Path);
    if (node <= 0)
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);   // the boot image must be a file of the image
    const CNode &n = Tree.Nodes[(unsigned)node];
    if (n.IsDir || !n.SymLink.IsEmpty() || n.Size == 0 || n.Size > 0xFFFFFFFF)
      return E_INVALIDARG;
    BootNodes.push_back((unsigned)node);
  }
  return S_OK;
}

int CImage::BiosBootNode() const
{
  FOR_VECTOR (i, Options.Boot)
    if (Options.Boot[i].PlatformId == 0)
      return (int)BootNodes[i];
  return -1;
}

int CImage::EfiBootNode() const
{
  FOR_VECTOR (i, Options.Boot)
    if (Options.Boot[i].PlatformId == 0xEF)
      return (int)BootNodes[i];
  return -1;
}

// boot record volume descriptor (El Torito 2.0): points to the catalog
void CImage::BuildBootRecord(Byte *d)
{
  memset(d, 0, kSector);
  d[0] = 0;   // boot record
  memcpy(d + 1, "CD001", 5);
  d[6] = 1;
  static const char k_Spec[] = "EL TORITO SPECIFICATION";
  memcpy(d + 7, k_Spec, sizeof(k_Spec) - 1);   // the rest of the 32 bytes stays zero
  // boot identifier (32 bytes) zero; then the absolute sector of the catalog
  SetUi32(d + 71, Tree.Nodes[(unsigned)Tree.CatalogNode].DataExtent)
}

// one sector: validation entry, default entry, then one section (header + entry)
// per further boot image
void CImage::BuildCatalog()
{
  CByteBuffer &out = Generated[(unsigned)Tree.CatalogNode];
  out.Alloc(kSector);
  Byte *c = out;
  memset(c, 0, kSector);

  // validation entry: id, platform of the default entry, 24 bytes of id string, checksum, 55 AA
  c[0] = 1;
  c[1] = Options.Boot[0].PlatformId;
  c[30] = 0x55;
  c[31] = 0xAA;
  {
    UInt32 sum = 0;
    for (unsigned i = 0; i < 32; i += 2)
      sum += GetUi16(c + i);
    SetUi16(c + 28, (UInt16)(0x10000 - (sum & 0xFFFF)))
  }

  unsigned pos = 32;
  FOR_VECTOR (i, Options.Boot)
  {
    const COutBootEntry &e = Options.Boot[i];
    const CNode &n = Tree.Nodes[BootNodes[i]];
    if (i != 0)
    {
      // section header: more sections follow (0x90) or the last one (0x91)
      c[pos] = (i + 1 < Options.Boot.Size()) ? 0x90 : 0x91;
      c[pos + 1] = e.PlatformId;
      SetUi16(c + pos + 2, 1)   // entries in this section
      pos += 32;
    }
    Byte *p = c + pos;
    p[0] = 0x88;   // bootable
    p[1] = e.MediaType;
    SetUi16(p + 2, e.LoadSegment)
    p[4] = e.SystemType;
    UInt32 sectors = e.SectorCount;
    if (sectors == 0)
    {
      // the whole image in 512-byte sectors; beyond 16 bits the UEFI rule applies:
      // 0 = "from the image to the end of the disc"
      const UInt64 all = (n.Size + 511) / 512;
      sectors = all <= 0xFFFF ? (UInt32)all : 0;
    }
    SetUi16(p + 6, (UInt16)sectors)
    SetUi32(p + 8, n.DataExtent)
    pos += 32;
  }
}

// the boot info table used by isolinux and GRUB's eltorito.img: at byte 8 the sector
// of the PVD, the sector of the image, its length and the checksum of the rest of
// the image (32-bit words from byte 64). `mode`: -1 = only if bytes 8..63 are free
// or already hold a table, 1 = always. Returns whether it was patched.
bool CImage::PatchBootInfoTable(unsigned node, CByteBuffer &data, int mode) const
{
  if (mode == 0 || data.Size() < 64)
    return false;
  Byte *d = data;
  if (mode < 0)
  {
    bool unused = true;
    for (unsigned i = 8; i < 64 && unused; i++)
      unused = (d[i] == 0);
    if (!unused && GetUi32(d + 8) != 16)
      return false;
  }
  UInt32 sum = 0;
  size_t i = 64;
  for (; i + 4 <= data.Size(); i += 4)
    sum += GetUi32(d + i);
  if (i < data.Size())
  {
    Byte tail[4] = { 0, 0, 0, 0 };
    memcpy(tail, d + i, data.Size() - i);
    sum += GetUi32(tail);
  }
  SetUi32(d + 8, 16)
  SetUi32(d + 12, Tree.Nodes[node].DataExtent)
  SetUi32(d + 16, (UInt32)data.Size())
  SetUi32(d + 20, sum)
  return true;
}

// CHS address of a 512-byte sector for the partition table (geometry 64 heads, 32 sectors,
// the one isohybrid uses), capped at the maximum representable
static void SetChs(Byte *p, UInt64 lba)
{
  UInt64 cyl = lba / (64 * 32);
  if (cyl > 1023)
  {
    p[0] = 0xFE;
    p[1] = 0xFF;
    p[2] = 0xFF;
    return;
  }
  const unsigned head = (unsigned)((lba / 32) % 64);
  const unsigned sec = (unsigned)(lba % 32) + 1;
  p[0] = (Byte)head;
  p[1] = (Byte)(sec | ((cyl >> 8) << 6));
  p[2] = (Byte)cyl;
}

static void SetPartition(Byte *p, Byte status, Byte type, UInt64 startLba, UInt64 sizeLba)
{
  p[0] = status;
  SetChs(p + 1, startLba);
  p[4] = type;
  SetChs(p + 5, startLba + sizeLba - 1);
  SetUi32(p + 8, (UInt32)startLba)
  SetUi32(p + 12, (UInt32)sizeLba)
}

// isohybrid MBR: boot code (if given) with the sector of the BIOS image at 0x1B0 as
// isohdpfx expects, a disk id, partition 1 = the whole image (bootable, type 0 so that
// UEFI firmware does not reject the table for its start at sector 0), partition 2 =
// the UEFI image (type 0xEF) if there is one
void CImage::BuildSystemArea(Byte *area)
{
  memset(area, 0, 16 * kSector);
  Byte *mbr = area;
  if (Options.MbrCode.Size() != 0)
    memcpy(mbr, Options.MbrCode, Options.MbrCode.Size() < kMbrCodeSize ? Options.MbrCode.Size() : kMbrCodeSize);
  const int bios = BiosBootNode();
  if (bios >= 0)
    SetUi64(mbr + 0x1B0, (UInt64)Tree.Nodes[(unsigned)bios].DataExtent * 4)
  UInt32 diskId = (UInt32)(Options.Now ^ (Options.Now >> 32));
  if (diskId == 0)
    diskId = 0x48455054;   // "HEPT"
  SetUi32(mbr + 440, diskId)
  SetPartition(mbr + 446, 0x80, 0x00, 0, (UInt64)TotalSectors * 4);
  const int efi = EfiBootNode();
  if (efi >= 0)
  {
    const CNode &n = Tree.Nodes[(unsigned)efi];
    SetPartition(mbr + 462, 0x00, 0xEF, (UInt64)n.DataExtent * 4, (n.Size + 511) / 512);
  }
  mbr[510] = 0x55;
  mbr[511] = 0xAA;
}

// ---------- the image

// ECMA-119 wants d-characters; every mastering tool (xorriso, mkisofs, oscdimg) accepts
// printable ASCII and keeps the case, and so do the readers: same here
static bool IsValidVolumeId(const AString &s)
{
  if (s.Len() > 32)
    return false;
  for (unsigned i = 0; i < s.Len(); i++)
    if (s[i] < 0x20 || s[i] > 0x7E)
      return false;
  return true;
}

HRESULT WriteIsoImage(ISequentialOutStream *outStream,
    const CObjectVector<COutItem> &items, const COutOptions &options,
    IOutDataSource &source, IArchiveUpdateCallback *progress)
{
  CImage image;
  image.Options = options;
  if (image.Options.VolumeId.IsEmpty())
    image.Options.VolumeId = "ISOIMAGE";
  if (!IsValidVolumeId(image.Options.VolumeId))
    return E_INVALIDARG;
  image.Tree.Joliet = options.Joliet;
  image.Tree.RockRidge = options.RockRidge;
  const bool boot = !options.Boot.IsEmpty();
  UString catalogPath = options.CatalogPath;
  if (catalogPath.IsEmpty())
    catalogPath = L"boot.catalog";
  if (!boot)
    image.Options.Hybrid = false;   // a partition table makes sense only with something to boot

  RINOK(image.Tree.Build(items, options.Now, boot ? &catalogPath : NULL))
  RINOK(image.ResolveBoot())
  RINOK(image.Layout())

  UInt64 total = (UInt64)image.FirstDataSector * kSector;
  FOR_VECTOR (i, items)
    if (!items[i].IsDir)
      total += items[i].Size;
  if (boot)
    total += kSector;
  if (progress)
    RINOK(progress->SetTotal(total))

  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(progress, true);

  // the BIOS boot images are read now, to patch their boot info table with the
  // sectors of the layout; `changedNodes` remembers the ones that were shorter than declared
  std::unordered_set<unsigned> changedNodes;
  if (boot)
  {
    image.BuildCatalog();
    FOR_VECTOR (i, options.Boot)
    {
      const COutBootEntry &e = options.Boot[i];
      const unsigned node = image.BootNodes[i];
      if (e.BootInfoTable == 0 || e.PlatformId != 0 || image.Generated.count(node) != 0)
        continue;
      const CNode &n = image.Tree.Nodes[node];
      if (n.Size > kMaxBootImageInMemory)
        continue;
      CByteBuffer data;
      data.Alloc((size_t)n.Size);
      memset(data, 0, data.Size());
      CMyComPtr<ISequentialInStream> inStream;
      const HRESULT res = source.GetStream(items[(unsigned)n.Item], &inStream);
      if (res != S_FALSE)
      {
        RINOK(res)
      }
      size_t got = 0;
      if (inStream)
      {
        got = data.Size();
        RINOK(ReadStream(inStream, data, &got))
      }
      if (got != data.Size())
        changedNodes.insert(node);
      image.PatchBootInfoTable(node, data, e.BootInfoTable);
      image.Generated[node] = data;
    }
  }

  {
    CByteBuffer headers;
    image.BuildHeaders(headers);
    RINOK(WriteStream(outStream, headers, headers.Size()))
    lps->InSize = lps->OutSize = headers.Size();
    RINOK(lps->SetCur())
  }

  CMyComPtr2_Create<ICompressCoder, NCompress::CCopyCoder> copyCoder;
  CMyComPtr2_Create<ISequentialInStream, CLimitedSequentialInStream> limited;
  Byte zeros[kSector];
  memset(zeros, 0, sizeof(zeros));
  UInt64 done = lps->OutSize;

  for (unsigned nodeIndex = 0; nodeIndex < image.Tree.Nodes.size(); nodeIndex++)
  {
    const CNode &n = image.Tree.Nodes[nodeIndex];
    if (n.IsDir)
      continue;
    const std::map<unsigned, CByteBuffer>::const_iterator gen = image.Generated.find(nodeIndex);
    if (gen != image.Generated.end())
    {
      // content prepared in memory: the catalog, a patched boot image
      RINOK(WriteStream(outStream, gen->second, gen->second.Size()))
      const unsigned pad = (unsigned)((kSector - (n.Size % kSector)) % kSector);
      if (pad)
        RINOK(WriteStream(outStream, zeros, pad))
      done += n.Size;
      lps->InSize = lps->OutSize = done;
      RINOK(lps->SetCur())
      if (n.Item >= 0)
        RINOK(source.ReportResult(items[(unsigned)n.Item], changedNodes.count(nodeIndex) != 0))
      continue;
    }
    if (n.Item < 0)
      continue;
    const COutItem &item = items[(unsigned)n.Item];
    UInt64 written = 0;
    bool changed = false;
    if (n.Size != 0)
    {
      CMyComPtr<ISequentialInStream> inStream;
      const HRESULT res = source.GetStream(item, &inStream);
      if (res != S_FALSE)
      {
        RINOK(res)
      }
      if (inStream)
      {
        limited->SetStream(inStream);
        limited->Init(n.Size);
        RINOK(copyCoder.Interface()->Code(limited, outStream, NULL, NULL, lps))
        written = copyCoder->TotalSize;
      }
      if (written != n.Size)
      {
        // the file shrank or vanished since the sizes were collected: the image keeps
        // the declared size (the records are already written), the rest is zeros
        changed = true;
        for (UInt64 rest = n.Size - written; rest != 0;)
        {
          const UInt32 chunk = rest < kSector ? (UInt32)rest : kSector;
          RINOK(WriteStream(outStream, zeros, chunk))
          rest -= chunk;
        }
      }
    }
    const unsigned pad = (unsigned)((kSector - (n.Size % kSector)) % kSector);
    if (pad)
      RINOK(WriteStream(outStream, zeros, pad))
    done += n.Size;
    lps->InSize = lps->OutSize = done;
    RINOK(lps->SetCur())
    RINOK(source.ReportResult(item, changed))
  }
  return S_OK;
}

}}
