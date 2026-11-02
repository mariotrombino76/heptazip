#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Research (issue #16): per stream, static tables per block vs an adaptive model.

    adaptive.py <input> <tokens.bin>

Same streams and contexts as lzstatic.py (LZS_LC). The adaptive model is a
multi-symbol frequency model per context (initial count 1, +32 per symbol, halved
over 2^16), coded at its ideal cost: it tells where adaptivity matters (LZMA is
adaptive, bit by bit)."""

import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzstatic  # noqa: E402


LIMIT = int(os.environ.get("LZS_LIMIT", str(1 << 16)))


def adaptive_bits(a, nsym, inc=32, limit=None):
    limit = limit or LIMIT
    tables = {}
    bits = 0.0
    log2 = math.log2
    for ctx, sym in zip(a[:, 1].tolist(), a[:, 2].tolist()):
        t = tables.get(ctx)
        if t is None:
            t = [[1] * nsym, nsym]
            tables[ctx] = t
        cnt, tot = t
        bits -= log2(cnt[sym] / tot)
        cnt[sym] += inc
        tot += inc
        if tot > limit:
            tot = 0
            for i in range(nsym):
                cnt[i] = (cnt[i] + 1) >> 1
                tot += cnt[i]
        t[1] = tot
    return bits


def main():
    data = np.fromfile(sys.argv[1], dtype=np.uint8)
    tok = np.fromfile(sys.argv[2], dtype=np.dtype([("len", "<u2"), ("back", "<u4")]))
    streams, raw_bits = lzstatic.parse(data, tok)
    nsyms = {"type": 7, "lit": 256, "mlit": 256, "mlitx": 256, "len": 272, "replen": 272,
             "slot": 64, "foot": 64, "align": 16}
    print("%-7s %12s %12s %12s %8s" % ("stream", "adattivo", "statico 256K", "statico 1M", "diff"))
    ta = ts = 0
    for k, a in streams.items():
        ad = adaptive_bits(a, nsyms[k]) / 8
        s256 = lzstatic.stream_bits(a, 262144, nsyms[k]) / 8
        s1m = lzstatic.stream_bits(a, 1 << 20, nsyms[k]) / 8
        print("%-7s %12.0f %12.0f %12.0f %+7.1f%%" % (k, ad, s256, s1m, 100.0 * (min(s256, s1m) / ad - 1)))
        if k != "mlit":
            ta += ad
            ts += min(s256, s1m)
    print("totale (xor), senza bit grezzi: adattivo %.0f  statico %.0f  (%+.2f%%); bit grezzi %d" %
          (ta, ts, 100.0 * (ts / ta - 1), raw_bits // 8))


if __name__ == "__main__":
    main()
