#!/usr/bin/env bash
# Build con Profile-Guided Optimization (GCC/Clang) in tre fasi:
#   1. build strumentata  (SEVENZIP_PGO=GEN)  in build-pgo-gen/
#   2. training: compressione/decompressione del corpus con i profili principali
#   3. build ottimizzata  (SEVENZIP_PGO=USE)  in build-pgo/
#
#   tools/pgo.sh [opzioni cmake aggiuntive...]
#   es: tools/pgo.sh -DSEVENZIP_LTO=ON -DSEVENZIP_NATIVE=ON
#
# Il risultato e' build-pgo/7zz. Il training usa bench-data/corpus
# (tools/bench/make-corpus.sh): il profilo rispecchia quel carico di lavoro.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tools/bench/common.sh"
CORPUS="$BENCH_DATA/corpus"
require_dir "$CORPUS"

GEN="$ROOT/build-pgo-gen"
USE="$ROOT/build-pgo"
PROFDIR="$ROOT/build-pgo-data"
JOBS="$(nproc)"

rm -rf "$PROFDIR"
info "1/3 build strumentata"
cmake -S "$ROOT" -B "$GEN" -DCMAKE_BUILD_TYPE=Release -DSEVENZIP_PGO=GEN \
      -DSEVENZIP_PGO_DIR="$PROFDIR" "$@" >/dev/null
cmake --build "$GEN" -j"$JOBS" >/dev/null

info "2/3 training"
work="$BENCH_TMP/pgo.$$"; mkdir -p "$work"; trap 'rm -rf "$work"' EXIT
for entry in "${METHODS_DEFAULT[@]}"; do
  name="${entry%%|*}"; opts="${entry#*|}"
  [[ "$opts" == *-txz* ]] && continue
  info "   $name"
  pw=(); [[ "$opts" == *-p* ]] && pw=(-psegreto)
  # estensione esplicita: senza, 7zz aggiunge ".7z" da solo
  arc="$work/a.7z"; [[ "$opts" == *-tzip* ]] && arc="$work/a.zip"
  # shellcheck disable=SC2086
  (cd "$CORPUS" && "$GEN/7zz" a $opts -bso0 -bsp0 "$arc" . >/dev/null)
  "$GEN/7zz" t "${pw[@]}" -bso0 -bsp0 "$arc" >/dev/null
  rm -f "$arc"
done
# benchmark interno: esercita LZMA a vari dizionari e i percorsi multi-thread
"$GEN/7zz" b -md22 >/dev/null

info "3/3 build ottimizzata"
cmake -S "$ROOT" -B "$USE" -DCMAKE_BUILD_TYPE=Release -DSEVENZIP_PGO=USE \
      -DSEVENZIP_PGO_DIR="$PROFDIR" "$@" >/dev/null
cmake --build "$USE" -j"$JOBS" >/dev/null
info "Fatto: $USE/7zz"
