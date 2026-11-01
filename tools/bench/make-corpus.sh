#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
# Crea il corpus di test in bench-data/corpus (o $BENCH_DATA/corpus).
#
#   tools/bench/make-corpus.sh [--silesia]
#
# Contenuto:
#   src/       sorgenti di questo repository (molti file piccoli, testo)
#   bin/       alcuni eseguibili di sistema (codice x86: utile per BCJ/BCJ2)
#   random.bin 16 MiB pseudo-casuali con seed fisso (incomprimibile)
#   zeros.bin  16 MiB di zeri (caso degenere, match lunghissimi)
#   repeat.txt testo ripetitivo (match a lunga distanza)
#   silesia/   (opzionale) Silesia corpus, il riferimento standard per i compressori
#
# NB: bin/ dipende dalla macchina: i confronti sono validi solo sulla stessa macchina.

source "$(dirname "$0")/common.sh"

WITH_SILESIA=0
[[ "${1:-}" == "--silesia" ]] && WITH_SILESIA=1

CORPUS="$BENCH_DATA/corpus"
info "Creo il corpus in $CORPUS"
rm -rf "$CORPUS"
mkdir -p "$CORPUS/src" "$CORPUS/bin"

# Sorgenti (via git per escludere file di build)
git -C "$REPO_ROOT" archive --format=tar HEAD C CPP DOC | tar -x -C "$CORPUS/src"

# Binari: i 40 eseguibili ELF piu' grandi in /usr/bin fino a ~60 MiB
budget=$((60 * 1024 * 1024)); used=0
while IFS= read -r f; do
  sz=$(stat -c %s "$f")
  (( used + sz > budget )) && continue
  if head -c 4 "$f" | grep -q 'ELF'; then
    cp "$f" "$CORPUS/bin/"; used=$((used + sz))
  fi
done < <(find /usr/bin -maxdepth 1 -type f -size +200k -printf '%s %p\n' | sort -rn | head -n 40 | cut -d' ' -f2-)

python3 -I - "$CORPUS" <<'PY'
import random, sys, os
d = sys.argv[1]
r = random.Random(20261029)
with open(os.path.join(d, "random.bin"), "wb") as f:
    f.write(r.randbytes(16 << 20))
with open(os.path.join(d, "zeros.bin"), "wb") as f:
    f.write(bytes(16 << 20))
words = [("".join(r.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(r.randint(2, 9)))) for _ in range(3000)]
para = [" ".join(r.choice(words) for _ in range(200)) for _ in range(50)]
with open(os.path.join(d, "repeat.txt"), "w") as f:
    for _ in range(4000):
        f.write(r.choice(para) + "\n")
PY

if (( WITH_SILESIA )); then
  info "Scarico il Silesia corpus"
  dl="$BENCH_DATA/download"; mkdir -p "$dl"
  [[ -f "$dl/silesia.zip" ]] || curl -fL -o "$dl/silesia.zip" https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
  mkdir -p "$CORPUS/silesia"
  python3 -I -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$dl/silesia.zip" "$CORPUS/silesia"
fi

du -sh "$CORPUS"/* >&2
info "Totale: $(du -sh "$CORPUS" | cut -f1)"
