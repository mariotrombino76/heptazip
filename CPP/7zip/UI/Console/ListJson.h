// ListJson.h
// Tensor7: JSON output of the "l" (list) command, switch -json.

#ifndef ZIP7_INC_LIST_JSON_H
#define ZIP7_INC_LIST_JSON_H

#include "List.h"

/* Same parameters as ListArchives() (without the text-output options).
   Prints one JSON document to stdout:
   {
     "archives": [ { "path", "type", <archive properties>, "items": [ {...} ] } ],
     "errors":   [ { "path", "message" } ]      // archives that can't be opened
   }
   Property names are the -slt names in lowerCamelCase ("Packed Size" -> "packedSize").
   See docs/json-output.md. */
HRESULT ListArchives_Json(
    const CListOptions &listOptions,
    CCodecs *codecs,
    const CObjectVector<COpenType> &types,
    const CIntVector &excludedFormats,
    bool stdInMode,
    UStringVector &archivePaths, UStringVector &archivePathsFull,
    bool processAltStreams, bool showAltStreams,
    const NWildcard::CCensorNode &wildcardCensor,
  #ifndef Z7_NO_CRYPTO
    bool &passwordEnabled, UString &password,
  #endif
  #ifndef Z7_SFX
    const CObjectVector<CProperty> *props,
  #endif
    UInt64 &errors,
    UInt64 &numWarnings);

/* Defined in List.cpp: name of the property as printed by -slt. */
void List_GetPropName(PROPID propID, const wchar_t *name, AString &nameA, UString &nameU);

#endif
