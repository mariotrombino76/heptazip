#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Composizione di un file GGUF: byte per tipo di tensore (F32, F16, BF16, Q4_K, Q6_K, ...).

Uso: tools/bench/gguf_info.py <file.gguf> [--tensors]

Legge solo l'intestazione (formato GGUF v2/v3: metadati chiave-valore e descrizione dei
tensori) e calcola la dimensione di ogni tensore dal tipo e dalle dimensioni.
Serve a stimare quanto di un modello e' quantizzato e quanto in virgola mobile
(docs/design: filtro per tensori in virgola mobile).
"""

import struct
import sys
from collections import defaultdict

# ggml_type -> (nome, elementi per blocco, byte per blocco)
GGML_TYPES = {
    0: ("F32", 1, 4), 1: ("F16", 1, 2), 2: ("Q4_0", 32, 18), 3: ("Q4_1", 32, 20),
    6: ("Q5_0", 32, 22), 7: ("Q5_1", 32, 24), 8: ("Q8_0", 32, 34), 9: ("Q8_1", 32, 36),
    10: ("Q2_K", 256, 84), 11: ("Q3_K", 256, 110), 12: ("Q4_K", 256, 144),
    13: ("Q5_K", 256, 176), 14: ("Q6_K", 256, 210), 15: ("Q8_K", 256, 292),
    16: ("IQ2_XXS", 256, 66), 17: ("IQ2_XS", 256, 74), 18: ("IQ3_XXS", 256, 98),
    19: ("IQ1_S", 256, 50), 20: ("IQ4_NL", 32, 18), 21: ("IQ3_S", 256, 110),
    22: ("IQ2_S", 256, 82), 23: ("IQ4_XS", 256, 136), 24: ("I8", 1, 1), 25: ("I16", 1, 2),
    26: ("I32", 1, 4), 27: ("I64", 1, 8), 28: ("F64", 1, 8), 29: ("IQ1_M", 256, 56),
    30: ("BF16", 1, 2),
}

# tipi dei valori dei metadati GGUF
GGUF_VT = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}


class Reader:
    def __init__(self, f):
        self.f = f

    def read(self, fmt):
        n = struct.calcsize("<" + fmt)
        return struct.unpack("<" + fmt, self.f.read(n))

    def string(self):
        (n,) = self.read("Q")
        return self.f.read(n).decode("utf-8", "replace")

    def value(self, vt):
        if vt == 8:
            return self.string()
        if vt == 9:
            (at, n) = self.read("IQ")
            return [self.value(at) for _ in range(n)]
        return self.read(GGUF_VT[vt])[0]


def main() -> int:
    path = sys.argv[1]
    with open(path, "rb") as f:
        r = Reader(f)
        magic = f.read(4)
        if magic != b"GGUF":
            sys.exit("non e' un file GGUF")
        (version, n_tensors, n_kv) = r.read("IQQ")
        meta = {}
        for _ in range(n_kv):
            key = r.string()
            (vt,) = r.read("I")
            val = r.value(vt)
            if not isinstance(val, list):
                meta[key] = val
        tensors = []
        for _ in range(n_tensors):
            name = r.string()
            (nd,) = r.read("I")
            dims = r.read("Q" * nd)
            (tt, off) = r.read("IQ")
            tensors.append((name, dims, tt, off))
        header_end = f.tell()
        f.seek(0, 2)
        file_size = f.tell()

    align = meta.get("general.alignment", 32)
    data_start = (header_end + align - 1) // align * align
    by_type = defaultdict(lambda: [0, 0])
    for name, dims, tt, off in tensors:
        tname, bl, bb = GGML_TYPES.get(tt, (f"type{tt}", 1, 0))
        n = 1
        for d in dims:
            n *= d
        size = n // bl * bb
        by_type[tname][0] += size
        by_type[tname][1] += 1
        if "--tensors" in sys.argv:
            print(f"{tname:8} {size:12} {name} {dims}")
    total = sum(v[0] for v in by_type.values())
    print(f"{path}\n  GGUF v{version}, {n_tensors} tensori, architettura: {meta.get('general.architecture')}, "
          f"file {file_size / 2**20:.1f} MiB, intestazione {data_start / 2**20:.2f} MiB")
    for t, (size, cnt) in sorted(by_type.items(), key=lambda x: -x[1][0]):
        print(f"  {t:8} {size / 2**20:10.1f} MiB  {100 * size / total:5.1f}%  ({cnt} tensori)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
