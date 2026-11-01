#!/usr/bin/env python3
"""Separazione per piani di byte dei pesi BF16/FP16 di un file safetensors (design 0006).

Uso: tools/bench/safetensors_planes.py <model.safetensors> <dir_uscita>    (richiede numpy)

Scrive in <dir_uscita> i flussi che un filtro "consapevole dei float" darebbe al
compressore, in due varianti, e stampa l'entropia di ordine 0 di ciascuno:
  bytes:  piano del byte alto (segno + 7 bit di esponente) e del byte basso
  exp:    esponente intero (8 bit, BF16) e segno+mantissa (8 bit)
L'intestazione JSON e i tensori di altri tipi vanno nel flusso "other" invariati.
Comprimere i flussi (7zz, xz, zstd) e sommarne le dimensioni dice quanto vale il filtro.
"""

import json
import struct
import sys
from pathlib import Path

import numpy as np


def entropy_bits(values, nsym):
    counts = np.bincount(values.ravel(), minlength=nsym).astype(np.float64)
    p = counts[counts > 0] / counts.sum()
    return float(-(p * np.log2(p)).sum()) * values.size


def main() -> int:
    path, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    with open(path, "rb") as f:
        (hlen,) = struct.unpack("<Q", f.read(8))
        header = json.loads(f.read(hlen))
    base = 8 + hlen
    mm = np.memmap(path, dtype=np.uint8, mode="r")

    streams = {k: open(out / k, "wb") for k in ("bytes.hi", "bytes.lo", "exp.exp", "exp.sm", "other")}
    streams["other"].write(mm[:base].tobytes())
    ent = {k: 0.0 for k in streams}
    nb = {k: 0 for k in streams}
    nb["other"] = base
    ent["other"] = base * 8.0
    dtypes = {}
    for name, t in sorted(header.items(), key=lambda kv: kv[1]["data_offsets"][0] if kv[0] != "__metadata__" else -1):
        if name == "__metadata__":
            continue
        a, b = t["data_offsets"]
        raw = mm[base + a: base + b]
        dtypes[t["dtype"]] = dtypes.get(t["dtype"], 0) + raw.size
        if t["dtype"] in ("BF16", "F16"):
            w = raw.view("<u2")
            hi = (w >> 8).astype(np.uint8)
            lo = (w & 0xFF).astype(np.uint8)
            if t["dtype"] == "BF16":
                e = ((w >> 7) & 0xFF).astype(np.uint8)                   # 8 bit di esponente
                sm = (((w >> 8) & 0x80) | (w & 0x7F)).astype(np.uint8)   # segno + 7 bit di mantissa
            else:
                e, sm = hi, lo
            for k, v in (("bytes.hi", hi), ("bytes.lo", lo), ("exp.exp", e), ("exp.sm", sm)):
                streams[k].write(v.tobytes())
                ent[k] += entropy_bits(v, 256)
                nb[k] += v.size
        else:
            streams["other"].write(raw.tobytes())
            ent["other"] += raw.size * 8.0
            nb["other"] += raw.size
    for s in streams.values():
        s.close()

    total = path.stat().st_size
    print(f"{path}  {total / 2**20:.1f} MiB  tipi: " + ", ".join(f"{k} {v / 2**20:.1f} MiB" for k, v in dtypes.items()))
    print(f"  {'flusso':10} {'MiB':>9} {'bit/byte':>9}")
    for k in streams:
        if nb[k]:
            print(f"  {k:10} {nb[k] / 2**20:9.1f} {ent[k] / nb[k]:9.3f}")
    for var in ("bytes", "exp"):
        bits = ent[f"{var}.hi" if var == "bytes" else "exp.exp"] + ent[f"{var}.lo" if var == "bytes" else "exp.sm"] + ent["other"]
        print(f"  minimo ordine 0, variante {var}: {100 * bits / 8 / total:.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
