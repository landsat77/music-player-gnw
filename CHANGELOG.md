# Changelog

This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Release tags must
match a section heading exactly (for example `v1.0.0`): CI uses the matching
section as the GitHub Release notes.

## [Unreleased]

## [v1.1.1] - 2026-10-19

### Changed

- Names too long for the screen scroll instead of ending with "..": the
  selected file or folder in the browser, the folder at the top, and the
  title, artist and format on the "now playing" screen (still for 1.5 s at
  each end).

### Fixed

- Accented file and folder names (UTF-8 on the SD card) were shown garbled:
  they are now shown in Latin-1 (é, è, ê, à, ç...).

## [v1.1.0] - 2026-10-19

### Added

- MIDI files (`.mid`, `.midi`, `.kar`, `.rmi`; SMF type 0 and 1) played with
  TinySoundFont and a SoundFont (`.sf2`) from the SD card: `<song>.sf2`, else
  the first `.sf2` of the folder, else of `/music`. The SoundFont is copied
  to the external flash once (progress bar) and read from there; only the
  instruments a song uses are set up. 32 kHz, up to 48 voices, seeking,
  savestates, track name / copyright shown, voices and tempo on screen.
- A message when a MIDI file has no SoundFont.

## [v1.0.1] - 2026-10-19

### Fixed

- Modules with many channels and patterns could not load ("not a module",
  out of memory), e.g. an 18 channel XM of 92 patterns: the identical
  tracks of the patterns (empty channels, repeated parts) are now kept once.
  That XM went from 837 KB of patterns to 129 KB; the sound is unchanged
  (checked sample for sample).

## [v1.0.0] - 2026-10-19

### Added

- Music player for the SD card as a Retro-Go SD GWHB homebrew (firmware ABI
  v2, Retro-Go SD v2.0.0-rc1 or later): MOD, S3M, XM, IT with libxmp-lite,
  MP3 with minimp3.
- Browser of `/music` and its sub-folders; "now playing" screen with title /
  artist (ID3v1 / ID3v2), time, oscilloscope, VU meters, position / row /
  tempo of the modules.
- Play modes: repeat all, repeat one, shuffle, play once; seeking; the music
  goes on in the browser.
- Big modules: samples copied once to the external flash (streamed and
  converted on the way, compressed IT samples included), used in place on
  later plays.
- Savestates of the track and position, resume from the launcher.
- Pause menu options: play mode, screen off (never, 30 s, 1 min, 5 min).
- English and French texts.
