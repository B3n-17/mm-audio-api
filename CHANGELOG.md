# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
