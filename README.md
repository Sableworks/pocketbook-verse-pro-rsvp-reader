# PocketBook Verse Pro Color — RSVP Reader

**RSVP Reader** — a Rapid Serial Visual Presentation (RSVP) app for reading EPUB on the **PocketBook Verse Pro Color (B300)**. Built with InkView / SDK 6. Other PocketBook models are untested — use at your own risk.

The on-device UI is in Polish; this README is in English for GitHub.

## Download

**[Download the latest release](https://github.com/Sableworks/pocketbook-verse-pro-rsvp-reader/releases/latest)** — ZIP with `rsvp.app`, ready for your PocketBook. No build tools required.

(GitHub does not allow uploading bare `.app` files, so the binary ships inside a ZIP.)

## Install on device

1. Download the ZIP from the [latest release](https://github.com/Sableworks/pocketbook-verse-pro-rsvp-reader/releases/latest) and unzip it to get `rsvp.app`.
2. Connect the PocketBook via USB (PC Link / mass storage).
3. Copy `rsvp.app` to `applications/` on device storage (`/mnt/ext1/applications/`).
4. **Disconnect USB** (important — with PC Link active, apps often cannot see files).
5. Launch **RSVP Reader** (`rsvp.app`) from the applications menu.

Reading progress and WPM are saved in `/mnt/ext1/.rsvp_saves.ini`.

## Controls

| Action | Gesture / key |
| --- | --- |
| Pause → options panel | Tap while playing |
| Start / Resume | Tap “Start” / “Wznów” (or tap the word preview) |
| Speed ±10 WPM | Swipe up/down (play or pause) or −10 / +10 in the panel (max **200 WPM**; default 180) |
| Word grouping | At most **2** words per frame (one function word + next). Never 3+. |
| Chapters | “Rozdziały” row in the pause panel |
| ±1 chapter / start of book | Navigation row in the pause panel |
| Another book | “Inna książka” or **Back** |
| Exit app | **Home** |

After opening a book, playback does **not** start automatically — you see a panel with progress (%), WPM, and Start/Resume.

## Build from source (optional)

Only needed if you want to modify or rebuild the app. End users should use the [prebuilt release](https://github.com/Sableworks/pocketbook-verse-pro-rsvp-reader/releases/latest).

The Docker image includes the PocketBook ARM toolchain and dependencies (libzip, libxml2).

```bash
# One-time — build the image (Apple Silicon: force amd64)
docker build --platform linux/amd64 -t pb-rsvp-builder .

# Compile
docker run --rm --platform linux/amd64 -v "$(pwd):/project" pb-rsvp-builder \
  -c 'mkdir -p build && cd build && cmake -DCMAKE_TOOLCHAIN_FILE=/SDK/share/cmake/arm_conf.cmake .. && cmake --build .'
```

Output: `build/rsvp.app` (binary filename; the app displays as **RSVP Reader** in the UI).

## Host tests (optional)

Simple smoke test for helper algorithms (no InkView SDK):

```bash
cc -O2 -Wall -o tests/host_smoke tests/host_smoke.c && ./tests/host_smoke
```

Full EPUB parsing (spine / OPF) requires libzip + libxml2 and is verified on device / in the Docker image.

## Credits

Made by Mateusz Blumensztajn ([Sableworks](https://github.com/Sableworks)).
