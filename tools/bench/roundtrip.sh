#!/usr/bin/env bash
# Test di correttezza: per ogni profilo comprime il corpus, verifica l'archivio
# (7zz t), estrae e confronta byte per byte con l'originale.
#
#   tools/bench/roundtrip.sh <7zz> [corpus_dir]
#
# Exit code != 0 al primo errore. Da eseguire dopo OGNI modifica al codice.
# X_OPTS: opzioni aggiuntive per i comandi t/x degli archivi .7z, es. X_OPTS="-mpfd=on".
# Non si applicano a zip/xz: le opzioni -m sono specifiche del formato (come -mtf).

source "$(dirname "$0")/common.sh"
X_OPTS="${X_OPTS:-}"

BIN="$(realpath "${1:?uso: $0 <7zz> [corpus_dir]}")"
CORPUS="$(realpath "${2:-$BENCH_DATA/corpus}")"
require_bin "$BIN"; require_dir "$CORPUS"

work="$BENCH_TMP/roundtrip.$$"
mkdir -p "$work"
trap 'rm -rf "$work"; rmdir "$BENCH_TMP" 2>/dev/null || true' EXIT

fail=0
for entry in "${METHODS_DEFAULT[@]}"; do
  name="${entry%%|*}"; opts="${entry#*|}"
  ext=7z; [[ "$opts" == *-tzip* ]] && ext=zip
  arc="$work/$name.$ext"

  # xz e' un formato a singolo stream: si comprime un tar del corpus
  if [[ "$opts" == *-txz* ]]; then
    arc="$work/$name.tar.xz"
    tar -C "$CORPUS" -cf "$work/corpus.tar" .
    # shellcheck disable=SC2086
    "$BIN" a $opts -bso0 -bsp0 "$arc" "$work/corpus.tar" >/dev/null
    "$BIN" t -bso0 -bsp0 "$arc" >/dev/null
    mkdir -p "$work/out"
    "$BIN" x -bso0 -bsp0 -o"$work/out" "$arc" >/dev/null
    # .xz non memorizza il nome del file: l'estratto si chiama come l'archivio senza .xz
    if cmp -s "$work/corpus.tar" "$work/out/$name.tar"; then r=OK; else r=FAIL; fail=1; fi
    printf '%-20s %-4s %12d byte\n' "$name" "$r" "$(stat -c %s "$arc")"
    rm -rf "$work/out" "$work/corpus.tar" "$arc"
    continue
  fi

  pw=(); [[ "$opts" == *-p* ]] && pw=(-psegreto)
  xopts=""; [[ "$ext" == 7z ]] && xopts="$X_OPTS"
  # shellcheck disable=SC2086
  (cd "$CORPUS" && "$BIN" a $opts -bso0 -bsp0 "$arc" . >/dev/null)
  "$BIN" t $xopts "${pw[@]}" -bso0 -bsp0 "$arc" >/dev/null
  mkdir -p "$work/out"
  "$BIN" x $xopts "${pw[@]}" -bso0 -bsp0 -o"$work/out" "$arc" >/dev/null
  if diff -rq "$CORPUS" "$work/out" >/dev/null; then r=OK; else r=FAIL; fail=1; fi
  printf '%-20s %-4s %12d byte\n' "$name" "$r" "$(stat -c %s "$arc")"
  rm -rf "$work/out" "$arc"
done

(( fail == 0 )) && info "Tutti i roundtrip OK" || die "roundtrip FALLITO"
