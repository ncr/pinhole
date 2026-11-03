# Lokalne wyszukiwanie zdjęć, opisy i osoby

Research: **3 listopada 2026**. Stan dokumentacji sprawdzony tego dnia.
To propozycja kolejnego etapu Pinhole, bez instalowania modeli ani wykonywania
analizy prywatnych zdjęć.

Kontynuacja: [narzędzie do przygotowania próbek i oceny wyników](photo-search-benchmark.md).
Na drugim komputerze (M2 Pro, Arch/Asahi) przygotowano 384 lokalne miniatury
za zgodą właściciela. Sprawdzono też konkretny stos ANE: brak firmware'u
blokuje urządzenie, a pakiet kompilatora odrzuca bezpośrednie odwzorowanie
warstw wejściowych CLIP i SigLIP 2. Porównanie modeli nie zostało jeszcze
wykonane; dalszy dokument zawiera odtwarzalny test i ograniczenia tej próby.

**Rekomendacja:** zacząć od wyszukiwania obraz–tekst i proponowanych tagów,
z lokalnym workerem CPU/CUDA. Porównać SigLIP 2 z wielojęzycznym CLIP na
polskich zapytaniach. Następnie dodać osoby jako osobny indeks twarzy.
NPU jest przydatnym opcjonalnym backendem; nie powinno warunkować funkcji.

## Sprzęt: tak, ale system i konkretny procesor mają znaczenie

