# PocketBook RSVP

**RSVP Reader** — aplikacja RSVP (Rapid Serial Visual Presentation) do czytania EPUB na czytnikach PocketBook (InkView / SDK 6).

## Budowanie (Docker)

Obraz zawiera toolchain ARM PocketBook oraz zależności (libzip, libxml2).

```bash
# Jednorazowo — zbuduj obraz (Apple Silicon: wymuś amd64)
docker build --platform linux/amd64 -t pb-rsvp-builder .

# Kompilacja
docker run --rm --platform linux/amd64 -v "$(pwd):/project" pb-rsvp-builder \
  -c 'mkdir -p build && cd build && cmake -DCMAKE_TOOLCHAIN_FILE=/SDK/share/cmake/arm_conf.cmake .. && cmake --build .'
```

Wynik: `build/rsvp.app` (nazwa pliku binarnego; w UI aplikacja wyświetla się jako **RSVP Reader**).

## Instalacja na urządzeniu

1. Podłącz PocketBook przez USB (tryb PC Link / masa).
2. Skopiuj `build/rsvp.app` do katalogu `applications/` na pamięci urządzenia (`/mnt/ext1/applications/`).
3. **Odłącz USB** (ważne — przy podłączonym PC Link aplikacje często nie widzą plików).
4. Uruchom **RSVP Reader** (`rsvp.app`) z menu aplikacji.

Postęp i tempo zapisują się w `/mnt/ext1/.rsvp_saves.ini`.

## Sterowanie

| Akcja | Gest / klawisz |
| --- | --- |
| Pauza → panel opcji | Tap podczas odtwarzania |
| Start / Wznów | Tap „Start” / „Wznów” (lub podgląd słowa) |
| Tempo ±10 sł/min | Swipe w górę/dół (play lub pauza) albo −10 / +10 w panelu |
| Pomiń słowa | ◄ / ► (PREV / NEXT) |
| Rozdziały | Wiersz „Rozdziały” w panelu pauzy |
| ±1 rozdział / początek | Wiersz nawigacji w panelu pauzy |
| Inna książka | „Inna książka” lub **Back** |
| Wyjście z aplikacji | **Home** |

Po otwarciu książki odtwarzanie **nie** startuje samo — widać panel z postępem (%), WPM i przyciskiem Start/Wznów.

## Testy hosta (opcjonalnie)

Prosty smoke test algorytmów pomocniczych (bez SDK InkView):

```bash
cc -O2 -Wall -o tests/host_smoke tests/host_smoke.c && ./tests/host_smoke
```

Pełny parser EPUB (spine / OPF) wymaga libzip + libxml2 i jest weryfikowany na urządzeniu / w obrazie Docker.
