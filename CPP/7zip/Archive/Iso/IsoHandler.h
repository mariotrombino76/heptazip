// IsoHandler.h

#ifndef ZIP7_INC_ISO_HANDLER_H
#define ZIP7_INC_ISO_HANDLER_H

#include "../../../Common/MyBuffer.h"
#include "../../../Common/MyCom.h"

#include "../IArchive.h"

#include "IsoIn.h"

namespace NArchive {
namespace NIso {

Z7_CLASS_IMP_CHandler_IInArchive_3(
  IInArchiveGetStream,
  ISetProperties,
  IOutArchive
)
  CMyComPtr<IInStream> _stream;
  CInArchive _archive;

  // Heptazip: options of the writer (IsoHandlerOut.cpp, IsoOut.cpp)
  AString _volumeId;
  bool _joliet;
  bool _rockRidge;
  UString _bootPath;      // -mboot: BIOS boot image (path in the image)
  UString _efiPath;       // -mefi: UEFI boot image
  UString _catalogPath;   // -mcatalog: boot catalog file
  UInt32 _bootLoad;       // -mbootload: 512-byte sectors loaded for the BIOS image (0 = whole)
  int _bootInfo;          // -mbootinfo: -1 auto, 0 off, 1 on
  int _hybrid;            // -mhybrid: -1 auto, 0 off, 1 on
  CByteBuffer _mbrCode;   // -mmbr: boot code of the MBR (at most 432 bytes)

  int FindRefByBlock(UInt32 block) const;
public:
  CHandler(): _joliet(true), _rockRidge(true), _bootLoad(4), _bootInfo(-1), _hybrid(-1) {}
};

}}

#endif
