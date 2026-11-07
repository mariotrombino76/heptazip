# 0019 — Scrittura di immagini ISO 9660 (Joliet, Rock Ridge)

| | |
|---|---|
| **Stato** | Implementato (issue #113) |
| **Data** | 2026-11-07 |
| **Fase** | 3: funzioni nuove |
| **Codice** | `CPP/7zip/Archive/Iso/IsoOut.{h,cpp}` (writer, nuovo), `IsoHandlerOut.cpp` (`IOutArchive`, nuovo), `IsoHandler.h`, `IsoRegister.cpp` (`REGISTER_ARC_IO`), `CPP/7zip/UI/GUI/CompressDialog.cpp` (formato nella tendina); test `tests/iso_writer.py` |

---

## 1. Contesto e problema

7-Zip legge ISO 9660 e UDF ma non scrive nessuno dei due, e la creazione di una ISO da una
cartella è tra le richieste storiche più frequenti. Su Windows non c'è uno strumento di
sistema (serve `oscdimg` dell'ADK o un programma terzo); su Linux ci sono `xorriso` e
`genisoimage`. Una ISO si monta con doppio clic su Windows e macOS, la leggono tutte le
macchine virtuali (VirtualBox, Hyper-V, Proxmox, QEMU) e qualunque sistema sa estrarla: è un
formato di scambio, non di compressione.

## 2. Obiettivi e non-obiettivi

- `7zz a -tiso disco.iso cartella/` con la sintassi di sempre, e il formato `iso` nella
  tendina del dialogo Aggiungi della GUI, senza altro codice nell'interfaccia.
- Immagini leggibili da Windows (albero **Joliet**), Linux e macOS (albero **Rock Ridge**:
  nomi lunghi, permessi, link simbolici, mtime) e dai lettori più vecchi (albero ISO 9660
  con identificatori rilassati).
- File oltre 4 GiB (estensioni multiple), directory profonde senza rilocazione.
- Riscrittura di un'immagine esistente (`a`, `d`, `u`): la ISO viene rigenerata per intero,
  i dati vecchi letti dal handler in lettura; nessun aggiornamento in loco.
- Non-obiettivi: El Torito (avviabili), UDF in scrittura, multisessione, HFS ibrido.

## 3. Formato scritto

Settori di 2048 byte (ECMA-119). Disposizione dell'immagine, tutta calcolata prima di
scrivere un byte:

| Settori | Contenuto |
|---|---|
| 0–15 | area di sistema (zeri) |
| 16 | descrittore primario (PVD) |
| 17 | descrittore supplementare Joliet (`%/E`, UCS-2 livello 3), se attivo |
| poi | terminatore |
| poi | path table L e M del 9660, poi L e M di Joliet |
| poi | estensioni delle directory del 9660 (ognuna seguita dalla propria area di continuazione Rock Ridge, se serve), poi quelle di Joliet |
| poi | dati dei file, nell'ordine degli elementi, ciascuno da un confine di settore |

**Identificatori 9660**: file `NOME.EST;1` con al massimo 30 caratteri d (A-Z, 0-9, `_`)
prima del `;1`, estensione fino a 3; directory fino a 31 caratteri senza punto. Le
collisioni (`a.txt` / `A.TXT`) prendono un suffisso `_1`, `_2`. Non si rispettano il
limite di 8 livelli né quello di 255 byte del percorso: Windows e Linux non li applicano e
la rilocazione delle directory (RE/CL/PL) è una complicazione che i lettori moderni non
richiedono.

**Joliet**: nomi in UCS-2 fino a 103 caratteri (il `-joliet-long` di mkisofs, accettato da
Windows), con `* / : ; ? \` sostituiti da `_`; i caratteri fuori dal BMP restano come
coppie surrogate, che Windows legge. Niente `;1` nei nomi Joliet.

**Rock Ridge** (SUSP 1.12, RRIP 1.12): nel record `.` del root `SP` e `ER` (`IEEE_P1282`);
in ogni record `NM` (nome completo in UTF-8, spezzato oltre 250 byte), `PX` nella forma a
44 byte (modo, link, uid 0, gid 0, numero seriale = indice del nodo), `TF` con la sola
modifica, `SL` per i link con un componente per elemento del percorso (ROOT, CURRENT,
PARENT). Le voci che non entrano nei 255 byte del record vanno in un'area di continuazione
(`CE`) propria della directory, subito dopo la sua estensione.

**File grandi**: oltre 4 GiB − 2048 byte un file ha più record consecutivi con il flag
multi-extent (0x80) su tutti tranne l'ultimo, in entrambi gli alberi.

**Date**: i record in UTC con offset 0; i descrittori con data di creazione = ora della
scrittura. **Etichetta**: `-mvol=NOME` (caratteri d e spazio, massimo 32; `ISOIMAGE` se
assente; la riscrittura di un'immagine conserva l'etichetta che aveva). `-mjoliet=off` e
`-mrr=off` tolgono un albero; `-mx`, `-mmt`, `-mmemuse` sono accettati e ignorati.

## 4. Design del codice

`IsoOut.cpp` costruisce l'albero (`CTree`: nodi con nome reale, identificatore 9660,
identificatore Joliet, figli ordinati separatamente per i due alberi; le directory
intermedie non date come elementi vengono create), calcola il layout (`CImage::Layout`:
le dimensioni delle directory non dipendono dalle posizioni, quindi una prima costruzione
in memoria con posizioni fittizie dà le dimensioni, poi si assegnano i settori e si
ricostruisce con i valori veri) e scrive: prima tutta la testa (descrittori, path table,
directory) in un buffer, poi i file copiati dagli stream in sequenza. L'uscita è un
`ISequentialOutStream`: non si cerca mai indietro, il che permette anche `-so`.

Se un file è cambiato tra la raccolta delle dimensioni e la copia, l'immagine conserva la
dimensione dichiarata (i record sono già scritti): il resto viene riempito di zeri e il
chiamante riceve `kInFileChanged`, come fa il tar.

`IsoHandlerOut.cpp` raccoglie gli elementi dal callback di aggiornamento (percorso, tipo,
dimensione, mtime, modo POSIX, destinazione del link) e, per quelli presi dall'immagine
aperta, dal `GetProperty`/`GetStream` del handler stesso; un'immagine con voci di boot non
si riscrive (`E_NOTIMPL`), perché El Torito non è scritto.

## 5. Verifica

`tests/iso_writer.py` (CTest `iso_writer`): albero con nomi lunghi, Unicode, collisioni,
directory vuote, file vuoti e di dimensione pari al settore, link relativi e assoluti,
permessi; tre varianti (Joliet+RR, solo RR, solo Joliet). Per ognuna: round trip con il
lettore di 7-Zip (Joliet, o Rock Ridge con `-mjoliet=off`, che restituisce permessi e
link), un parser ISO 9660 indipendente scritto nel test (descrittori, path table L e M
coerenti, record che non attraversano i settori, ordinamento, validità e unicità degli
identificatori, `SP`/`ER` del root, `PX`/`NM`/`TF`/`SL` seguendo le `CE`, estensioni
dentro il volume) e, se installato, pycdlib (la CI lo installa) che percorre i tre
alberi. Poi riscrittura (aggiunta e cancellazione) ed etichetta non valida.

A mano: file da 4,4 GB (due estensioni) letto per intero da 7-Zip su entrambi gli alberi
e da pycdlib su Joliet; 3000 file con 13 livelli; montaggio su Windows con
`Mount-DiskImage` e confronto dei file.

## 6. Limiti e lavoro futuro

- Il lettore di 7-Zip usa Joliet quando c'è: i link simbolici appaiono come file vuoti e
  i nomi oltre 103 caratteri troncati, anche se l'albero Rock Ridge li ha interi. Un
  lettore che preferisca Rock Ridge (o combini i due) è un miglioramento possibile.
- pycdlib legge un file multi-extent dall'albero Rock Ridge con la sola ultima estensione
  (limite suo: 7-Zip e l'albero Joliet lo vedono intero).
- El Torito (`-mboot=`) e UDF in scrittura: issue a parte, se serviranno.
- uid/gid sono sempre 0 (le immagini si montano in sola lettura, i permessi contano, i
  proprietari no).
