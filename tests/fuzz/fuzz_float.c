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

#include "../../C/FloatPlaneLz.h"

static Byte g_Enc[FLOAT_PLANE_ENCODED_MAX(FLOAT_PLANE_BLOCK_MAX)];
static Byte g_Dec[FLOAT_PLANE_BLOCK_MAX];
static CFloatPlaneZstd g_Zstd;   /* zstd reale per i blocchi mode 2 */
static int g_ZstdInit;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  size_t used;
  if (!g_ZstdInit)
  {
    FloatPlaneZstd_Construct(&g_Zstd);
    g_ZstdInit = 1;
  }
  if (size < 2)
    return 0;
  if (data[0] & 1)
  {
    const size_t len = size - 1 > FLOAT_PLANE_BLOCK_MAX ? FLOAT_PLANE_BLOCK_MAX : size - 1;
    const size_t encLen = FloatPlane_EncodeBlock(data + 1, len, g_Enc, &g_Zstd.Lz);
    if (encLen > FLOAT_PLANE_ENCODED_MAX(len)
        || FloatPlane_DecodeBlock(g_Enc, encLen, g_Dec, len, &used, &g_Zstd.Lz) != SZ_OK
        || used != encLen
        || memcmp(g_Dec, data + 1, len) != 0)
      abort();
  }
  else
    FloatPlane_DecodeBlock(data + 1, size - 1, g_Dec, sizeof(g_Dec), &used, &g_Zstd.Lz);
  return 0;
}
