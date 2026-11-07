# Lingue della GUI (7zFM, 7zG)

I file `*.txt` sono le traduzioni di 7-Zip 26.05 (https://7-zip.org, GNU LGPL come il resto di
7-Zip, copyright dei rispettivi traduttori indicati in testa a ogni file); `en.ttt` è il
riferimento inglese. 7-Zip li distribuisce con l'installatore e non con i sorgenti.

La GUI li cerca nella cartella `Lang` accanto a `7zFM.exe` e `7zG.exe` e usa da sola quello
della lingua di Windows; si cambia da Strumenti > Opzioni > Lingua.

I file non vanno modificati a mano: li genera `tools/import-lang.py` dall'installatore di
7-Zip, sostituendo «7-Zip» con «Heptazip» nelle voci che il fork ha rinominato e aggiungendo
le stringhe nuove del fork (menu Tema, profili del dialogo Aggiungi, Informazioni), che per ora
sono tradotte solo in italiano: nelle altre lingue restano in inglese. Per tradurle in un'altra
lingua si aggiunge una voce a `FORK` nello script e si rigenera la cartella. Il test
`lang_files` (`tests/lang_files.py`) controlla che ogni file sia leggibile dal programma.
