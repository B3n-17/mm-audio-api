# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.8.5] - 2026-05-16 - Unreleased
### Fixed
- [custom soundfonts] Out-of-bounds read when deep-copying custom envelopes terminated by `ADSR_GOTO` / `ADSR_RESTART`
- [polyphony] Audio-thread stack corruption when `numNotes > 88` (`noteIndices` array sized to support full 255-note range)
- [no effect] AudioApi_SetSequenceFlags uninitialized data
- [mem-leak] `CacheStrategy::Default` never resolved to `PreloadOnUse` on `Audiofile`/`Generic`/`SampleBank`
- [decoder] TOCTOU use-after-free between audio thread `decode()` and GC worker `close()` after stream idle
- [vfs] `ZipFile::close` wrote `curPos` outside `mutex` on the uncompressed path, racing with locked `read`/`seek`/`tell`; now locks unconditionally
- [mem-leak] `ZipArchive` leaked `mz_zip_archive` on init failure and never called `mz_zip_reader_end` (no destructor)
- [debug menu] crash safe-guards
- [vadpcm encode] Out-of-bounds read in loop-predictor pass when `loopStart` exceeds padded sample length
- [soundfont] `AudioLoad_RelocateSample` re-relocated already-relocated samples when `isRelocated` was stale (KSEG0 `sampleAddr` with `isRelocated=0`); restored vanilla's outer KSEG0 guard to skip re-entry
- `AudioApi_PlayFanfare` / async `PLAY_SEQUENCE` dereferenced `AudioThread_GetFontsForSequence` without bounds- or NULL-checking seqId
- [metadata] `Metadata::parseId3v2` misaligned 2-byte read (UB; SIGBUS on strict-alignment ARM).
- `Filesystem::isPathAllowed` / `openFile` rejected legitimate filenames containing `..`, accepted absolute `pathStr` that bypassed `baseDir`, and ignored symlinks



## [0.8.4] - 2026-03-21
### Added
- 32 kHz mode in mod config
- Custom audio debug event registration (will be displayed in debug HTML)
### Improved
- Debug HTML design and features

## [0.8.3] - 2026-03-15
### Fixed
- Heap: RSP cache invalidation
- Mayors theme mono playback
### Added
- HTML debug panel. Disabled by default; must be enabled before game start (http://127.0.0.1:18480/audio-debug.html)

## [0.8.2] - 2026-03-09
### Fixed
- Day 2 Termina soundtrack not playing
- Guru Guru stops playing if ringing the bell or destroying the crate
- Ikana Music Box scene entry volume jump

## [0.8.1] - 2026-02-26
### Added
- AudioApiFileInfo2, AudioApi_CreateStreamedBgmEx, AudioApi_CreateStreamedFanfareEx, allowing to define a volume offset per track
### Fixed
- Main bgm not correctly unmuted after enemy cleared
- Note priority, which caused N64 stealing soundtrack notes whenever it felt like it
### Changed
- better concern separation to make it easier to control audio behavior without locking down API to a specific use case
- Streamed audio dropping left channel due to note stealing prio and max notes restrictions
- SFX not working during Final Hours after seperation of concerns patch

## [0.7.3] - 2026-02-23
### Fixed
- Cavern playback in Astral Observatory
- Guru Guru handling

## [0.7.2] - 2026-02-22
### NOTE
- From now on all changes are documented here and versioning will follow SemVer versioning
### Fixed
- Enemy battle music not playing on second encounter
### Changed
- .gitignore
- make now deletes at starts and generates after build /dist and creates the Thunderstore zip to /publish

## [0.7.1] - 2026-02-20
### Fixed
- Removed warp to credits debug code

## [0.7.0] - 2026-02-20
### Note
- This is a fork of Magemods Audio API
### Fixed
- Frog Song
- Credits
- Ballad of Windfish
- Bremen Mask
- Fanfares looping behavior
- ZIP DMA
- loop index bugs
- queue overflow
- vanilla radio effect
- removed debug code breaking engine behavior
### Added
- Morning Jingle feature
- Porcelain registration type flags
- Mod menu config: Radio emulation in shops and minigames toggle (vanilla is on, but sounds really bad, so default is disabled)
