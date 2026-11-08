<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/heptazip-logo-dark.svg">
    <img alt="Heptazip" src="docs/logo/heptazip-logo.svg" width="420">
  </picture>
</p>

🇮🇹 [Italiano](README.md) · 🇬🇧 English

# Heptazip

Heptazip is a fork of [7-Zip](https://7-zip.org) 26.05 (Igor Pavlov) for Linux and
modern compilers: faster at compression and decompression, parallel where 7-Zip is
not, and with a new method for artificial intelligence models. The program is still
`7zz`, with the same commands and options; `.7z` archives made with the standard methods
open with any 7-Zip.

The project was called Tensor7 until 2 November 2026 (renamed so as not to be confused
with a company of the same name).

## What it adds

- **FLOAT method** for the weights of AI models, lossless and chosen automatically for
  `.safetensors` and `.gguf`: BF16/FP16/FP32 with the exponent in Huffman, quantized
  GGUF (Q2_K … Q8_0) with a context model and rANS. Examples (`7zz a` with no options):
  Qwen3-0.6B BF16 at 66.3% (LZMA2 70.2%) in 1 s instead of 90; DeepHat-7B Q4_K_M at
  93.2% (LZMA2 97.8%), 18 times faster.
- **zstd** and **brotli** in the 7z format (`-m0=zstd`, `-m0=brotli`), compatible with
  7-Zip-zstd: brotli `-mx11` compresses better than zstd -19 and decompresses at over
  1 GB/s with several threads.
- **BWT method** for text and structured data (`-m0=bwt`, libbsc: Burrows-Wheeler with
  libsais and the QLFC coder, parallel blocks): silesia at 22.41% at 72 MB/s with 20
  threads, against 22.97% at 7.6 MB/s for LZMA2 `-mx9`; webster 16.0% vs 20.2%. Heptazip
  only, like FLOAT; for binaries LZMA2 stays better.
- **Parallel extraction and compression** of 7z folders (`-mpfd`, `-mpfc`), with
  archives byte-for-byte identical to the single-thread ones; **parallel zip extraction**
  over the files (`-mpfd`): 2 times faster on silesia, 3 to 4 times on AES zips, the same
  per-file results as the single-thread extraction.
- **Parallel gzip and zip**: block-based Deflate on all cores, over 8 times faster than
  7-Zip (`-tgzip -mx5`: 105 MB/s, a better ratio than pigz -9), files readable by any
  gzip/unzip and identical for any number of threads. The blocks are compressed by
  libdeflate: `-mx3` compresses more than pigz -6 at 2.4 times the speed (790 MB/s),
  `-mx1` exceeds 1 GB/s. Deflate decompression 2.6-2.9 times faster than 7-Zip (silesia
  770-800 MB/s, 3 to 15% behind libdeflate; faster than libdeflate on the corpus).
- **Faster LZMA2** (the default method): at `-mx5` silesia at 23.22% at 16.4 MB/s with
  decompression at 580 MB/s, against 23.40%, 9.4 and 221 MB/s for 7-Zip 26.05 built with
  its assembly routines; better than xz -6 in ratio and compression (23.35%, 13.3 MB/s) and
  at 91% of its decompression (635). The cost is CPU time: more threads for the same work
  (93 s against 58 for 7-Zip, 103 for xz -6).
  From level 5, word size 64 and blocks equal to the dictionary of the level, with a 16 MiB
  dictionary for the 32 MiB blocks of level 5 (1.4 GB of memory on silesia with 20 threads
  instead of 2.3); at `-mx1` and `-mx9` the archives are identical to 7-Zip's and decompress
  at the same speed (the assembly decoder is the same one; 1.4-1.5 times a 7-Zip built
  without assembly). With a single block (medium files, `-mx9`, plain LZMA) the match
  finder runs on several threads, with identical archives: silesia `-mx9` from 42 to 26 s,
  a 12 MB file at `-mx5` from 2.9 to 1.7 s.
- **More speed**: assembly on by default, CRC32 with PCLMULQDQ, optimized match finder,
  SIMD decoders (AVX2, NEON).
- **ISO images**, bootable too: 7-Zip only reads them, Heptazip creates them (`7zz a -tiso disc.iso
  folder/`, and from the GUI): ISO 9660 with Joliet (Windows) and Rock Ridge (Linux, macOS:
  long names, permissions, links), files over 4 GB, label with `-mvol=NAME`; BIOS and UEFI
  boot (El Torito) with `-mboot=`/`-mefi=` and a hybrid MBR for USB sticks; an existing ISO
  is updated by rewriting it, boot included.
- **Modern build** with CMake (GCC/Clang, C++20, optional LTO/PGO), tests with CTest,
  sanitizers, fuzzing (libFuzzer) and ARM64 builds.

Archives with FLOAT can only be extracted with Heptazip: for compatibility with 7-Zip
use `-mf=off`.

## Documentation

- [Website](https://mariotrombino76.github.io/heptazip/en/): presentation of the project, in English and
  Italian (sources in [site](site)).
- [Wiki](https://github.com/mariotrombino76/heptazip/wiki/Home-en): installation, quick
  start, which method to use, performance, compatibility, FAQ.
- [BUILD-modern.md](BUILD-modern.md) (Italian): building, testing, using the new options.
- [docs/design](docs/design) (Italian): one note for each decision (formats,
  measurements, alternatives).
- [docs/benchmark-mercato.md](docs/benchmark-mercato.md) (Italian): comparison with
  zstd, xz, brotli, pigz and others.

## Versions

`26.05-m3`: the 7-Zip base (26.05) and the Heptazip release on that base. Packages
(Linux x64, Windows x64) are in the [releases](https://github.com/mariotrombino76/heptazip/releases),
the changes in [docs/release-notes](docs/release-notes).

The Windows package requires Windows 10 version 1903 (May 2019) or later, or Windows 11: the
GUI (7zFM, 7zG) uses the dark-theme and per-monitor DPI APIs of those versions, and Windows 7
and 8.1 are no longer supported by Microsoft. The package contains the `Lang` folder with the
GUI translations (the 92 languages of 7-Zip; Heptazip's new strings only in Italian for now):
7zFM and 7zG pick the Windows language on their own, and Tools > Options > Language changes it.
See [Lang/README.md](Lang/README.md) (Italian).

## Branches

| branch | content |
|---|---|
| `develop` | development: base of the pull requests, build and tests (GitHub Actions) on every push |
| `modern` | stable line, updated from `develop` at every release |
| `main` | original 7-Zip 26.05, as a reference (does not receive the Heptazip work) |

Every change goes through an issue, a branch from `develop` and a pull request.

## License and credits

Heptazip: Copyright (c) 2026 Mario Trombino. The code written for Heptazip is
distributed under the 2-clause BSD license: whoever reuses it must keep the copyright
notice and the license text. Details, file by file:
[LICENSE-Heptazip.en](LICENSE-Heptazip.en) (Italian original:
[LICENSE-Heptazip](LICENSE-Heptazip)).

7-Zip is by Igor Pavlov: the original code keeps its licenses (GNU LGPL, with the unRAR
restriction for the RAR code, and the BSD parts; see `DOC/License.txt`). zstd is
distributed under the BSD license, the libdeflate compressor and brotli under the MIT
license, libbsc (BWT method) under the Apache-2.0 license. Heptazip is not an official
7-Zip product.
