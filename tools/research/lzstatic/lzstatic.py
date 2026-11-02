#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
"""Research (issue #16): size of the LZMA parse with static entropy coding per block.

    lzstatic.py <input> <tokens.bin> <lzma_size> [--blocks 65536,262144,...]

The tokens come from lztrace.c (the decisions of the LZMA optimal parser). They are
split in streams, as a format with static tables per block would code them:

  type     LIT, MATCH, REP0 (len >= 2), SHORTREP (rep0, len 1), REP1, REP2, REP3;
           context: the LZMA state (12 states, from the last tokens)
  literal  context: high 3 bits of the previous byte (lc = 3); after a match
           ("matched literal", LZMA state >= 7) one of:
             plain  the same tables, separate contexts
             xor    the literal XOR the byte at rep0 (the "match byte")
  len      len - 2 for MATCH and REP* (separate tables)
  slot     distance slot (64), context min(len - 2, 3) as LZMA
  footer   the low bits of the distance: slots 4..13 modeled per slot; slots >= 14:
           (bits - 4) raw bits + 4 "align" bits modeled (as LZMA)

The size of a block is the entropy of each table (what rANS reaches within ~0.1%)
plus the cost of the tables (an estimate: 5 bits per used symbol + 8 per table; an
order-0 Huffman would add ~2-4%, see --huffman).
"""

import argparse
import os
import math
import sys

import numpy as np

K_LIT_NEXT = [0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5]
K_MATCH_NEXT = [7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10]
K_REP_NEXT = [8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11]
K_SHORTREP_NEXT = [9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11]

T_LIT, T_MATCH, T_REP0, T_SHORTREP, T_REP1, T_REP2, T_REP3 = range(7)

LC = int(os.environ.get("LZS_LC", "3"))   # bits of the previous byte in the literal context
MC = int(os.environ.get("LZS_MC", "0"))   # bits of the match byte in the context of the XOR literals
LC_MODE = os.environ.get("LZS_LCMODE", "prevbyte")   # prevbyte | prevlit
PB = int(os.environ.get("LZS_PB", "0"))   # bits of the position in the type and len contexts (LZMA: 2)


def pos_slot(dist):
    """LZMA distance slot of (dist - 1) = d (d = 0 for distance 1)."""
    if dist < 4:
        return dist
    n = dist.bit_length() - 1
    return 2 * n + ((dist >> (n - 1)) & 1)


def parse(data, tok):
    """Streams with (block position, context, symbol); raw extra bits."""
    n = len(tok)
    lens = tok["len"].astype(np.int64)
    backs = tok["back"].astype(np.int64)
    # stream arrays (filled in a Python loop: the reps need the order)
    out = {k: [] for k in ("type", "lit", "mlit", "mlitx", "len", "replen", "slot", "foot", "align")}
    raw_bits = 0
    pos = 1                     # the first byte is a literal without a record
    out["lit"].append((0, 0, data[0]))
    state = 0
    prev_lit = data[0]
    reps = [1, 1, 1, 1]         # distances (1 = previous byte)
    for i in range(n):
        ln = int(lens[i])
        b = int(backs[i])
        if b == 0xFFFFFFFF:
            if LC_MODE == "prevlit":
                ctx_prev = prev_lit >> (8 - LC)
            else:
                ctx_prev = data[pos - 1] >> (8 - LC) if LC else 0
            out["type"].append((pos, state * 4 + (pos & ((1 << PB) - 1)), T_LIT))
            if state < 7:
                out["lit"].append((pos, ctx_prev, data[pos]))
                prev_lit = data[pos]
            else:
                mb = data[pos - reps[0]]
                out["mlit"].append((pos, ctx_prev, data[pos]))
                out["mlitx"].append((pos, mb >> (8 - MC), data[pos] ^ mb))
            state = K_LIT_NEXT[state]
            pos += 1
            continue
        if b < 4:
            if b == 0:
                if ln == 1:
                    out["type"].append((pos, state * 4 + (pos & ((1 << PB) - 1)), T_SHORTREP))
                    state = K_SHORTREP_NEXT[state]
                    pos += 1
                    continue
                out["type"].append((pos, state * 4 + (pos & ((1 << PB) - 1)), T_REP0))
            else:
                out["type"].append((pos, state * 4 + (pos & ((1 << PB) - 1)), T_REP0 + 1 + b))
                d = reps[b]
                del reps[b]
                reps.insert(0, d)
            out["replen"].append((pos, pos & ((1 << PB) - 1), ln - 2))
            state = K_REP_NEXT[state]
            pos += ln
            continue
        dist = b - 4                # distance - 1
        out["type"].append((pos, state * 4 + (pos & ((1 << PB) - 1)), T_MATCH))
        out["len"].append((pos, pos & ((1 << PB) - 1), ln - 2))
        slot = pos_slot(dist)
        out["slot"].append((pos, min(ln - 2, 3), slot))
        if slot >= 4:
            fb = (slot >> 1) - 1
            base = (2 | (slot & 1)) << fb
            extra = dist - base
            if slot < 14:
                out["foot"].append((pos, slot, extra))
            else:
                raw_bits += fb - 4
                out["align"].append((pos, 0, extra & 15))
        reps = [dist + 1] + reps[:3]
        state = K_MATCH_NEXT[state]
        pos += ln
    assert pos == len(data), (pos, len(data))
    arrays = {}
    for k, v in out.items():
        a = np.array(v, dtype=np.int64).reshape(-1, 3)
        arrays[k] = a
    return arrays, raw_bits


