# 7zip
Voglio creare un branch/fork moderno e altamente performante del codice sorgente di 7-Zip, con l'obiettivo di superare i limiti storici del software in termini di velocità di compressione/decompressione, parallelizzazione e manutenibilità del codice, mantenendo o migliorando il tasso di compressione. Lavorerò principalmente su un ambiente Linux (Ubuntu) con compilatori moderni (GCC/Clang).

Per raggiungere questo obiettivo, voglio suddividere il lavoro in 4 fasi principali e ho bisogno del tuo supporto tecnico, di analisi del codice e di scrittura di patch/refactoring.

Ecco il piano d'azione su cui voglio lavorare con te:

1. Modernizzazione e Pulizia del Codice (Refactoring):
- Analisi e transizione graduale di parti del codice verso standard C++ moderni (C++20/C++23) dove non compromette le performance.
- Sostituzione di costrutti legacy con RAII e puntatori intelligenti per prevenire bug di memoria.
- Sostituzione del sistema di build originale con un moderno CMake configurato per GCC/Clang, abilitando flag di ottimizzazione spinti (es. -O3, -march=native, LTO).

2. Ottimizzazione delle Performance e Parallelizzazione:
- Implementazione o ottimizzazione di algoritmi di "Match-Finder" vettorializzati tramite istruzioni SIMD (AVX2 / AVX-512) per velocizzare la fase di creazione degli archivi.
- Risoluzione del collo di bottiglia della decompressione a singolo thread nel formato .7z, introducendo una pipeline di decompressione multi-thread nativa ed efficiente.

3. Integrazione di Nuovi Algoritmi (Ibridi):
- Valutazione e integrazione di motori di compressione alternativi e ultra-rapidi (come Zstandard - zstd) all'interno dell'architettura per offrire profili di compressione flessibili ("Ultra-Speed" vs "Ultra-Compression").

4. Gestione del Codice e Contributi:
- Scrittura di codice pulito, altamente commentato e compatibile con l'architettura esistente di 7-Zip, fornendo patch chiare e istruzioni su come integrarle nel codice sorgente clonato.
