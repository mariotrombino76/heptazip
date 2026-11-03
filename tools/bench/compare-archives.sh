#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)
# Verifica che due binari producano archivi IDENTICI byte per byte.
# Serve a dimostrare che una modifica (nuovo build system, flag del compilatore,
# refactoring) non altera il formato/l'output.
#
#   tools/bench/compare-archives.sh <7zz_A> <7zz_B> [corpus_dir]
#
# NB: modifiche che cambiano volutamente l'output dell'encoder (es. nuovo
# match finder) falliranno qui: in quel caso valgono solo roundtrip.sh + bench.

source "$(dirname "$0")/common.sh"

A="$(realpath "${1:?uso: $0 <7zz_A> <7zz_B> [corpus_dir]}")"
B="$(realpath "${2:?uso: $0 <7zz_A> <7zz_B> [corpus_dir]}")"
CORPUS="$(realpath "${3:-$BENCH_DATA/corpus}")"
require_bin "$A"; require_bin "$B"; require_dir "$CORPUS"

work="$BENCH_TMP/compare.$$"
mkdir -p "$work"
trap 'rm -rf "$work"; rmdir "$BENCH_TMP" 2>/dev/null || true' EXIT

fail=0
for entry in "${METHODS_DEFAULT[@]}"; do
  name="${entry%%|*}"; opts="${entry#*|}"
  # AES usa un IV/salt casuale: l'output non e' deterministico per costruzione
  [[ "$opts" == *-p* ]] && { printf '%-20s %s\n' "$name" "SKIP (AES: IV casuale)"; continue; }
  # zstd: encoder del fork, assente in 7-Zip upstream
  if [[ "$opts" == *zstd* ]] && ! { has_encoder "$A" 4F71101 && has_encoder "$B" 4F71101; }; then
    printf '%-20s %s\n' "$name" "SKIP (encoder zstd assente in uno dei binari)"; continue
  fi
  for side in A B; do
    bin=$A; [[ $side == B ]] && bin=$B
    if [[ "$opts" == *-txz* ]]; then
      # xz e' a singolo stream: si comprime un tar (deterministico) del corpus
      [[ -f "$work/corpus.tar" ]] || tar -C "$CORPUS" --sort=name -cf "$work/corpus.tar" .
      # shellcheck disable=SC2086
      "$bin" a $opts -bso0 -bsp0 "$work/$name.$side" "$work/corpus.tar" >/dev/null
    else
      # shellcheck disable=SC2086
      (cd "$CORPUS" && "$bin" a $opts -bso0 -bsp0 "$work/$name.$side" . >/dev/null)
    fi
  done
  if cmp -s "$work/$name.A" "$work/$name.B"; then
    printf '%-20s %s\n' "$name" "IDENTICI"
  else
    printf '%-20s %s\n' "$name" "DIVERSI"; fail=1
  fi
  rm -f "$work/$name.A" "$work/$name.B"
done

(( fail == 0 )) && info "Output identico per tutti i profili" || die "gli archivi differiscono"
