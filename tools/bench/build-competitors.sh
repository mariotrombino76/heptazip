#!/usr/bin/env bash
# Scarica, verifica e compila gli strumenti concorrenti per tools/bench/market-bench.py,
# senza privilegi di amministratore: sorgenti in $DEVTOOLS/src, eseguibili in $DEVTOOLS/bin.
#
#   tools/bench/build-competitors.sh            # DEVTOOLS=~/devtools
#   PATH=~/devtools/bin:$PATH tools/bench/market-bench.py --upstream <7zz 26.05>
#
# Versioni fissate (ultima release al 2026-10-31) con SHA-256: le impronte di zstd e lz4
# coincidono con quelle pubblicate dai progetti; le altre sono state calcolate al primo
# download. Opzioni di build predefinite (release), senza -march=native, come il 7zz del fork.
# lbzip2 2.5 non compila con la glibc attuale (gnulib di fseterr.c troppo vecchio): escluso.
set -euo pipefail

DEVTOOLS="${DEVTOOLS:-$HOME/devtools}"
SRC="$DEVTOOLS/src"
BIN="$DEVTOOLS/bin"
LOG="$DEVTOOLS/build-logs"
J="$(nproc)"
mkdir -p "$SRC" "$BIN" "$LOG"

fetch() {  # dir url sha256
  local dir="$SRC/$1" url="$2" sum="$3" file
  file="$dir/$(basename "$url")"
  mkdir -p "$dir"
  [ -f "$file" ] || curl -sSfL -o "$file" "$url"
  echo "$sum  $file" | sha256sum -c --quiet
  tar -C "$dir" -xzf "$file"
}

fetch zstd   https://github.com/meta/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz \
  eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
fetch lz4    https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz \
  537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
fetch brotli https://github.com/google/brotli/archive/refs/tags/v1.2.1.tar.gz \
  98d35d11a78dc453aebb7d10c3748f3f73ff78c8267c0dc697b8631f67bf7e8f
fetch xz     https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz \
  0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
fetch bzip3  https://github.com/iczelia/bzip3/releases/download/1.5.5/bzip3-1.5.5.tar.gz \
  dc3bd8237178e1a2b99d8dcf1f4aeb3593c723788a9863606e121b40784e596d
fetch lzip   https://download.savannah.gnu.org/releases/lzip/lzlib/lzlib-1.16.tar.gz \
  203228de911780309dad6813e51541d7ea89469784f01cb661edba080ff1b038
fetch lzip   https://download.savannah.gnu.org/releases/lzip/plzip/plzip-1.13.tar.gz \
  64d49dde20daa5fdff2b3ff28e3348082de10dd54eb10df6da7d1bc6c7a6db64

(cd "$SRC/zstd/zstd-1.5.7" && make -j"$J" -C programs zstd > "$LOG/zstd.log" 2>&1 && cp programs/zstd "$BIN/")
(cd "$SRC/lz4/lz4-1.10.0" && make -j"$J" -C programs lz4 > "$LOG/lz4.log" 2>&1 && cp programs/lz4 "$BIN/")
(cd "$SRC/brotli/brotli-1.2.1" \
  && cmake -S . -B out -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF > "$LOG/brotli.log" 2>&1 \
  && cmake --build out -j"$J" --target brotli >> "$LOG/brotli.log" 2>&1 && cp out/brotli "$BIN/")
(cd "$SRC/xz/xz-5.8.4" && ./configure --disable-shared --disable-doc --disable-nls > "$LOG/xz.log" 2>&1 \
  && make -j"$J" >> "$LOG/xz.log" 2>&1 && cp src/xz/xz "$BIN/")
(cd "$SRC/bzip3/bzip3-1.5.5" && ./configure --disable-shared > "$LOG/bzip3.log" 2>&1 \
  && make -j"$J" >> "$LOG/bzip3.log" 2>&1 && cp bzip3 "$BIN/")
(cd "$SRC/lzip/lzlib-1.16" && ./configure --prefix="$DEVTOOLS" > "$LOG/lzlib.log" 2>&1 \
  && make -j"$J" >> "$LOG/lzlib.log" 2>&1 && make install >> "$LOG/lzlib.log" 2>&1)
(cd "$SRC/lzip/plzip-1.13" \
  && ./configure CPPFLAGS="-I$DEVTOOLS/include" LDFLAGS="-L$DEVTOOLS/lib" > "$LOG/plzip.log" 2>&1 \
  && make -j"$J" >> "$LOG/plzip.log" 2>&1 && cp plzip "$BIN/")

for t in zstd lz4 brotli xz bzip3 plzip; do
  printf '%-7s %s\n' "$t" "$("$BIN/$t" --version 2>&1 | head -1)"
done
