// IsoOut.h
// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
//
// Writer of ISO 9660 images with Joliet, Rock Ridge and El Torito (design notes 0019
// and 0020). The whole layout is computed in memory first, then the image is written
// sequentially: no seeking on the output stream.

#ifndef ZIP7_INC_ARCHIVE_ISO_OUT_H
#define ZIP7_INC_ARCHIVE_ISO_OUT_H

#include "../../../Common/MyBuffer.h"
#include "../../../Common/MyString.h"
#include "../../../Common/MyVector.h"

#include "../../IStream.h"
#include "../IArchive.h"

namespace NArchive {
namespace NIso {

// one entry given by the update callback (or taken from the archive being rewritten)
struct COutItem
{
  UString Path;       // relative, '/' separated, no trailing slash
  AString SymLink;    // UTF-8 target of a symbolic link, empty otherwise
  UInt64 Size;        // 0 for directories and links
  UInt64 MTime;       // FILETIME (100 ns since 1601), 0 = not set
  UInt32 Mode;        // POSIX mode (type bits included), 0 = not set
  bool IsDir;
  bool Hidden;        // data outside the directory trees: a boot image that is not a file of the image
  bool NewData;       // data from the update callback (IndexInClient), else from the old archive (IndexInArc)
  UInt32 IndexInClient;
  UInt32 IndexInArc;

  COutItem(): Size(0), MTime(0), Mode(0), IsDir(false), Hidden(false), NewData(false), IndexInClient(0), IndexInArc(0) {}
};

// one El Torito boot entry
struct COutBootEntry
{
  UString Path;         // path in the image of the boot image: a file of the tree or a hidden item
  Byte PlatformId;      // 0 = 80x86 (BIOS), 0xEF = UEFI
  Byte MediaType;       // 0 = no emulation, 1..3 = floppy emulation, 4 = hard disk emulation
  UInt16 LoadSegment;   // 0 = the default 7C0
  Byte SystemType;
  UInt16 SectorCount;   // 512-byte sectors loaded by the firmware; 0 = the whole image (UEFI rule for big ones)
  int BootInfoTable;    // patch the isolinux/GRUB boot info table: -1 when bytes 8..63 are free or hold one, 0 never, 1 always

  COutBootEntry(): PlatformId(0), MediaType(0), LoadSegment(0), SystemType(0), SectorCount(0), BootInfoTable(-1) {}
};

struct COutOptions
{
  AString VolumeId;   // printable ASCII, at most 32; empty = "ISOIMAGE"
  bool Joliet;
  bool RockRidge;
  UInt64 Now;         // FILETIME of the creation, for the descriptors and the implicit directories

  // El Torito (design note 0020)
  CObjectVector<COutBootEntry> Boot;   // the first entry is the default one
  UString CatalogPath;                 // path of the boot catalog file; empty = "boot.catalog"
  bool Hybrid;                         // MBR partition table in the system area (isohybrid)
  CByteBuffer MbrCode;                 // boot code of the MBR, at most 432 bytes; empty = none

  COutOptions(): Joliet(true), RockRidge(true), Now(0), Hybrid(false) {}
};

// Provides the data of the items: the callback for new data, the open archive for the
// rest (a rewrite of an existing image).
struct IOutDataSource
{
  virtual HRESULT GetStream(const COutItem &item, ISequentialInStream **stream) = 0;
  virtual HRESULT ReportResult(const COutItem &item, bool changed) = 0;
  virtual ~IOutDataSource() {}
};

HRESULT WriteIsoImage(ISequentialOutStream *outStream,
    const CObjectVector<COutItem> &items, const COutOptions &options,
    IOutDataSource &source, IArchiveUpdateCallback *progress);

}}

#endif
