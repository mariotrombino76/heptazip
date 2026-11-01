#!/usr/bin/env bash
# Semi per il fuzzing dei gestori di archivio (design 0007): archivi piccoli e validi.
#
#   tools/fuzz/make-seeds.sh <7zz> <directory_corpus>
#
# Crea <directory_corpus>/<formato>/... per i formati che 7zz (o zstd/plzip in
# ~/devtools/bin, se presenti) sa creare. I formati di sola lettura partono da zero.
set -euo pipefail

SZ="$(realpath "${1:?uso: $0 <7zz> <dir>}")"
OUT="$(realpath -m "${2:?uso: $0 <7zz> <dir>}")"
DT="${DEVTOOLS:-$HOME/devtools}/bin"
WORK="$(mktemp -d /dev/shm/7z-seeds.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

src="$WORK/src"
mkdir -p "$src/dir/sub" "$src/empty"
printf 'hello hello hello world\n' > "$src/a.txt"
head -c 3000 /dev/urandom > "$src/dir/random.bin"
seq 1 400 > "$src/dir/sub/numbers.txt"
: > "$src/zero"
cp /usr/bin/true "$src/prog"          # ELF: gruppo con filtro BCJ
head -c 20000 /usr/bin/true > "$src/prog.part"

mk() {  # formato nome opzioni...
  local fmt="$1" name="$2"; shift 2
  mkdir -p "$OUT/$fmt"
  (cd "$src" && "$SZ" a "$@" -bso0 -bsp0 -bse0 "$OUT/$fmt/$name" . >/dev/null) || true
}

mk 7z lzma2.7z -t7z -mx5
mk 7z lzma2-nonsolid.7z -t7z -mx1 -ms=off
mk 7z lzma.7z -t7z -m0=lzma
mk 7z ppmd.7z -t7z -m0=ppmd
mk 7z bzip2.7z -t7z -m0=bzip2
mk 7z deflate.7z -t7z -m0=deflate
mk 7z deflate64.7z -t7z -m0=deflate64
mk 7z copy.7z -t7z -mx0
mk 7z bcj2.7z -t7z -mf=BCJ2
mk 7z delta.7z -t7z -mf=Delta:4
mk 7z arm64.7z -t7z -mf=ARM64
mk 7z zstd.7z -t7z -m0=zstd
mk 7z aes.7z -t7z -pfuzz
mk 7z aes-headers.7z -t7z -pfuzz -mhe=on
mk 7z noheadercompr.7z -t7z -mhc=off

mk zip deflate.zip -tzip -mm=Deflate
mk zip deflate64.zip -tzip -mm=Deflate64
mk zip bzip2.zip -tzip -mm=BZip2
mk zip lzma.zip -tzip -mm=LZMA
mk zip ppmd.zip -tzip -mm=PPMd
mk zip store.zip -tzip -mm=Copy
mk zip aes.zip -tzip -mem=AES256 -pfuzz
mk zip zipcrypto.zip -tzip -mem=ZipCrypto -pfuzz

mk tar a.tar -ttar
mk wim a.wim -twim

tarball="$WORK/a.tar"
(cd "$src" && tar -cf "$tarball" .)
one() {  # formato nome opzioni...
  local fmt="$1" name="$2"; shift 2
  mkdir -p "$OUT/$fmt"
  "$SZ" a "$@" -bso0 -bsp0 -bse0 "$OUT/$fmt/$name" "$tarball" >/dev/null || true
}
one gzip a.tar.gz -tgzip
one bzip2 a.tar.bz2 -tbzip2
one xz a.tar.xz -txz
one xz arm64.tar.xz -txz -mf=ARM64
if [ -x "$DT/zstd" ]; then mkdir -p "$OUT/zstd"; "$DT/zstd" -q -19 -f "$tarball" -o "$OUT/zstd/a.tar.zst"; fi
if [ -x "$DT/plzip" ]; then mkdir -p "$OUT/lzip"; "$DT/plzip" -c "$tarball" > "$OUT/lzip/a.tar.lz"; fi

find "$OUT" -type f | sort | while read -r f; do printf '%7d %s\n' "$(stat -c %s "$f")" "${f#"$OUT"/}"; done
