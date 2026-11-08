# 0020 — Immagini ISO avviabili (El Torito, MBR ibrido)

| | |
|---|---|
| **Stato** | Implementato (issue #115) |
| **Data** | 2026-11-08 |
| **Fase** | 3: funzioni nuove |
| **Codice** | `CPP/7zip/Archive/Iso/IsoOut.{h,cpp}` (catalogo, boot record, area di sistema, boot info table), `IsoHandlerOut.cpp` (opzioni, conservazione nella riscrittura), `IsoIn.{h,cpp}` (piattaforma delle voci, settore del catalogo); test in `tests/iso_writer.py` (`boot_cases`) |

---

## 1. Contesto e problema

La nota 0019 ha dato a Heptazip la scrittura di ISO 9660 con Joliet e Rock Ridge, ma con
l'area di sistema a zeri e senza El Torito: una live Linux ricostruita dai suoi file non
si avvia né in una macchina virtuale, né masterizzata, né scritta su chiavetta con `dd`
(Rufus in modalità ISO funziona ugualmente perché estrae i file e installa i suoi
bootloader, ma è l'unico caso). Rimasterizzare una live — estrarre, modificare, ricreare —
è l'uso più frequente della creazione di ISO; senza avvio il formato vale solo per lo
scambio di dati.

## 2. Obiettivi e non-obiettivi

- Voce di avvio **BIOS** (`-mboot=`) e **UEFI** (`-mefi=`) nello stesso catalogo, come
  fanno xorriso e le live di Debian, Ubuntu, Alpine, Fedora.
- **Boot info table** per isolinux e per `eltorito.img` di GRUB, che la richiedono per
  trovare il proprio file sul disco.
- **MBR ibrido** (isohybrid) per avviare la stessa immagine da chiavetta, con il codice
  di avvio di syslinux (`isohdpfx.bin`) che le live portano con sé.
- Riscrittura di un'immagine avviabile (`a`, `d`, `u`) senza perdere l'avvio.
- Verifica indipendente (parser nel test, pycdlib) e con firmware veri (QEMU: SeaBIOS e
  OVMF, da CD e da disco).
- Non-obiettivi: GPT nell'area di sistema (BIOS, UEFI e Linux si accontentano dell'MBR;
  i Mac no), partizioni aggiunte in coda, emulazione floppy/disco per immagini nuove
  (le voci esistenti con emulazione si conservano), UDF, HFS.

## 3. Formato scritto

**Boot record volume descriptor** al settore 17 (El Torito lo vuole lì): tipo 0,
`CD001`, identificatore `EL TORITO SPECIFICATION` riempito di zeri a 32 byte, 32 byte di
zeri, settore del catalogo (32 bit little endian a byte 71). Il descrittore Joliet e il
terminatore scalano di un settore.

**Catalogo**: un settore, file dell'albero (`-mcatalog=`, predefinito `boot.catalog`; un
file già presente con quel nome viene sostituito, perché un albero estratto contiene il
vecchio catalogo). Voce di validazione (piattaforma della voce predefinita, 24 byte di
identificatore a zero, checksum che azzera la somma delle 16 parole, `55 AA`); voce
predefinita; poi, per ogni altra immagine, un'intestazione di sezione (`90` o `91`
sull'ultima, piattaforma, una voce) e la sua voce. Ogni voce: `88` avviabile, tipo di
supporto, segmento di caricamento, tipo di sistema, numero di settori da 512 byte, settore
dell'immagine. Al massimo 30 voci oltre la predefinita.

- `-mboot`: piattaforma 0 (80x86), nessuna emulazione, 4 settori (`-mbootload=N`; 0 =
  tutta l'immagine), voce predefinita.
- `-mefi`: piattaforma 0xEF, nessuna emulazione, settori = tutta l'immagine; oltre i
  32 MiB il campo a 16 bit non basta e si scrive 0, che per la specifica UEFI vuol dire
  «dall'immagine alla fine del disco» (è l'eccezione che anche il lettore di 7-Zip
  conosce).
- Solo `-mefi`: la voce UEFI è quella predefinita e la validazione dice 0xEF.

**Boot info table** (`-mbootinfo`, predefinito `auto`): nell'immagine BIOS, a byte 8,
quattro interi a 32 bit: settore del PVD (16), settore dell'immagine, lunghezza in byte,
checksum (somma delle parole a 32 bit dal byte 64 alla fine, coda riempita di zeri). I
restanti 40 byte non si toccano. `auto` scrive la tabella se i byte 8..63 sono tutti zero
o se i primi quattro valgono già 16 (una tabella precedente: `isolinux.bin` di Debian ne
ha una precompilata, con `DEADBEEF` nei byte riservati); così un'immagine che a byte 8
ha codice (etfsboot.com di Windows) resta intatta. L'immagine viene letta in memoria
prima di scrivere le intestazioni (limite 64 MiB, oltre non si patcha).

