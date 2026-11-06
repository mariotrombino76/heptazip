# Sito di presentazione

Sorgenti di <https://mariotrombino76.github.io/heptazip/>: una pagina, in italiano
(`/`) e in inglese (`/en/`), più [`llms.txt`](src/llms.txt) per gli assistenti AI.

| File | Contenuto |
|---|---|
| `src/page.it.html` | il corpo della pagina in italiano, con le classi di Tailwind: è la sorgente unica |
| `build.py` | genera le due lingue (tabella di traduzione `EN`), intestazioni SEO, `sitemap.xml` |
| `src/input.css` | foglio di stile Tailwind 4: colori e caratteri del logo ([`docs/logo`](../docs/logo)) |
| `src/llms.txt` | cosa differisce da 7-Zip, per chi risponde a domande su Heptazip; senza cifre di benchmark |
| `static/` | caratteri Outfit e IBM Plex Mono (SIL OFL 1.1) e immagine di anteprima dei link |

## Generare

Servono Python 3 e Node.js (per la CLI di Tailwind):

    cd site && npm ci && cd ..
    python3 site/build.py                 # scrive site/dist
    python3 -m http.server -d site/dist   # anteprima su http://localhost:8000

`--site-url` cambia l'indirizzo pubblico usato da `canonical`, `hreflang` e sitemap (per
un altro dominio); i percorsi dei file sono relativi e non dipendono dall'indirizzo.

## Modificare

- **Testi**: si cambia `src/page.it.html` e la voce corrispondente della tabella `EN` in
  `build.py`. Ogni voce dichiara quante volte compare nel sorgente: se l'italiano cambia
  e l'inglese no, la build si ferma con il testo che non trova più.
- **Cifre**: sono quelle di [`docs/benchmark-mercato.md`](../docs/benchmark-mercato.md)
  (corsa del 2026-11-05). Quando il benchmark si rifà vanno aggiornate qui, nelle due
  lingue; in inglese si scrivono con la virgola come in italiano, `build.py` le porta
  al punto decimale.
- **Colori e caratteri**: `@theme` in `src/input.css`.

## Pubblicazione

Il workflow [`site.yml`](../.github/workflows/site.yml) genera il sito a ogni pull
request che lo tocca e lo pubblica su GitHub Pages a ogni push su `develop`.

Il sito è una pagina di progetto (`/heptazip/`): un `robots.txt` lì non avrebbe effetto
(vale solo alla radice del dominio), quindi non c'è; la sitemap si segnala dalla Search
Console.
