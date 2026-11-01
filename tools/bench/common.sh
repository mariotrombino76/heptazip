# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
# Funzioni e configurazione condivise dagli script in tools/bench.
# shellcheck shell=bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BENCH_DATA="${BENCH_DATA:-$REPO_ROOT/bench-data}"

# Directory di lavoro per archivi temporanei: /dev/shm (RAM) se disponibile,
# cosi' i tempi misurano la CPU e non il disco.
if [[ -z "${BENCH_TMP:-}" ]]; then
  if [[ -d /dev/shm && -w /dev/shm ]]; then
    BENCH_TMP="/dev/shm/7zip-bench.$USER"
  else
    BENCH_TMP="$BENCH_DATA/tmp"
  fi
fi

die()  { echo "ERRORE: $*" >&2; exit 1; }
info() { echo ">> $*" >&2; }

require_bin() {
  [[ -x "$1" ]] || die "eseguibile non trovato: $1"
}

# true se il binario ha l'encoder del metodo con ID $2 (colonna "E" di "7zz i")
has_encoder() {
  "$1" i 2>/dev/null | grep -qE "^[[:space:]]*E[^[:space:]]*[[:space:]]+$2[[:space:]]"
}

require_dir() {
  [[ -d "$1" ]] || die "directory non trovata: $1 (generala con tools/bench/make-corpus.sh)"
}

# Profili di compressione usati da roundtrip/compare/bench.
# Formato: "<nome>|<opzioni 7zz>". Il numero di thread e' sempre esplicito
# per rendere l'output deterministico e confrontabile tra binari.
METHODS_DEFAULT=(
  "7z-lzma2-mx1|-t7z -m0=lzma2 -mx1 -mmt=4"
  "7z-lzma2-mx5|-t7z -m0=lzma2 -mx5 -mmt=4"
  "7z-lzma2-mx9-st|-t7z -m0=lzma2 -mx9 -mmt=1"
  "7z-lzma2-mx9-mt|-t7z -m0=lzma2 -mx9 -mmt=8"
  "7z-lzma-mx5|-t7z -m0=lzma -mx5 -mmt=2"
  "7z-ppmd|-t7z -m0=ppmd -mx7"
  "7z-bzip2|-t7z -m0=bzip2 -mx5 -mmt=4"
  "7z-nonsolid|-t7z -m0=lzma2 -mx5 -ms=off -mmt=4"
  "7z-solid-ext|-t7z -m0=lzma2 -mx5 -ms=e -mmt=4"
  "7z-smallsolid|-t7z -m0=lzma2 -mx5 -ms=8m -mmt=4"
  "7z-bcj2|-t7z -mf=BCJ2 -mx5 -mmt=4"
  "7z-aes|-t7z -mx3 -mmt=4 -mhe=on -psegreto"
  "7z-nonsolid-aes|-t7z -mx3 -ms=off -mmt=4 -psegreto"
  "7z-zstd|-t7z -m0=zstd -mx3 -mmt=4"
  "7z-zstd-nonsolid|-t7z -m0=zstd -mx9 -ms=off -mmt=4"
  "zip-deflate|-tzip -mx5 -mmt=4"
  "xz|-txz -mx5 -mmt=4"
)

# METHODS_FILTER=<regex bash> limita i profili eseguiti, es.:
#   METHODS_FILTER='lzma2' tools/bench/roundtrip.sh build/7zz
if [[ -n "${METHODS_FILTER:-}" ]]; then
  _filtered=()
  for _m in "${METHODS_DEFAULT[@]}"; do
    [[ "${_m%%|*}" =~ $METHODS_FILTER ]] && _filtered+=("$_m")
  done
  METHODS_DEFAULT=("${_filtered[@]}")
  unset _filtered _m
fi
