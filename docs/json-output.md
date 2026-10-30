# Output JSON del comando `l` (`-json`)

```sh
7zz l -json archivio.7z
7zz l -json -pPASSWORD cifrato.7z  "*.txt" -r
```

Stampa su stdout **un solo documento JSON** (UTF-8, indentato con 2 spazi). Banner, avanzamento
e messaggi vanno su stderr o sono disattivati. Implementazione:
`CPP/7zip/UI/Console/ListJson.cpp`; test: `tests/list_json.py`, che confronta ogni campo con `-slt`.

## Struttura

```json
{
  "archives": [
    {
      "path": "archivio.7z",
      "volumes": ["archivio.7z.002"],
      "levels": [
        {
          "path": "archivio.7z",
          "type": "7z",
          "physicalSize": 219,
          "headersSize": 198,
          "method": "LZMA2:24",
          "solid": true,
          "blocks": 1
        }
      ],
      "items": [
        {
          "path": "dir/file.txt",
          "isDir": false,
          "size": 500,
          "packedSize": 21,
          "modified": "2026-10-30T21:23:15.7783534Z",
          "attributes": "A -rw-r--r--",
          "crc": "8F836A5E",
          "encrypted": false,
          "method": "LZMA2:24",
          "block": 0
        }
      ]
    }
  ],
  "errors": [
    { "path": "rotto.7z", "message": "Cannot open the file as archive" }
  ]
}
```

- `archives`: un oggetto per ogni archivio aperto. `volumes` c'è solo se l'archivio ha più
  volumi.
- `levels`: la catena dei livelli aperti, per esempio un archivio dentro un'immagine; gli
  elementi sono quelli dell'**ultimo** livello. Per ogni livello: `path`, `type`, `offset`
  (se diverso da 0), `errors` / `warnings` (nomi dei flag, vedi sotto), `errorMessage`,
  `warningMessage`, `tailSize`, poi le proprietà dell'archivio.
- `items`: gli elementi, con lo stesso filtro di `l` (caratteri jolly, `-r`, `-x`…). Ogni
  elemento ha sempre `path` e `isDir`, poi le proprietà fornite dal formato.
- `errors`: gli archivi indicati che esistono ma **non si aprono** (non sono archivi,
  password errata, intestazioni rovinate).

## Nomi e valori

- **Nomi**: sono i nomi di `-slt` in lowerCamelCase: `Packed Size` → `packedSize`,
  `CRC` → `crc`, `Host OS` → `hostOs`. Una proprietà senza nome noto diventa `prop<ID>`.
- **Numeri**: un intero è un numero JSON quando `-slt` lo stampa come numero decimale
  (dimensioni, contatori). Valori oltre 2^53 restano esatti nel testo JSON, ma alcuni
  parser (JavaScript) li arrotondano.
- **Testo formattato**: se `-slt` stampa il valore in un formato proprio (CRC in
  esadecimale, attributi, metodo…), il JSON contiene la stessa stringa.
- **Booleani**: `true` / `false` (in `-slt`: `+` / `-`).
- **Date**: ISO 8601 in **UTC**, con il numero di decimali della precisione dell'archivio
  (0 per zip e tar, 7 per 7z, fino a 9). Le date non definite (valore 0) non compaiono.
- **Stringhe**: UTF-8 sempre valido; caratteri di controllo e surrogati isolati (nomi di
  file malformati) sono scritti come `\uXXXX`.
- Le proprietà vuote non compaiono. Le proprietà "raw" di `-slt` (descrittori di
  sicurezza NTFS, reparse point) non sono incluse.

Flag di `errors` / `warnings` (da `PropID.h`): `isNotArc`, `headersError`,
`encryptedHeadersError`, `unavailableStart`, `unconfirmedStart`, `unexpectedEnd`,
`dataAfterEnd`, `unsupportedMethod`, `unsupportedFeature`, `dataError`, `crcError`.

## Codici di uscita ed errori

- Il codice di uscita è lo stesso di `7zz l` senza `-json`.
- **Errori fatali prima dell'elenco** (riga di comando non valida, nome di archivio
  inesistente, tipo `-t` non valido): stdout è **vuoto**, il messaggio è su stderr e il
  codice di uscita è diverso da 0.
- `-json` è accettato solo con il comando `l`, e non con `-bse1` (errori su stdout):
  altrimenti si ha un errore di riga di comando (codice 7).
