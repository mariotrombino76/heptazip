# Modifiche al codice upstream

Elenco delle modifiche del fork ai file **esistenti** di 7-Zip, in particolare al core
in `C/`. Serve per due scopi:

- **merge di nuove release upstream:** ogni voce va controllata quando cambia il file;
- **contributi:** le correzioni generali (es. comportamento indefinito) si possono
  proporre a upstream.

I file nuovi del fork (`CMakeLists.txt`, `cmake/`, `tools/`, `tests/`, `docs/`, moduli
`*Mt.*` nuovi) non sono elencati qui.

Nel codice ogni modifica è marcata con un commento che inizia con `7zip-modern:`
(`grep -rn "7zip-modern:" C CPP`).

| File | Modifica | Motivo | Effetto sull'output / sulle prestazioni |
|---|---|---|---|
| `C/LzFindOpt.c` (`GetMatchesSpecN_2`, calcolo di `src`) | `(...) << 1` → `(...) * 2` | Shift a sinistra di un valore negativo: comportamento indefinito in C, segnalato da UBSan (`-fsanitize=shift-base`) | Nessuno: stesso codice macchina; archivi identici (`compare-archives.sh`) |

## Comportamenti upstream noti, non modificati

| Dove | Cosa | Perché non è modificato |
|---|---|---|
| `C/CpuArch.h` (`MY_CPU_LE_UNALIGN`, `GetUi32`/`SetUi32`/…) | Su x86 accessi a memoria non allineata tramite cast di puntatori: per lo standard è comportamento indefinito | Scelta voluta da upstream, usata in tutto il codice. Nei test UBSan è escluso il solo controllo `alignment`. Correzione possibile in futuro (fase 1): accessi tramite `memcpy`, conformi e con lo stesso codice macchina |
| `C/LzFindMt.c` (`stopWriting` in `MtSync_StopWriting` / `BtThreadFunc` / `HashThreadFunc`) | Flag `BoolInt` non atomico scritto da un thread e letto da un altro. La lettura che conta è ordinata dai semafori; TSan segnala quella dell'iterazione precedente (race formale C11, benigna su x86). Solo encoder multi-thread | Codice dell'encoder, fuori dall'ambito della fase 2. Soppresso nei test con `tests/tsan.supp`. Correzione possibile: flag atomico C11 (da proporre a upstream) |
