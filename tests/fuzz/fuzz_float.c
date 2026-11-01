/* fuzz_float.c -- harness libFuzzer per il formato a blocchi FLOAT (C/FloatPlane.c, design 0008)
 *
 * Primo byte dispari: l'input (senza il primo byte) e' un blocco da comprimere;
 *   encode -> decode deve restituire esattamente l'input.
 * Primo byte pari: l'input e' un blocco codificato, da decodificare: deve dare un errore
 *   o un blocco valido, senza accessi fuori dai limiti (ASan).
 *
 *   ./fuzz_float corpus/float
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../C/FloatPlane.h"

static Byte g_Enc[FLOAT_PLANE_ENCODED_MAX(FLOAT_PLANE_BLOCK_MAX)];
static Byte g_Dec[FLOAT_PLANE_BLOCK_MAX];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  size_t used;
  if (size < 2)
    return 0;
  if (data[0] & 1)
  {
    const size_t len = size - 1 > FLOAT_PLANE_BLOCK_MAX ? FLOAT_PLANE_BLOCK_MAX : size - 1;
    const size_t encLen = FloatPlane_EncodeBlock(data + 1, len, g_Enc);
    if (encLen > FLOAT_PLANE_ENCODED_MAX(len)
        || FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, len, &used) != SZ_OK
        || used != encLen
        || memcmp(g_Dec, data + 1, len) != 0)
      abort();
  }
  else
    FloatPlane_DecodeBlock(data + 1, size - 1, g_Dec, sizeof(g_Dec), &used);
  return 0;
}
