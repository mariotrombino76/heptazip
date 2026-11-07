// IsoHandler.h

#ifndef ZIP7_INC_ISO_HANDLER_H
#define ZIP7_INC_ISO_HANDLER_H

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
public:
  CHandler(): _joliet(true), _rockRidge(true) {}
};

}}

#endif
