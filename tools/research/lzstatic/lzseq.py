#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Research (issue #16): the LZMA parse as sequences (literal run + match), as zstd.

    lzseq.py <input> <tokens.bin> <lzma_size>

Instead of one LIT/MATCH decision per literal (LZMA isMatch), one sequence per match:
  runlen  literals before the match: code (0..15 direct, then 2 per power of 2) +
          extra bits; context: the previous match type (0..5)
  mtype   MATCH, REP0, SHORTREP, REP1, REP2, REP3; context: previous mtype * 2 +
          (runlen == 0)
  lens, distances, literals as lzstatic.py (literals without context, XOR after a
  match: the first literal of the run)
Static tables per block (entropy + table cost)."""

import os
import sys

NOCROSS = os.environ.get("LZS_NOCROSS") == "1"   # no context from another stream
JOINT = os.environ.get("LZS_JOINT") == "1"       # (length class, slot) as one symbol
RUNPOS = os.environ.get("LZS_RUNPOS") == "1"     # posState of the start of the run


def len_class(n):
    """length - 2 -> class (0..15 direct, then 2 per power of 2) and extra bits"""
    if n < 16:
        return n, 0
    b = n.bit_length() - 1
    return 16 + 2 * (b - 4) + ((n >> (b - 1)) & 1), b - 1

import numpy as np

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzstatic  # noqa: E402


def code_of(n):
    """length code, extra bits (zstd-like)"""
    if n < 16:
        return n, 0, 0
    b = n.bit_length() - 1          # n >= 16 -> b >= 4
    half = (n >> (b - 1)) & 1
    return 16 + 2 * (b - 4) + half, b - 1, n & ((1 << (b - 1)) - 1)


def main():
    data = np.fromfile(sys.argv[1], dtype=np.uint8)
    tok = np.fromfile(sys.argv[2], dtype=np.dtype([("len", "<u2"), ("back", "<u4")]))
    lzma_size = int(sys.argv[3])
    lens = tok["len"].astype(np.int64).tolist()
    backs = tok["back"].astype(np.int64).tolist()
    out = {k: [] for k in ("runlen", "mtype", "lit", "mlitx", "len", "replen", "slot", "foot", "align")}
    raw = 0
    pos = 1
    run = 1                      # the first byte is a literal
    out["lit"].append((0, 0, int(data[0])))
    prev_type = 0
    after_match = False
    reps = [1, 1, 1, 1]
    for ln, b in zip(lens, backs):
        if b == 0xFFFFFFFF:
            if after_match:
                out["mlitx"].append((pos, 0, int(data[pos]) ^ int(data[pos - reps[0]])))
            else:
                out["lit"].append((pos, 0, int(data[pos])))
            after_match = False
            run += 1
            pos += 1
            continue
        code, nb, extra = code_of(run)
        out["runlen"].append((pos, prev_type, code))
        raw += nb
        if b < 4:
            if b == 0 and ln == 1:
                t = 2
            elif b == 0:
                t = 1
            else:
                t = 2 + b
                d = reps[b]
                del reps[b]
                reps.insert(0, d)
        else:
            t = 0
        out["mtype"].append((pos, prev_type if NOCROSS else prev_type * 2 + (run == 0), t))
        if t == 0:
            dist = b - 4
            slot = lzstatic.pos_slot(dist)
            ps = (pos - run) & 3 if RUNPOS else pos & 3
            if JOINT:
                lc, lnb = len_class(ln - 2)
                raw += lnb
                out["len"].append((pos, 0 if NOCROSS else ps, lc * 64 + slot))
            else:
                out["len"].append((pos, 0 if NOCROSS else ps, ln - 2))
                out["slot"].append((pos, 0 if NOCROSS else min(ln - 2, 3), slot))
            if slot >= 4:
                fb = (slot >> 1) - 1
                extra = dist - ((2 | (slot & 1)) << fb)
                if slot < 14:
                    out["foot"].append((pos, slot, extra))
                else:
                    raw += fb - 4
                    out["align"].append((pos, 0, extra & 15))
            reps = [dist + 1] + reps[:3]
        elif t != 2:
            out["replen"].append((pos, 0 if NOCROSS else pos & 3, ln - 2))
        prev_type = t
        run = 0
        after_match = True
        pos += ln
    # the final literal run: one more runlen
    code, nb, extra = code_of(run)
    out["runlen"].append((pos, prev_type, code))
    raw += nb
    nsyms = {"runlen": 80, "mtype": 6, "lit": 256, "mlitx": 256, "len": 64 * 40 if JOINT else 272, "replen": 272,
             "slot": 64, "foot": 64, "align": 16}
    for block in (262144, 1048576):
        res = {}
        for k, v in out.items():
            a = np.array(v, dtype=np.int64).reshape(-1, 3)
            res[k] = lzstatic.stream_bits(a, block, nsyms[k])
        total = (sum(res.values()) + raw) / 8
        print("block %8d  %10d byte  %.3f%%  vs lzma %+.2f%%" % (block, total, 100.0 * total / len(data),
                                                          100.0 * (total / lzma_size - 1)))
        print("   " + ", ".join("%s %.0f" % (k, v / 8) for k, v in res.items()) + ", raw %d" % (raw // 8))
    print("sequenze %d, token LZMA %d" % (len(out["mtype"]), len(lens)))


if __name__ == "__main__":
    main()
