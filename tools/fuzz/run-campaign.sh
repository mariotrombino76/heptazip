#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
# Campagna di fuzzing su tutti i formati (design 0007).
#
#   tools/fuzz/run-campaign.sh <fuzz_archive> <dir_lavoro> [secondi_per_formato] [processi]
#
# <dir_lavoro>/seeds/<formato>  semi (tools/fuzz/make-seeds.sh), facoltativi
# <dir_lavoro>/corpus/<formato> corpus che cresce di campagna in campagna
# <dir_lavoro>/crash/<formato>  input che fanno crashare (crash-*, leak-*, timeout-*, oom-*)
# <dir_lavoro>/log/<formato>.log
# Un processo libFuzzer per formato, fino a [processi] in parallelo (default: nproc).
# FUZZ_FORMATS="iso udf ..." limita la campagna a quei formati; FUZZ_CRASH=crash4 cambia
# la directory degli artefatti (per tenere separati quelli delle campagne precedenti).
# Per fuzz_codec: FUZZ_VAR=FUZZ_CODEC, FUZZ_FORMATS="LZMA LZMA2 ..." e FUZZ_DIRS=codec-
# (directory codec-seeds, codec-corpus, codec-log, codec-<FUZZ_CRASH>).
set -euo pipefail

FUZZER="$(realpath "${1:?uso: $0 <fuzz_archive> <dir> [sec] [proc]}")"
WORK="$(realpath -m "${2:?uso: $0 <fuzz_archive> <dir> [sec] [proc]}")"
SECS="${3:-300}"
JOBS="${4:-$(nproc)}"

FORMATS="${FUZZ_FORMATS:-7z zip tar gzip bzip2 xz zstd lzma lzma86 Z wim cab chm hxs rar rar5 arj lzh cpio ar rpm
xar iso udf nsis dmg hfs apm apfs ext fat ntfs squashfs cramfs vhd vhdx vdi vmdk qcow sparse lp
mbr gpt compound mslz ppmd flv swf swfc uefic uefif te pe elf macho mub coff ihex base64}"
VAR="${FUZZ_VAR:-FUZZ_FORMAT}"
P="${FUZZ_DIRS:-}"
CRASH="$P${FUZZ_CRASH:-crash}"

mkdir -p "$WORK/${P}log"
run_one() {
  local fmt="$1"
  mkdir -p "$WORK/${P}corpus/$fmt" "$WORK/$CRASH/$fmt"
  local seeds=()
  [ -d "$WORK/${P}seeds/$fmt" ] && seeds=("$WORK/${P}seeds/$fmt")
  # -malloc_limit_mb=4200: LZMA e zstd allocano il dizionario/la finestra dichiarati
  # dall'intestazione (fino a 4 GiB, come xz e zstd senza limite di memoria): con un limite
  # piu' basso ogni campagna si fermerebbe su questi casi attesi. Le allocazioni non toccate
  # non consumano RAM; -rss_limit_mb cattura i consumi reali.
  env "$VAR=$fmt" "$FUZZER" -malloc_limit_mb=4200 -rss_limit_mb=2560 -timeout=20 \
      -max_total_time="$SECS" -print_final_stats=1 -artifact_prefix="$WORK/$CRASH/$fmt/" \
      "$WORK/${P}corpus/$fmt" "${seeds[@]}" > "$WORK/${P}log/$fmt.log" 2>&1 || true
  local n; n=$(find "$WORK/$CRASH/$fmt" -type f | wc -l)
  printf '%-10s corpus %5d  artefatti %d  %s\n' "$fmt" "$(find "$WORK/${P}corpus/$fmt" -type f | wc -l)" "$n" \
      "$(grep -m1 -o 'stat::number_of_executed_units: *[0-9]*' "$WORK/${P}log/$fmt.log" | awk '{print $2" esecuzioni"}')"
}
export -f run_one
export FUZZER WORK SECS CRASH VAR P
printf '%s\n' $FORMATS | xargs -P "$JOBS" -I{} bash -c 'run_one {}'