def table_bits(counts, huffman=False):
    """Bits of the symbols of one table (entropy) + cost of the table."""
    c = counts[counts > 0].astype(np.float64)
    tot = c.sum()
    if tot == 0:
        return 0.0
    if huffman:
        bits = huffman_bits(c)
    else:
        bits = float((c * np.log2(tot / c)).sum())
    return bits + 5.0 * len(c) + 8.0


def huffman_bits(c):
    import heapq
    if len(c) == 1:
        return float(c[0])
    h = [(float(x), i, 0) for i, x in enumerate(c)]
    heapq.heapify(h)
    total = 0.0
    while len(h) > 1:
        a = heapq.heappop(h)
        b = heapq.heappop(h)
        total += a[0] + b[0]
        heapq.heappush(h, (a[0] + b[0], -1, 0))
    return total


def stream_bits(a, block, nsym, huffman=False):
    """Total bits of a stream: tables per (block, context)."""
    if len(a) == 0:
        return 0.0
    blk = a[:, 0] // block
    key = (blk * 4096 + a[:, 1]) * nsym + a[:, 2]
    uk, cnt = np.unique(key, return_counts=True)
    tab = uk // nsym
    total = 0.0
    # group counts by table
    starts = np.flatnonzero(np.r_[True, tab[1:] != tab[:-1]])
    ends = np.r_[starts[1:], len(tab)]
    for s, e in zip(starts, ends):
        total += table_bits(cnt[s:e], huffman)
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("tokens")
    ap.add_argument("lzma_size", type=int)
    ap.add_argument("--blocks", default="65536,262144,1048576,4194304")
    ap.add_argument("--huffman", action="store_true")
    args = ap.parse_args()
    data = np.fromfile(args.input, dtype=np.uint8)
    tok = np.fromfile(args.tokens, dtype=np.dtype([("len", "<u2"), ("back", "<u4")]))
    streams, raw_bits = parse(data, tok)
    print("input %d  tokens %d  lzma %d (%.3f%%)" % (len(data), len(tok), args.lzma_size,
                                                     100.0 * args.lzma_size / len(data)))
    nsyms = {"type": 7, "lit": 256, "mlit": 256, "mlitx": 256, "len": 272, "replen": 272,
             "slot": 64, "foot": 64, "align": 16}
    for block in [int(x) for x in args.blocks.split(",")]:
        res = {k: stream_bits(v, block, nsyms[k], args.huffman) for k, v in streams.items()}
        common = res["type"] + res["lit"] + res["len"] + res["replen"] + res["slot"] + res["foot"] + res["align"] + raw_bits
        for variant, ml in (("plain", res["mlit"]), ("xor", res["mlitx"])):
            total = (common + ml) / 8
            print("block %8d  %-5s  %10d byte  %.3f%%  vs lzma %+.2f%%" %
                  (block, variant, total, 100.0 * total / len(data), 100.0 * (total / args.lzma_size - 1)))
        parts = ", ".join("%s %.0f" % (k, v / 8) for k, v in res.items())
        print("   streams (byte): %s, raw %d" % (parts, raw_bits // 8))


if __name__ == "__main__":
    sys.exit(main())
