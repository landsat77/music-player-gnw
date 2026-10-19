# Music Player for Game & Watch Retro-Go SD

<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/45f4e71a-b0e0-49a8-a489-5f52a069411f" />



A music player for the Nintendo Game & Watch (Mario / Zelda) running
**Retro-Go SD** (v2.0.0-rc1 or later), as a homebrew
(`/homebrews/MusicPlayer.bin`). It plays the music of the SD card:

| Format | Extensions | Engine |
|---|---|---|
| ProTracker and other Amiga modules | `.mod`, `.nst`, `.m15`, `mod.*` | libxmp-lite |
| Scream Tracker 3 | `.s3m` | libxmp-lite |
| FastTracker 2 | `.xm` | libxmp-lite |
| Impulse Tracker | `.it` | libxmp-lite |
| MP3 (MPEG-1/2/2.5 layer III, CBR / VBR) | `.mp3` | minimp3 |
| MIDI (Standard MIDI File type 0 / 1, RIFF RMID, karaoke) | `.mid`, `.midi`, `.kar`, `.rmi` | TinySoundFont, with a SoundFont (`.sf2`) |

- A browser of the `/music` folder (and its sub-folders), folders first.
- A "now playing" screen: title / artist (module name, ID3v1 / ID3v2 tags),
  format, time and duration, oscilloscope, VU meter per channel (modules),
  position / row / tempo (modules).
- Play modes: repeat all, repeat one, shuffle, play once (the files of the
  current folder are the playlist).
- The music goes on in the browser; the screen can turn off by itself while
  the music plays (pause menu, *Options*).
- Savestates: the track and the position. "Resume" in the launcher starts
  the music again where it was.
- English or French, from the language of Retro-Go.

**Big modules.** A module whose samples do not fit in the RAM of the console
(about 780 KB for the whole player) has its samples copied to the external
flash the first time it is played (a progress bar is shown, a few seconds per
MB); later plays find them there and start at once. These modules play
without interpolation (the samples in the flash are read only). MP3 files of
any size are streamed from the SD card.

**MIDI.** MIDI files need a General MIDI SoundFont (`.sf2`) on the SD card:
`<song name>.sf2` next to the song, else the first `.sf2` of its folder, else
the first `.sf2` of `/music`. Small ones work best, for example TimGM6mb.sf2
(6 MB, GPL-2) or GeneralUser GS (30 MB). The SoundFont is copied to the
external flash the first time (a progress bar), then played from there.
Up to 48 voices at 32 kHz; only the instruments a song uses are set up.

Not supported: modules packed with an Amiga cruncher, the formats libxmp-lite
does not read (MED, OctaMED, 669, MTM... and the "FEST" MODs), MP3 files that
are not layer III.

## Installation

See [INSTALL.md](INSTALL.md) (English and French). In short:

1. Copy `MusicPlayer.bin` to `/homebrews/` on the SD card.
2. Put your music in `/music/` (sub-folders are fine).
3. Start "Music Player" from the Homebrew tab.

## Controls

| Game & Watch | Browser | Now playing |
|---|---|---|
| Up / Down | Move (held: repeat) | Seek: MP3 ±10 s, modules ±1 position |
| Left / Right | Page up / down | Previous / next track (previous: restart the track after 3 s) |
| A | Open the folder / play | Pause / play |
| B | Parent folder | Back to the browser (the music goes on) |
| GAME (START on Zelda units) | Now playing screen | Back to the browser |
| TIME (SELECT on Zelda units) | Next play mode | Next play mode |
| PAUSE/SET | Retro-Go menu | Retro-Go menu |

The Retro-Go menu has the savestates and, in *Options*: *Play mode* and
*Screen off* (never, 30 s, 1 min, 5 min). Any button turns the screen on
again.

## Building

Needs `arm-none-eabi-gcc` (hard-float, v10+), GNU Make, Python 3 + Pillow:

```
make            # → MusicPlayer.bin
make docker     # same, inside the Retro-Go SD builder image
```

### Testing without hardware

`harness/` builds the same player (core, decoders, heap) for Linux, headless,
with a virtual clock, scripted buttons, PPM screenshots and WAV output:

```
make -C harness
harness/build/mp_host /path/to/music -t 20000 -e 1000 -o /tmp/out -w /tmp/out/a.wav
harness/build/mp_host /path/to/music -p Modules/song.xm -H 780 -t 10000 -o /tmp/out
```

`-s "ms:BTN:1,ms:BTN:0,..."` presses buttons, `-S ms,ms` takes screenshots,
`-H kb` sets the heap (780 KB is the console's), `-f` French, `-q file -Q ms`
writes a savestate, `-L file` loads one at start, `-P` profiles the audio.
The flash cache is emulated (read only once written); `MP_FORCE_FLASH=1`,
`MP_FLASH_TWICE=1` and `MP_NEAREST=1` test that path.
`make -C harness SAN=1` builds it with AddressSanitizer and UBSan.

## Layout

| Path | Role |
|------|------|
| `src/xmp/` | libxmp-lite (libxmp/libxmp @ 5d4f6aa): MOD, S3M, XM, IT. Game & Watch changes are under `#ifdef LIBXMP_GNW`. |
| `src/mp3/` | minimp3 (lieff/minimp3 @ ea99364). Its 16 KB scratch is static on the console (`MINIMP3_GNW_STATIC_SCRATCH`). |
| `src/app/mp_core.c` | The player: browser, now playing screen, playlist, settings, savestates. |
| `src/midi/` | TinySoundFont (schellingb/TinySoundFont @ 853a0a1). Game & Watch changes under `#ifdef TSF_GNW`: 16-bit samples read in place from the flash, only the presets a song uses, float filter, fixed-point sample position. |
| `src/app/mp_midi.c` | The MIDI sequencer (SMF type 0 / 1, tempo, seeking). |
| `src/app/mp_player.c` | The decoders behind one API: modules (with the flash cache of the big ones), MP3 (ID3 tags, VBR, resampling). |
| `src/app/mp_draw.c`, `mp_font.c` | Drawing: 8 bit canvas, 8x8 font (Latin-1), icons. |
| `src/app/mp_alloc.c` | The heap: a boundary-tag allocator over several memory regions. |
| `src/app/mp_main.c`, `mp_libc.c` | Retro-Go side: entry point, display (LUT8), audio, buttons, pause menu, flash cache; the libc functions the firmware does not export. |
| `harness/` | Headless test harness. |
| `sdk/` | Retro-Go SD core / homebrew SDK (from retro-go-sd-templates). |

See `CLAUDE.md` for the internals.

## License

MIT (see `LICENSE`). libxmp is MIT (`src/xmp/LICENSE`), minimp3 is CC0
(`src/mp3/LICENSE`), TinySoundFont is MIT (`src/midi/LICENSE`), the 8x8 font is public domain, the SDK under `sdk/` is
MIT (`sdk/LICENSE.MIT`).
