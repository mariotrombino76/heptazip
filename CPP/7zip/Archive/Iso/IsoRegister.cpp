// IsoRegister.cpp

#include "StdAfx.h"

#include "../../Common/RegisterArc.h"

#include "IsoHandler.h"

namespace NArchive {
namespace NIso {

static const Byte k_Signature[] = { 'C', 'D', '0', '0', '1' };

// Heptazip: the handler also writes images (ISO 9660 + Joliet + Rock Ridge, IsoOut.cpp)
REGISTER_ARC_IO(
  "Iso", "iso img", NULL, 0xE7,
  k_Signature,
  NArchive::NIso::kStartPos + 1,
    NArcInfoFlags::kSymLinks
  | NArcInfoFlags::kMTime
  | NArcInfoFlags::kMTime_Default
  , TIME_PREC_TO_ARC_FLAGS_MASK (NFileTimeType::kUnix)
  | TIME_PREC_TO_ARC_FLAGS_TIME_DEFAULT (NFileTimeType::kUnix)
  , NULL)

}}