Odczyt tej maszyny: Ryzen 7 **9800X3D**, 8 rdzeni / 16 wątków;
RTX **5080**, 16 303 MiB pamięci widocznej przez `nvidia-smi`, sterownik
615.78.08; Linux 7.2.8-9-omarchy. `lspci` pokazuje też Radeon 610M;
`/sys/class/accel` nie zawiera urządzenia. To desktopowy Ryzen bez NPU XDNA,
a nie Ryzen AI. Nie ma tu NPU do włączenia dla tej funkcji.
[Specyfikacja AMD](https://www.amd.com/en/products/processors/desktops/ryzen/9000-series/amd-ryzen-7-9800X3d.html).

| Platforma | Faktyczne możliwości | Decyzja dla Pinhole |
|---|---|---|
| Obecny komputer, Arch/Omarchy | CPU i działający NVIDIA GPU; brak NPU | CPU jako podstawa, CUDA jako pierwszy akcelerator do testu |
| Apple Silicon, macOS | Core ML udostępnia CPU, GPU i ANE; ONNX Runtime ma CoreML EP | Opcjonalny backend macOS po przygotowaniu i pomiarze modelu |
| Apple Silicon, Asahi/Linux | Sterownik ANE poza głównym drzewem kernela, odrębny userspace | Eksperyment później; nie obiecywać ścieżki Core ML na Linuksie |
| Nowszy Ryzen AI, Linux | Oficjalny Ryzen AI SDK obsługuje NPU na wybranych APU; instrukcja Linux zaczyna się od Strix Point | Dodatkowy backend sprawdzony na konkretnym laptopie i wersji SDK |
| Ryzen AI, Windows | Szersza macierz generacji, zależna od typu modelu | Osobna walidacja; wsparcia Windows nie przenosić automatycznie na Linux |

Apple pozwala wybrać m.in. CPU+ANE lub wszystkie jednostki, ale to nie jest
obietnica uruchomienia całego modelu na ANE. CoreML EP ma ograniczenia
operatorów i kształtów; `MLProgram` wymaga macOS 12+. Rozmieszczenie operacji
należy potwierdzić profilerem. MLX sam opisuje backendy CPU/GPU — użycie MLX
nie dowodzi użycia Neural Engine.
[CoreML EP](https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html),
[profilowanie Apple](https://developer.apple.com/documentation/coreml/analyzing-a-core-ml-model-s-performance-in-xcode),
[MLX](https://github.com/ml-explore/mlx).

Asahi oznacza ANE jako **out of tree**. Projekt sterownika ma własne `libane`
i bindingi Python; nie jest to gotowy odpowiednik runtime Core ML.
Wniosek projektowy: traktować tę ścieżkę jako badawczą, a działający model
CPU/GPU zachować jako niezależną podstawę.
[Macierz Asahi M1](https://asahilinux.org/docs/platform/feature-support/m1/),
[sterownik ANE](https://github.com/eiln/ane).

Aktualne instrukcje AMD **Ryzen AI 1.9** opisują Linux, Ubuntu 24.04,
kernel >=6.10 i nowsze konsumenckie APU od Strix Point. Wymieniają CNN INT8/BF16,
NLP BF16 oraz przepływ LLM NPU-only. Sama obecność `amdxdna` w kernelu nie
zapewnia całego runtime ani zgodności modelu. Dokumentacja nie daje podstaw,
by obiecać gotową, wspieraną instalację na Arch/Omarchy. Starszy Phoenix/Hawk
Point występuje w ogólnej macierzy AMD, ale nie wolno z tego wywnioskować
identycznej obsługi Linux. Strona release notes ma nawet pozostałość nazwy
„1.8” przy nagłówku konfiguracji, więc przy wdrożeniu trzeba przypiąć wydanie.
[Linux AMD](https://ryzenai.docs.amd.com/en/latest/linux.html),
[macierz modeli/generacji](https://ryzenai.docs.amd.com/en/latest/relnotes.html),
[sterownik XDNA](https://github.com/amd/xdna-driver).

TOPS nie pozwala porównać czasu indeksowania. Liczy się także precyzja,
obsługiwane operatory, przenoszenie tensorów i koszt przygotowania modelu.
AMD dzieli graf między NPU i CPU; udostępnia raport przydziału operatorów.
Dla obecnej RTX najpierw sprawdzić zwykły CUDA EP, z przypiętymi zgodnymi
wersjami CUDA/cuDNN. TensorRT rozważyć dopiero po działającym baseline.
To hipoteza, że GPU będzie tu korzystne — czasu i poboru energii jeszcze
nie zmierzono.
[Podział grafu AMD](https://ryzenai.docs.amd.com/en/latest/modelrun.html),
[CUDA EP i zgodność bibliotek](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html).

## Co indeksować

Trzy osobne warstwy dają różne możliwości:

1. **Wektor obrazu i tekstu:** zapisujemy wektor zdjęcia raz; zapytanie
   „rodzina przy stole” kodujemy tym samym zestawem modeli i szukamy
   podobnych wektorów. Nie trzeba najpierw wymyślać kompletnego opisu każdego
   zdjęcia. Lista znanych pojęć może z tego samego obrazu proponować tagi.
2. **Opis i tekst widoczny na zdjęciu:** generator może dodać krótkie zdanie;
   OCR osobno przyda się do zrzutów ekranu, dokumentów i napisów. To uzupełnienie
   wyszukiwania, bo opis może pominąć szczegół lub dopowiedzieć coś błędnego.
3. **Osoby:** detekcja twarzy → wyrównanie → wektor każdej twarzy → grupowanie.
   Użytkownik nazywa grupę, a program proponuje kolejne dopasowania.
   Nazwy nie wynikają z samego ogólnego opisu obrazu.

Przykład „Martyna na rowerze” wymaga połączenia nazwanego przez użytkownika
indeksu osoby z wyszukiwaniem sceny. Zdjęcie od Martyny nie musi przedstawiać
Martyny; nadawca i osoba na fotografii to różne dane.

### Modele do krótkiego porównania

| Kandydat | Zastosowanie / licencja deklarowana przez wydawcę | Rozmiar do zaplanowania |
|---|---|---|
| Google `siglip2-base-patch16-224` | Obraz–tekst, klasyfikacja po etykietach; Apache 2.0; główny kandydat jakościowy | Oficjalne wagi FP32: **1,5 GB**; tokenizer i runtime dodatkowo |
| OpenAI CLIP ViT-B/32 + `sentence-transformers/clip-ViT-B-32-multilingual-v1` | Baseline wyszukiwania po polsku; kod/wagi CLIP MIT, adapter Apache 2.0 | Pełny checkpoint CLIP **605 MB**, osobny tekstowy **539 MB**; eksport samej wieży obrazu może zmniejszyć pakiet |
| `SmolVLM-256M-Instruct` | Opcjonalne krótkie opisy; Apache 2.0; karta oznacza English | Wagi **513 MB**, bufory generacji/obrazu dodatkowo |
| OpenCV YuNet + SFace | Detekcja i wektory twarzy; katalogi modeli MIT + Apache 2.0 | YuNet 2023mar **0,233 MB**, SFace FP32 **38,7 MB** lub wariant INT8 **9,90 MB** |

SigLIP 2 jest modelem wielojęzycznym; jego konkretna skuteczność dla polskich
zapytań i rodzinnych zdjęć wymaga własnej oceny. Wielojęzyczny adapter CLIP
wprost wymienia `pl` i współdziała z niezmienioną wieżą obrazu CLIP.
Nie wolno mieszać wektorów CLIP i SigLIP ani modeli o innych wagach w jednym
porównaniu bez migracji indeksu.
[SigLIP 2: karta](https://huggingface.co/google/siglip2-base-patch16-224),
[publikacja autorów](https://arxiv.org/abs/2502.14786),
[plik wag](https://huggingface.co/google/siglip2-base-patch16-224/blob/main/model.safetensors),
[CLIP i licencja](https://github.com/openai/CLIP),
[adapter wielojęzyczny](https://huggingface.co/sentence-transformers/clip-ViT-B-32-multilingual-v1),
[CLIP: rozmiar](https://huggingface.co/openai/clip-vit-base-patch32/blob/main/pytorch_model.bin),
[adapter: rozmiar](https://huggingface.co/sentence-transformers/clip-ViT-B-32-multilingual-v1/blob/main/model.safetensors).

SmolVLM to kandydat do oceny, nie gwarancja dobrych polskich opisów. Pamięć
na dysku nie jest szczytową pamięcią inferencji; rozdzielczość, liczba
fragmentów obrazu i długość generacji też kosztują. Zaczynać od krótkiego
opisu i ograniczonego wejścia, zgodnego z procesorem modelu.
[Karta modelu](https://huggingface.co/HuggingFaceTB/SmolVLM-256M-Instruct),
[rzeczywisty plik wag](https://huggingface.co/HuggingFaceTB/SmolVLM-256M-Instruct/blob/main/model.safetensors).

YuNet/SFace mają oficjalne przykłady C++ i ONNX; istnieją warianty
kwantyzowane. Podane rozmiary pochodzą z plików Git LFS, a licencje są
przypisane do całych katalogów modeli przez OpenCV.
[YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet),
[SFace](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface),
[wagi YuNet](https://raw.githubusercontent.com/opencv/opencv_zoo/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx),
[wagi SFace FP32](https://raw.githubusercontent.com/opencv/opencv_zoo/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx),
[wagi SFace INT8](https://raw.githubusercontent.com/opencv/opencv_zoo/main/models/face_recognition_sface/face_recognition_sface_2021dec_int8.onnx).

Nie przyjmować domyślnie popularnych wag InsightFace/buffalo jako swobodnie
redystrybuowalnych: kod jest MIT, ale publiczne wagi mają osobne ograniczenie
non-commercial research. AuraFace deklaruje Apache 2.0, lecz jest większym
kandydatem i nie zastępuje sprawdzenia licencji każdego dołączanego detektora.
[Warunki InsightFace](https://github.com/deepinsight/insightface#license),
[karta AuraFace](https://huggingface.co/fal/AuraFace-v1).

## Jak wpasować to w Pinhole

Poniższe są decyzjami proponowanymi, a nie opisem zaimplementowanego API.
Obecny kontrakt pozwala na helper procesu, własny `cacheDir`, stabilne id,
`fetch` i dane użytkownika w `state`; zakazuje zapisu pluginu do głównego
indeksu. Wzorem rozdziału wyników analizy od ręcznych poprawek jest Series.
[Kontrakt agentów](../plugin-agents.md), [API](../plugins.md),
[Series](../../plugins/series/Main.qml).

- **Osobny worker analizy**, uruchamiany na żądanie / po gotowości UI,
  bez importu runtime ML, ładowania wag i kompilacji NPU na ścieżce startu
  okna. Prototyp może być helperem pluginu; wspólny długotrwały scheduler
  powinien należeć do usługi, żeby kilka okien nie indeksowało tego samego.
- **Osobna baza pochodna** dla wektorów/opisów/FTS i stanu kolejki. Klucz:
  `(library.key, asset.id, fingerprint wejścia, model+revision+preprocessing)`.
  Ścieżka pliku i pozycja w gridzie nie są tożsamością zdjęcia. Nie
  unieważniać całego indeksu po zmianie ulubionego lub kolejności grida.
- **Ręczne tagi, nazwy osób i poprawki** to trwała praca użytkownika w state,
  z eksportem/backupem i Undo. Przeliczenie modeli nie usuwa tych danych;
  etykiety wygenerowane i zatwierdzone są rozróżnione.
- **Wznawianie i małe transakcje:** tylko nowe/zmienione zdjęcia, checkpointy
  partii, atomowe wyniki, anulowanie, ograniczone retry i obsługa usuniętego
  źródła. Przy zmianie modelu osobny indeks/wersja; nie mieszać wymiarów.
- **Mały koszt w tle:** początkowo jeden job i małe batche; limit wątków,
  RAM/VRAM i tempa, pauza podczas scrollowania/dekodowania/eksportu,
  na baterii i pod obciążeniem. GPU analizy współdzieli zasoby z gridem:
  samo `nice` nie gwarantuje płynności GPU. ORT udostępnia kontrolę wątków.
  [Threading ORT](https://onnxruntime.ai/docs/performance/tune-performance/threading.html).
- **Miniatury najpierw:** poprawna orientacja/kolor i dokładny preprocessing
  checkpointu. Detekcja małych twarzy w zdjęciach grupowych może wymagać
  medium; pobrania z iCloud mają przechodzić przez usługę i osobny budżet,
  bez automatycznego ściągania wszystkich oryginałów.
- **Jeden grid wyników:** operacje nadal po stabilnych id, z sortowaniem
  po trafności lub dacie. Docelowo dodać udokumentowaną operację wyszukiwania
  hosta; nie dublować całego modelu nawigacji i renderera w QML.
- **Lokalne dane:** jeden świadomy download modelu z podanym rozmiarem,
  przypiętą rewizją, SHA256 i licencją; dalsza analiza offline, bez wysyłania
  zdjęć do API. Ustawienia pokazują postęp, backend i przyciski Pauza/
  Wznów/Usuń indeks. Osoby jako osobna opcja; scalenie/rozdzielenie grup
  i „to nie ta osoba”, z pozostawieniem niepewnych przypadków bez nazwy.

Dla 50 tys. zdjęć i 768 elementów wektora same wartości FP16 zajmują
**76,8 MB**, FP32 **153,6 MB** (własne obliczenie, bez narzutu bazy).
Na początek sprawdzić dokładne wyszukiwanie iloczynem skalarnym po
znormalizowanej macierzy. Nie zakładać, że osobny serwer vector DB/ANN jest
już konieczny. ANN dodać, gdy pomiary uzasadnią narzut i złożoność.

## Następny krok: mały benchmark, potem wybór backendu

Bez nowego sprzętu: dobrać reprezentatywne 300–500 zdjęć oraz 30–50
zapytań po polsku, z ręcznie oznaczonymi trafieniami. Uwzględnić rodzinę,
zdjęcia grupowe/dzieci, słabe światło, obrót, panoramy, screenshoty,
przedmioty i pojęcia podobne znaczeniowo. Osobny test dopasowań twarzy
powinien zawierać różne osoby podobne do siebie i tę samą osobę w różnym
wieku; nie przyjmować progu z benchmarku producenta jako gotowej reguły.

Porównać **CPU z limitem dwóch wątków** i **RTX/CUDA**, SigLIP 2 oraz
multilingual CLIP. Zapisać cold load/eksport/kompilację, warm zdjęcia/s,
czas i p95 zapytania, RAM/VRAM, jakość top-10 i wpływ na first frame oraz
p99 scrollowania Pinhole. Osobno mierzyć dekodowanie i inferencję; uwzględnić
czas tokenizacji oraz faktyczne operatory na akceleratorze. Dla twarzy
mierzyć fałszywe scalenia, pominięte detekcje i koszt ręcznych korekt.

Ten sam przypięty zestaw i zgodność wektorów/rankingu sprawdzić później na
Macu z macOS/Core ML i konkretnym Ryzen AI/Linux. ONNX jest wspólnym formatem,
a nie gwarancją pełnego wykonania na NPU. Konwersja, kwantyzacja i fallback
mogą zmienić jakość, więc wymagają własnego testu oraz raportu operatorów.
[Architektura Execution Providers](https://onnxruntime.ai/docs/execution-providers/).

**Kolejność wdrożenia:** wyszukiwanie semantyczne → ręczne/proponowane tagi
→ osoby z zatwierdzaniem grup → opcjonalne opisy/OCR. To zapewnia użyteczny
pierwszy etap bez uzależnienia projektu od NPU i bez kosztu generowania
opisów całej biblioteki przed pierwszym wyszukiwaniem.