**Area di sistema** (`-mhybrid`, acceso quando c'è una voce di avvio): MBR nei primi 512
byte, resto a zeri.

| Byte | Contenuto |
|---|---|
| 0–431 | codice di avvio di `-mmbr=FILE` (i primi 432 byte; `isohdpfx.bin` di syslinux è esattamente così), altrimenti zeri |
| 432–439 | settore dell'immagine BIOS in unità da 512 byte (settore × 4), 64 bit: è dove `isohdpfx` legge l'indirizzo del file da caricare, la convenzione di isohybrid e di xorriso |
| 440–443 | identificatore del disco (dall'ora di creazione, mai zero) |
| 446–461 | partizione 1: avviabile (`80`), tipo `00`, da LBA 0, lunga quanto l'immagine; CHS con geometria 64 testine × 32 settori |
| 462–477 | partizione 2 (se c'è `-mefi`): tipo `EF`, dall'immagine UEFI, lunga quanto il file |
| 510–511 | `55 AA` |

La partizione 1 che parte da LBA 0 (quindi contiene l'MBR stesso) è l'anomalia che
rende avviabile da BIOS un'immagine pensata per il CD; il tipo `00` è quello che usano
oggi Debian, Ubuntu e Alpine: il driver di partizioni di EDK2 salta le voci di tipo 0
e non scarta l'intera tabella per quella sovrapposizione, così da chiavetta l'UEFI
trova la partizione `EF` e il suo file system FAT. Senza `-mmbr` la tabella c'è ma il
codice no: da chiavetta parte l'UEFI e non il BIOS.

## 4. Design del codice

`IsoOut.h` aggiunge `COutBootEntry` (percorso dell'immagine nell'albero, piattaforma, tipo
di supporto, segmento, tipo di sistema, settori, modalità della boot info table) e, in
`COutOptions`, le voci, il percorso del catalogo, `Hybrid`, `MbrCode`. `COutItem::Hidden`
marca un'immagine di avvio che non è un file dell'albero: `CTree::Build` le dà un nodo
senza genitore (solo dati, nessun record). Il catalogo è un nodo con contenuto generato
(`CImage::Generated`, mappa nodo → buffer), come le immagini BIOS patchate: il ciclo di
scrittura le prende dalla memoria invece che dallo stream.

`CTree::Insert` (estratto da `Build`) crea il percorso e dice se esisteva; `Find`
risolve i percorsi di `-mboot`/`-mefi`/`-mcatalog` (anche quelli nascosti). `Layout`
riserva il settore 17 se ci sono voci; `BuildCatalog`, `BuildBootRecord`,
`BuildSystemArea` e `PatchBootInfoTable` usano i settori assegnati. Le immagini BIOS
vengono lette dallo stream del chiamante subito dopo il layout (è l'unica lettura fuori
ordine: il callback di 7-Zip apre i file per indice e non se ne cura).

`IsoHandlerOut.cpp`: le opzioni (`-mmbr` legge un file locale con `CInFile`, l'unica
opzione che tocca il file system: il codice MBR non sta nell'immagine). Nella
riscrittura le pseudo-voci `[BOOT]/…` del lettore (indici oltre `Refs`) non sono file:
per ognuna si cerca il file dell'albero che inizia al suo settore (`FindRefByBlock`) e
la voce si aggancia a quel percorso; se non c'è (immagine nascosta, o file cancellato
dall'utente) i suoi dati — per quanto il lettore ne conosce: i settori dichiarati nel
catalogo — viaggiano come elemento nascosto. Il vecchio catalogo (file al settore
`BootCatalogBlock`, esposto dal lettore) si scarta e il suo percorso diventa quello del
nuovo; i primi 512 byte dell'immagine aperta, se finiscono in `55 AA`, forniscono il
codice MBR quando `-mmbr` manca. `-mboot`/`-mefi` nella riscrittura sostituiscono le
voci della stessa piattaforma. Cancellare una pseudo-voce (`7zz d x.iso
"[BOOT]/2-Boot-NoEmul.img"`) toglie quella voce di avvio.

Il lettore (`IsoIn`) ora conserva la piattaforma di ogni voce (dalla validazione o
dall'intestazione di sezione) e il settore del catalogo: due campi, nessun cambiamento
di comportamento.

Errori: immagine di avvio assente o directory → `ERROR_FILE_NOT_FOUND` («No such file or
directory»); catalogo su una directory → `ERROR_FILE_EXISTS`; valori non validi →
`E_INVALIDARG`. Sono gli unici canali che `IOutArchive` offre.

L'etichetta (`-mvol`) accetta da questa nota ASCII stampabile senza forzare le
maiuscole: `alpine-virt 3.24.4 x86_64` è un'etichetta vera, che la regola stretta dei
caratteri d della 0019 rifiutava.

## 5. Verifica

`tests/iso_writer.py` (`boot_cases`): immagine BIOS sintetica (byte 8..63 a zero,
lunghezza non multipla di 4 per la coda della checksum), immagine UEFI di 1440 KiB,
codice MBR sintetico. Casi: BIOS+UEFI con MBR e codice; solo BIOS senza hybrid, tabella
spenta, 8 settori, catalogo predefinito; solo UEFI (validazione 0xEF, partizione 2 senza
codice); catalogo al posto di un file esistente. Per ognuno `parse_catalog` e
`check_boot` controllano boot record, validazione (checksum), voci, tabella (valori e
checksum ricalcolata, resto dell'immagine intatto), MBR (codice, 0x1B0, identificatore,
partizioni, CHS di partenza) e, con pycdlib, le stesse voci e il settore del file del
catalogo. Riscrittura: aggiunta di un file (voci rimappate, tabella ricalcolata, codice
conservato), cancellazione della pseudo-voce UEFI (partizione 2 sparisce), cancellazione
del file BIOS (la voce resta con i dati nascosti, 4 settori), nuova riscrittura.
Errori: immagine assente, directory come immagine, catalogo su directory, `-mmbr`
inesistente, senza lasciare l'archivio.

A mano con Alpine Linux 3.24.4 (virt): la ISO originale ha esattamente il layout qui
descritto (tipo `00` + `EF`, `isohdpfx` con 0x1B0); estratta con `7zz x` e ricostruita
con `-mboot=boot/syslinux/isolinux.bin -mefi=boot/grub/efi.img
-mcatalog=boot/syslinux/boot.cat -mmbr=boot/syslinux/isohdpfx.bin`, e in alternativa
riscritta aggiungendo un file, dà immagini con catalogo, tabella e MBR coerenti; avvio
in QEMU: vedi la sezione 7.

## 6. Limiti e lavoro futuro

- Niente GPT: i Mac con firmware Apple non vedono la partizione EFI; i firmware che
  pretendono una GPT per l'avvio da USB sono rari ma esistono.
- Le voci nascoste nella riscrittura hanno la lunghezza che il catalogo dichiara: per
  isolinux (che si aspetta il proprio file intero) la voce resta ma non è più utile se il
  suo file viene cancellato dall'albero; chi cancella `isolinux.bin` lo sa.
- Nessuna emulazione floppy/disco per immagini nuove; nessun `-mbootload` per l'UEFI
  (è sempre tutta l'immagine).
- Il codice MBR è un file esterno: Heptazip non lo porta con sé (`isohdpfx.bin` è
  GPL, e sta comunque in ogni live).
- La GUI non espone le opzioni di avvio (vanno nel campo Parametri del dialogo).

## 7. Collaudo con QEMU

QEMU 8.2 (TCG, senza KVM), 768 MiB, Alpine Linux 3.24.4 virt; esito letto dalla seriale
(isolinux e la console di Alpine) e dallo screendump del monitor.

| Firmware | Supporto | ISO ricostruita (`-mboot -mefi -mcatalog -mmbr`) | ISO originale riscritta (`7zz a alpine.iso extra.txt`) |
|---|---|---|---|
| SeaBIOS 1.16.3 | CD (`-cdrom`) | ISOLINUX 6.04 → kernel → login | login |
| SeaBIOS 1.16.3 | disco USB (`usb-storage`, isohybrid) | ISOLINUX via `isohdpfx` → login | — |
| OVMF 2024.02 | CD | GRUB (`efi.img`) → login | — |
| OVMF 2024.02 | disco USB (partizione `EF`) | GRUB → login | login |

Controllo negativo: la stessa ISO scritta senza `-mboot` dà «No bootable device» in
SeaBIOS. Il test automatico non usa QEMU (non è tra i requisiti della CI): le strutture
sono verificate dal parser e da pycdlib, l'avvio vero a mano come sopra.
