#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Limite teorico di un filtro "consapevole di GGUF" per i blocchi Q4_K e Q6_K.

Uso: tools/bench/gguf_entropy.py <file.gguf>     (richiede numpy)

Separa i campi dei blocchi quantizzati in flussi omogenei e ne calcola l'entropia di
ordine 0 (bit per simbolo):
  Q4_K (144 byte, 256 pesi): d e dmin (fp16), 12 byte di sottoscale, 256 valori a 4 bit
  Q6_K (210 byte, 256 pesi): 256 valori a 6 bit (ql 4 bit + qh 2 bit), 16 scale int8, d (fp16)
La somma delle entropie e' la dimensione minima con un codificatore entropico semplice
per flusso: confrontata con LZMA/zstd sul file intero dice quanto margine ha un filtro.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gguf_info import GGML_TYPES, Reader  # noqa: E402


def entropy_bits(values, nsym):
    counts = np.bincount(values.ravel(), minlength=nsym).astype(np.float64)
    p = counts[counts > 0] / counts.sum()
    return float(-(p * np.log2(p)).sum()) * values.size


def read_tensors(path):
    with open(path, "rb") as f:
        r = Reader(f)
        f.read(4)
        (version, n_tensors, n_kv) = r.read("IQQ")
        align = 32
        for _ in range(n_kv):
            key = r.string()
            (vt,) = r.read("I")
            val = r.value(vt)
            if key == "general.alignment":
                align = val
        tensors = []
        for _ in range(n_tensors):
            name = r.string()
            (nd,) = r.read("I")
            dims = r.read("Q" * nd)
            (tt, off) = r.read("IQ")
            tensors.append((name, dims, tt, off))
        data_start = (f.tell() + align - 1) // align * align
    return tensors, data_start


def main() -> int:
    path = sys.argv[1]
    tensors, data_start = read_tensors(path)
    mm = np.memmap(path, dtype=np.uint8, mode="r")
    acc = {}  # flusso -> [bit di entropia, byte originali]

    def add(stream, bits, nbytes):
        a = acc.setdefault(stream, [0.0, 0])
        a[0] += bits
        a[1] += nbytes

    other_bytes = 0
    for name, dims, tt, off in tensors:
        tname, bl, bb = GGML_TYPES.get(tt, ("?", 1, 0))
        n = 1
        for d in dims:
            n *= d
        size = n // bl * bb
        raw = mm[data_start + off: data_start + off + size]
        if tname == "Q4_K":
            b = raw.reshape(-1, 144)
            d16 = b[:, 0:4].copy().view(np.uint16)          # d, dmin
            add("Q4_K fp16 d/dmin", entropy_bits(d16, 65536), d16.nbytes)
            add("Q4_K sottoscale (12 B)", entropy_bits(b[:, 4:16], 256), b[:, 4:16].size)
            qs = b[:, 16:144]
            nib = np.concatenate([qs & 15, qs >> 4], axis=1)
            add("Q4_K valori 4 bit", entropy_bits(nib, 16), qs.size)
        elif tname == "Q6_K":
            b = raw.reshape(-1, 210)
            ql = b[:, 0:128]
            qh = b[:, 128:192]
            # 6-bit values: low 4 bits from ql nibbles, high 2 bits from qh (4 per byte)
            lo = np.concatenate([ql & 15, ql >> 4], axis=1)            # 256 nibbles
            hi = np.concatenate([(qh >> s) & 3 for s in (0, 2, 4, 6)], axis=1)  # 256 x 2 bit
            q6 = (lo | (hi << 4)).astype(np.uint8)
            add("Q6_K valori 6 bit", entropy_bits(q6, 64), ql.size + qh.size)
            add("Q6_K scale int8 (16 B)", entropy_bits(b[:, 192:208], 256), b[:, 192:208].size)
            d16 = b[:, 208:210].copy().view(np.uint16)
            add("Q6_K fp16 d", entropy_bits(d16, 65536), d16.nbytes)
        else:
            other_bytes += size

    total_bytes = sum(v[1] for v in acc.values()) + other_bytes
    total_bits = sum(v[0] for v in acc.values()) + other_bytes * 8
    print(f"{path}")
    print(f"  {'flusso':26} {'MiB':>9} {'bit/byte':>9} {'min MiB':>9}")
    for k, (bits, nb) in sorted(acc.items(), key=lambda x: -x[1][1]):
        print(f"  {k:26} {nb / 2**20:9.1f} {bits / nb:9.3f} {bits / 8 / 2**20:9.1f}")
    print(f"  altri tensori (F32...)   {other_bytes / 2**20:9.1f}")
    print(f"  TOTALE tensori {total_bytes / 2**20:.1f} MiB -> minimo ordine 0: {total_bits / 8 / 2**20:.1f} MiB "
          f"({100 * total_bits / 8 / total_bytes:.2f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
