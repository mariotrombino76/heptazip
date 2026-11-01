#!/usr/bin/env bash
# Campagna di fuzzing su tutti i formati (design 0007).
#
#   tools/fuzz/run-campaign.sh <fuzz_archive> <dir_lavoro> [secondi_per_formato] [processi]
#
# <dir_lavoro>/seeds/<formato>  semi (tools/fuzz/make-seeds.sh), facoltativi
# <dir_lavoro>/corpus/<formato> corpus che cresce di campagna in campagna
# <dir_lavoro>/crash/<formato>  input che fanno crashare (crash-*, leak-*, timeout-*, oom-*)
# <dir_lavoro>/log/<formato>.log
# Un processo libFuzzer per formato, fino a [processi] in parallelo (default: nproc).
set -euo pipefail

FUZZER="$(realpath "${1:?uso: $0 <fuzz_archive> <dir> [sec] [proc]}")"
WORK="$(realpath -m "${2:?uso: $0 <fuzz_archive> <dir> [sec] [proc]}")"
SECS="${3:-300}"
JOBS="${4:-$(nproc)}"

FORMATS="7z zip tar gzip bzip2 xz zstd lzma lzma86 Z wim cab chm hxs rar rar5 arj lzh cpio ar rpm
xar iso udf nsis dmg hfs apm apfs ext fat ntfs squashfs cramfs vhd vhdx vdi vmdk qcow sparse lp
mbr gpt compound mslz ppmd flv swf swfc uefic uefif te pe elf macho mub coff ihex base64"

mkdir -p "$WORK/log"
run_one() {
  local fmt="$1"
  mkdir -p "$WORK/corpus/$fmt" "$WORK/crash/$fmt"
  local seeds=()
  [ -d "$WORK/seeds/$fmt" ] && seeds=("$WORK/seeds/$fmt")
  # -malloc_limit_mb=4200: LZMA e zstd allocano il dizionario/la finestra dichiarati
  # dall'intestazione (fino a 4 GiB, come xz e zstd senza limite di memoria): con un limite
  # piu' basso ogni campagna si fermerebbe su questi casi attesi. Le allocazioni non toccate
  # non consumano RAM; -rss_limit_mb cattura i consumi reali.
  FUZZ_FORMAT="$fmt" "$FUZZER" -malloc_limit_mb=4200 -rss_limit_mb=2560 -timeout=20 \
      -max_total_time="$SECS" -print_final_stats=1 -artifact_prefix="$WORK/crash/$fmt/" \
      "$WORK/corpus/$fmt" "${seeds[@]}" > "$WORK/log/$fmt.log" 2>&1 || true
  local n; n=$(find "$WORK/crash/$fmt" -type f | wc -l)
  printf '%-10s corpus %5d  artefatti %d  %s\n' "$fmt" "$(find "$WORK/corpus/$fmt" -type f | wc -l)" "$n" \
      "$(grep -m1 -o 'stat::number_of_executed_units: *[0-9]*' "$WORK/log/$fmt.log" | awk '{print $2" esecuzioni"}')"
}
export -f run_one
export FUZZER WORK SECS
printf '%s\n' $FORMATS | xargs -P "$JOBS" -I{} bash -c 'run_one {}'
