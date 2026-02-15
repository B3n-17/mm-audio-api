# MM Audio API — AI Agent Reference

> **Purpose of this file:** Provide AI agents with enough context to work on this codebase without reading every file first. Start here, then drill into the specific files you need.

## What This Project Is

A **Majora's Mask: Recompiled** mod (`magemods_audio_api`) that:

1. **Replaces the game's audio engine** — patches the N64 audio system to support 48kHz output, >256 sequences, custom soundfonts, streamed audio files, and extended audio commands.
2. **Provides a public API** — other mods depend on this to add custom music, sound effects, and instruments.
3. **Ships as a native library** — cross-compiled to DLL/SO/DYLIB via Zig toolchain, loaded by the recomp runtime alongside a `.nrm` mod file containing the patched game code.

**Mod ID:** `magemods_audio_api` | **Game:** Majora's Mask | **Min Recomp:** 1.2.1

---

## Project Layout

```
mm-audio-api/
├── src/
│   ├── core/           # C — Replacement audio engine (patches vanilla functions)
│   ├── porcelain/      # C — High-level mod-facing API
│   ├── utils/          # C — Data structures (queue, dynamic array, misc)
│   └── extlib/         # C++ — Native library (decoders, VFS, resources, threading)
├── include/
│   ├── audio_api/      # Public API headers (what other mods #include)
│   ├── core/           # Internal core headers
│   ├── utils/          # Utility headers
│   ├── extlib/         # C++ library headers
│   └── recomp/         # Recomp framework headers (not ours)
├── thirdparty/         # Git submodules: plog, miniz, dr_libs, ogg, vorbis, opus, opusfile, utfcpp
├── Zelda64RecompSyms/  # Symbol definitions for MM function/data patching
├── mm-decomp/          # Vanilla MM decompilation (reference only)
├── cmake/              # Zig cross-compilation toolchain files
├── dist/               # Built artifacts (.nrm, .dll, .so, .dylib)
├── CMakeLists.txt      # Root build config
├── mod.toml            # Mod manifest and config
├── mod.ld              # Linker script
└── Makefile            # Build orchestration
```

---

## Architecture Overview

### Layer Diagram

```
┌─────────────────────────────────────────────────────┐
│  Other Mods (consumers)                             │
│  #include <audio_api/all.h>                         │
├─────────────────────────────────────────────────────┤
│  Porcelain Layer (src/porcelain/)                   │
│  High-level: load files, create streamed sequences  │
├─────────────────────────────────────────────────────┤
│  Core Audio API (src/core/)                         │
│  Mid-level: sequence/soundfont/samplebank tables,   │
│  extended commands, CSeq builder, flag management   │
├─────────────────────────────────────────────────────┤
│  Native Library (src/extlib/)                       │
│  Low-level C++: file I/O, audio decoding, caching,  │
│  DMA callbacks, worker thread preloading            │
├─────────────────────────────────────────────────────┤
│  N64 Audio Engine (patched vanilla via RECOMP_PATCH)│
│  Synthesis, MML script interpreter, RSP commands    │
├─────────────────────────────────────────────────────┤
│  Zelda 64: Recompiled Runtime                       │
└─────────────────────────────────────────────────────┘
```

### Key Architectural Patterns

- **RECOMP_PATCH** — Replaces vanilla functions at link time. The patched function has the same signature but new behavior. Most files in `src/core/` patch functions from `mm-decomp/src/audio/`.
- **RECOMP_CALLBACK / RECOMP_HOOK** — Event hooks for initialization lifecycle and per-frame ticks.
- **RECOMP_EXPORT / RECOMP_IMPORT** — Cross-mod function exports. The porcelain layer exports functions; other mods import them.
- **4-Phase Initialization** — `gAudioApiInitPhase`: NOT_READY → QUEUEING (mods register resources) → QUEUED (drain deferred commands) → READY (live). During QUEUEING, mutations are enqueued and deduplicated; they execute at the QUEUEING→READY transition.
- **Extended Sequence IDs** — Vanilla uses u8 seqId (max 255). This mod uses s32 seqId with shadow arrays (`sExtSeqPlayersSeqId[]`, `gExtActiveSeqs[]`) to break the 255-sequence limit.
- **DMA Callback System** — Resources loaded by the native library are accessed during synthesis via registered DMA callbacks. `AudioApi_AddDmaCallback()` returns a virtual device address (devAddr ≥ 0x10000000) that the RSP uses to request sample data.
- **Mod Memory Workaround** — The RSP can only DMA from N64 address space (KSEG0), not mod memory. `IS_MOD_MEMORY()` detects mod addresses and `AudioApi_RspCacheMemcpy()` copies data into the audio heap for RSP access.

---

## Source File Guide

### `src/core/` — Replacement Audio Engine

| File | Responsibility | Key Functions |
|------|---------------|---------------|
| [init.c](src/core/init.c) | 4-phase init lifecycle, 48kHz buffer sizing, heap setup | `AudioLoad_Init` (PATCH), `AudioApi_ExtLibInit`, `AudioApi_ExtLibReady` |
| [synthesis.c](src/core/synthesis.c) | 48kHz RSP audio pipeline, CODEC_S16 support, mod memory workaround | `AudioSynth_Update` (PATCH), `AudioSynth_ProcessSample` (PATCH) |
| [audio_cmd.c](src/core/audio_cmd.c) | Extended command queue for >256 seqIds | `AudioSeq_QueueSeqCmd` (PATCH), `AudioApi_ProcessSeqCmd`, `AudioApi_QueueExtendedSeqCmd` |
| [seqplayer.c](src/core/seqplayer.c) | MML bytecode interpreter, extended seqId storage | `AudioScript_SequencePlayerProcessSequence` (PATCH), `AudioScript_SequenceChannelProcessScript` (PATCH) |
| [sequence.c](src/core/sequence.c) | Sequence table management (add/replace/restore), font table | `AudioApi_AddSequence`, `AudioApi_ReplaceSequence`, `AudioApi_AddSequenceFont` |
| [sequence_functions.c](src/core/sequence_functions.c) | High-level playback: BGM, fanfare, spatial, sub-BGM, scene music | `AudioApi_PlayMainBgm`, `AudioApi_PlayFanfare`, `AudioApi_PlaySubBgmAtPos`, `AudioApi_StartSequence` |
| [soundfont.c](src/core/soundfont.c) | Soundfont/instrument/drum management, sample dedup via FNV-32a | `AudioApi_CreateEmptySoundFont`, `AudioApi_AddInstrument`, `AudioLoad_RelocateFont` (PATCH) |
| [samplebank.c](src/core/samplebank.c) | Sample bank table management | `AudioApi_AddSampleBank`, `AudioApi_ReplaceSampleBank` |
| [cseq.c](src/core/cseq.c) | Runtime MML sequence builder (programmatic sequence generation) | `cseq_create`, `cseq_compile`, `cseq_ldchan`, `cseq_notedvg`, etc. |
| [effects.c](src/core/effects.c) | 48kHz effect rescaling (Haas delay, reverb, comb filter) | `AudioApi_EffectsInit`, `AudioApi_ApplyCombFilter` |
| [load.c](src/core/load.c) | DMA callback registration, native DMA handler | `AudioApi_AddDmaCallback`, `AudioApi_NativeDmaCallback` |
| [load_status.c](src/core/load_status.c) | Load status tracking with hashset-backed caches | `AudioApi_GetTableEntryLoadStatus`, `AudioApi_PushFakeCache` |
| [heap.c](src/core/heap.c) | Audio heap management, RSP cache for mod memory | `AudioHeap_LoadBufferAlloc`, `AudioApi_RspCacheMemcpy` |

### `src/porcelain/` — High-Level Mod-Facing API

| File | Responsibility | Key Functions |
|------|---------------|---------------|
| [resource.c](src/porcelain/resource.c) | Filesystem resource loading, routes to native | `AudioApi_AddResourceFromFs`, `AudioApi_AddAudioFileFromFs`, `AudioApi_GetResourceDevAddr` |
| [audiofile.c](src/porcelain/audiofile.c) | Converts audio files (WAV/FLAC/MP3/OGG/Opus) into playable sequences | `AudioApi_CreateStreamedSequence`, `AudioApi_CreateStreamedBgm`, `AudioApi_CreateStreamedFanfare` |

### `src/utils/` — Data Structures

| File | Responsibility |
|------|---------------|
| [queue.c](src/utils/queue.c) | FIFO command queue (`RecompQueue`) — used for deferred init commands |
| [dynamicdataarray.c](src/utils/dynamicdataarray.c) | Generic dynamic array (`DynamicDataArray`) — type-erased, 1.5x growth |
| [misc.c](src/utils/misc.c) | Ref-counting, FNV-32a hash, memory compare, string dup, hex dump |

### `src/extlib/` — Native C++ Library

| Directory | Responsibility |
|-----------|---------------|
| [decoder/](src/extlib/decoder/) | Audio format decoders: WAV (dr_wav), FLAC (dr_flac), MP3 (dr_mp3), Vorbis, Opus |
| [resource/](src/extlib/resource/) | Resource management: Generic (raw file), Audiofile (decoded+streamed), SampleBank |
| [vfs/](src/extlib/vfs/) | Virtual filesystem: native files + ZIP archive support |
| [main.cpp](src/extlib/main.cpp) | Native library entry, global VFS and resource map |
| [thread.cpp](src/extlib/thread.cpp) | Worker thread for background preloading |

---

## Public API Quick Reference

All public headers are under `include/audio_api/`. Mods include `<audio_api/all.h>`.

### Resource Loading (Porcelain)

```c
// Load resources from filesystem (dir = base directory, filename = relative path)
bool AudioApi_AddSequenceFromFs(AudioApiSequenceInfo* info, char* dir, char* filename);
bool AudioApi_AddSoundFontFromFs(AudioApiSoundFontInfo* info, char* dir, char* filename);
bool AudioApi_AddSampleBankFromFs(AudioApiSampleBankInfo* info, char* dir, char* filename);
bool AudioApi_AddAudioFileFromFs(AudioApiFileInfo* info, char* dir, char* filename);

// Create playable sequence from a decoded audio file
s32  AudioApi_CreateStreamedBgm(AudioApiFileInfo* info, char* dir, char* filename);      // loops forever
s32  AudioApi_CreateStreamedFanfare(AudioApiFileInfo* info, char* dir, char* filename);   // plays once

// Get DMA device address for a loaded resource
uintptr_t AudioApi_GetResourceDevAddr(u32 resourceId);
```

### Sequence Management

```c
s32  AudioApi_AddSequence(AudioTableEntry* entry);         // returns seqId
void AudioApi_ReplaceSequence(s32 seqId, AudioTableEntry* entry);
void AudioApi_RestoreSequence(s32 seqId);

s32  AudioApi_AddSequenceFont(s32 seqId, s32 fontId);      // returns fontNum
void AudioApi_ReplaceSequenceFont(s32 seqId, s32 fontNum, s32 fontId);

u8   AudioApi_GetSequenceFlags(s32 seqId);
void AudioApi_SetSequenceFlags(s32 seqId, u8 flags);
s32  AudioApi_IsSequencePlaying(s32 seqId);
```

### Sequence Playback

```c
void AudioApi_PlayMainBgm(s32 seqId);
void AudioApi_PlayFanfare(s32 seqId, u16 seqArgs);
void AudioApi_PlaySubBgm(s32 seqId, u16 seqArgs);
void AudioApi_PlaySubBgmAtPos(Vec3f* pos, s32 seqId, f32 maxDist);
void AudioApi_PlayObjSoundBgm(Vec3f* pos, s32 seqId);
void AudioApi_PlayObjSoundFanfare(Vec3f* pos, s32 seqId);
void AudioApi_StartSequence(u8 seqPlayerIndex, s32 seqId, u16 seqArgs, u16 fadeInDuration);
```

### Soundfont Management

```c
s32  AudioApi_CreateEmptySoundFont(void);                   // returns fontId
s32  AudioApi_ImportVanillaSoundFont(uintptr_t* fontData, u8 bank1, u8 bank2, u8 nInst, u8 nDrum, u16 nSfx);
s32  AudioApi_AddSoundFont(AudioTableEntry* entry);
void AudioApi_SetSoundFontSampleBank(s32 fontId, s32 bankNum, s32 bankId);

s32  AudioApi_AddInstrument(s32 fontId, Instrument* inst);  // returns instId
s32  AudioApi_AddDrum(s32 fontId, Drum* drum);
s32  AudioApi_AddSoundEffect(s32 fontId, SoundEffect* sfx);
void AudioApi_ReplaceInstrument(s32 fontId, s32 instId, Instrument* inst);
```

### Sample Bank Management

```c
s32  AudioApi_AddSampleBank(AudioTableEntry* entry);        // returns bankId
void AudioApi_ReplaceSampleBank(s32 bankId, AudioTableEntry* entry);
void AudioApi_RestoreSampleBank(s32 bankId);
```

### DMA Callback Registration

```c
uintptr_t AudioApi_AddDmaCallback(AudioApiDmaCallback callback, u32 arg0, u32 arg1, u32 arg2);
// callback signature: s32 (*)(void* ramAddr, size_t size, size_t offset, u32 arg0, u32 arg1, u32 arg2)
```

### CSeq — Runtime Sequence Builder

```c
CSeqContainer* cseq_create(void);
void           cseq_compile(CSeqContainer* root, size_t base_offset);
void           cseq_destroy(CSeqContainer* root);

CSeqSection*   cseq_sequence_create(CSeqContainer* root);
CSeqSection*   cseq_channel_create(CSeqContainer* root);
CSeqSection*   cseq_layer_create(CSeqContainer* root);
CSeqSection*   cseq_label_create(CSeqSection* section);

// Sequence commands
bool cseq_initchan(CSeqSection* seq, u16 bitmask);
bool cseq_freechan(CSeqSection* seq, u16 bitmask);
bool cseq_tempo(CSeqSection* seq, u8 bpm);
bool cseq_ldchan(CSeqSection* seq, u8 channelNum, CSeqSection* channel);

// Channel commands
bool cseq_ldlayer(CSeqSection* ch, u8 layerNum, CSeqSection* layer);
bool cseq_font(CSeqSection* ch, u8 fontId);
bool cseq_instr(CSeqSection* ch, u8 instNum);
bool cseq_pan(CSeqSection* ch, u8 pan);
bool cseq_vol(CSeqSection* ch, u8 amount);

// Layer commands (note playback)
bool cseq_notedvg(CSeqSection* layer, u8 pitch, u16 delay, u8 velocity, u8 gateTime);
bool cseq_notedv(CSeqSection* layer, u8 pitch, u16 delay, u8 velocity);

// Flow control
bool cseq_loop(CSeqSection* section, u8 count);
bool cseq_loopend(CSeqSection* section);
bool cseq_jump(CSeqSection* section, CSeqSection* target);
bool cseq_delay(CSeqSection* section, u16 delay);
```

---

## Key Data Types

```c
// Resource info (base for sequence, soundfont, samplebank)
typedef struct {
    u32 resourceId;
    u32 filesize;
    AudioApiCacheStrategy cacheStrategy;  // NONE, PRELOAD, PRELOAD_ON_USE, PRELOAD_ON_USE_NO_EVICT
} AudioApiResourceInfo;

// Audio file info (decoded audio: WAV, FLAC, MP3, Vorbis, Opus)
typedef struct {
    u32 resourceId;
    u32 trackCount;
    u32 sampleRate;
    u32 sampleCount;
    u32 loopStart;
    u32 loopEnd;
    s32 loopCount;           // -1 = infinite, 0 = once, N = N+1 times
    AudioApiCodec codec;     // AUTO, WAV, FLAC, MP3, VORBIS, OPUS
    AudioApiChannelType channelType;  // DEFAULT, MONO, STEREO
    AudioApiCacheStrategy cacheStrategy;
} AudioApiFileInfo;

// Sequence flags (per-seqId behavioral metadata)
#define SEQ_FLAG_ENEMY            (1 << 0)  // Enemy BGM
#define SEQ_FLAG_FANFARE          (1 << 1)  // Fanfare (ducks main BGM)
#define SEQ_FLAG_FANFARE_KAMARO   (1 << 2)  // Kamaro fanfare variant
#define SEQ_FLAG_RESTORE          (1 << 3)  // Restore to vanilla on unload
#define SEQ_FLAG_RESUME           (1 << 4)  // Resume playback state
#define SEQ_FLAG_RESUME_PREV      (1 << 5)  // Resume previous sequence
#define SEQ_FLAG_SKIP_HARP_INTRO  (1 << 6)  // Skip harp intro
#define SEQ_FLAG_NO_AMBIENCE      (1 << 7)  // Disable ambient sounds
```

---

## Audio Pipeline (How Sound Gets to Speakers)

```
Game Thread                          Audio Thread (per frame)
───────────                          ─────────────────────────
AudioApi_PlayMainBgm(seqId)         AudioSeq_ProcessSeqCmds()
  → SEQCMD_EXTENDED_PLAY_SEQUENCE     → drain sAudioSeqCmdQueue
  → sAudioSeqCmdQueue.push()            → PLAY: AudioApi_StartSequence()
                                         → QUEUE: insert sExtSeqRequests[]
                                         → SETUP: deferred command

                                     AudioScript_ProcessSequences() (3x/frame)
                                       → MML sequence interpreter
                                         → channel scripts → layer scripts
                                         → note allocation, volume updates

                                     AudioSynth_Update() (6 RSP passes/frame)
                                       → for each active note:
                                         1. Decode (ADPCM/S8/S16) → DMEM
                                         2. Resample (pitch shift)
                                         3. Gain (volume)
                                         4. Filter (IIR)
                                         5. Comb filter (chorus/flange)
                                         6. Surround
                                         7. Envelope (mono → stereo L/R + reverb)
                                         8. Haas effect (stereo widening)
                                       → Interleave → AI buffer → DAC
```

**48kHz Strategy:** Sequence scripts update at the original 32kHz rate (3x/frame). RSP synthesis is subdivided into 2 passes per update (6 total/frame) with sample counts scaled by FREQ_FACTOR=1.5.

---

## How Streamed Audio Works

The audiofile system bridges arbitrary audio formats (WAV/FLAC/MP3/OGG/Opus) into the N64 sequence engine:

1. **Load** — `AudioApi_AddAudioFileFromFs()` loads + decodes the file via the native C++ library
2. **Soundfont** — Creates an empty soundfont; for each track, wraps the decoded PCM as a `Sample` (CODEC_S16) with tuning = `sampleRate / 32000.0f`, wraps in an `Instrument`, adds to soundfont
3. **Sequence** — Generates a minimal CSeq binary: each channel plays one instrument, each layer plays a single C4 note for the full duration. Stereo files use 2 layers per channel (L pan=0, R pan=127)
4. **Playback** — The generated sequence is indistinguishable from a real sequence. The DMA callback streams sample data on demand during synthesis

---

## Initialization Lifecycle

```
Recomp Runtime loads mod
  → RECOMP_CALLBACK("*", recomp_on_init)   # misc.c: init refcounter
  → AudioLoad_Init() PATCH                  # init.c: full replacement
    ├── gAudioApiInitPhase = NOT_READY
    ├── Initialize audio heap, buffers, tables
    ├── gAudioApiInitPhase = QUEUEING
    ├── AudioApi_InitInternal()              # effects init, extlib init
    ├── AudioApi_Init()                      # PUBLIC EVENT: mods register resources here
    ├── gAudioApiInitPhase = QUEUED
    ├── AudioApi_ReadyInternal()             # drain deferred queues
    ├── AudioApi_Ready()                     # PUBLIC EVENT: mods can interact post-load
    └── gAudioApiInitPhase = READY
```

**During QUEUEING:** API calls like `AudioApi_ReplaceSequence()` enqueue commands (deduplicated). They execute when draining at the QUEUED→READY transition.

**After READY:** API calls execute immediately.

---

## Native Library Exports

Defined in `mod.toml`, implemented in `src/extlib/main.cpp`:

| Function | Purpose |
|----------|---------|
| `AudioApiNative_Init` | Initialize VFS, decoders, worker thread |
| `AudioApiNative_Ready` | Signal initialization complete |
| `AudioApiNative_Tick` | Per-frame update (cache management) |
| `AudioApiNative_Dma` | DMA callback: stream decoded audio to N64 memory |
| `AudioApiNative_AddResource` | Load generic resource (sequence/soundfont) from filesystem |
| `AudioApiNative_AddAudioFile` | Load + decode audio file, register for streaming |
| `AudioApiNative_AddSampleBank` | Load raw sample bank from filesystem |

---

## Debugging Guide

### Where to start for bugs

1. Check `@mod` comments in `src/core/*.c` — these mark deviations from vanilla
2. Compare patched functions against their vanilla counterparts in `mm-decomp/src/audio/lib/`
3. Common issues: off-by-one in table indexing, early break in loops, bounds overflow when tables grow

### Key state to inspect

- `gAudioApiInitPhase` — current init phase
- `gAudioCtx.sequenceTable` / `gAudioCtx.soundFontTable` / `gAudioCtx.sampleBankTable` — resource tables
- `sExtSeqPlayersSeqId[SEQ_PLAYER_MAX]` — actual s32 seqId per player
- `gExtActiveSeqs[SEQ_PLAYER_MAX]` — extended sequence state (setup cmds, prev seq, etc.)
- `sExtSeqFlags[]` / `sExtSeqLoadStatus[]` — per-sequence metadata
- `sAudioSeqCmdQueue` — pending audio commands

### Known hotspots

- `AudioLoad_Init` memory zeroing loop in [init.c](src/core/init.c)
- Setup-command loop control flow in [sequence_functions.c](src/core/sequence_functions.c)
- Soundfont relocation in [soundfont.c](src/core/soundfont.c) — pointer relocation between ROM-relative and absolute addresses
- RSP cache coherency in [synthesis.c](src/core/synthesis.c) — mod memory detection and cache copying

### Recording findings

Use `DEBUG.md` with status: `confirmed`, `likely`, `needs repro`, `dismissed`

---

## Consumer Mod Example: OST Replacement

This shows how a mod uses the Audio API to replace in-game music with streamed audio files. This is the primary use case for most consumer mods.

### Minimal Setup

```c
#include "audio_api/sequence.h"
#include "audio_api/porcelain.h"

// Hook into Audio API initialization event
RECOMP_CALLBACK("magemods_audio_api", AudioApi_Init) void onAudioApiInit() {
    // Load a FLAC file as looping BGM from a zip archive
    s32 seqId = AudioApi_CreateStreamedBgm(NULL, "mymod.zip", "music.flac");

    // Replace vanilla sequence with the streamed one
    AudioApi_ReplaceSequence(NA_BGM_TERMINA_FIELD, &gAudioCtx.sequenceTable->entries[seqId]);
    AudioApi_ReplaceSequenceFont(NA_BGM_TERMINA_FIELD, 0, AudioApi_GetSequenceFont(seqId, 0));
}
```

### Key Patterns

**1. Initialization — always use `AudioApi_Init` callback:**
```c
RECOMP_CALLBACK("magemods_audio_api", AudioApi_Init) void onAudioApiInit() {
    // This runs during the QUEUEING phase — safe to register resources
}
```

**2. Loading audio — BGM vs Fanfare:**
```c
// BGM: loops forever, for background music
s32 bgmId = AudioApi_CreateStreamedBgm(NULL, "mymod.zip", "field_theme.flac");

// Fanfare: plays once, for jingles/stingers (auto-sets SEQ_FLAG_FANFARE)
s32 fanfareId = AudioApi_CreateStreamedFanfare(NULL, "mymod.zip", "item_get.flac");
```

**3. Replacing vanilla sequences:**
```c
// After creating a streamed sequence, copy its table entry + font to the vanilla slot
AudioApi_ReplaceSequence(NA_BGM_TERMINA_FIELD, &gAudioCtx.sequenceTable->entries[seqId]);
AudioApi_ReplaceSequenceFont(NA_BGM_TERMINA_FIELD, 0, AudioApi_GetSequenceFont(seqId, 0));
```

**4. File sources — supports filesystem paths and ZIP archives:**
```c
// From a ZIP file bundled with the mod
AudioApi_CreateStreamedBgm(NULL, "mymod.zip", "music/theme.flac");

// From a loose file on disk (dir = base directory)
AudioApi_CreateStreamedBgm(NULL, "mymod_assets", "theme.flac");
```

**5. Supported formats:** WAV, FLAC, MP3, OGG Vorbis, Opus (auto-detected from file extension)

**6. Per-frame hooks — manipulate playback at runtime:**
```c
// Hook into the audio script processing loop (runs on audio thread)
RECOMP_HOOK("AudioScript_ProcessSequences") void onProcessSequences() {
    // Per-frame audio logic (crossfades, volume control, etc.)
}

// Hook into per-player sound processing
RECOMP_HOOK("AudioScript_SequencePlayerProcessSound") void onSound(SequencePlayer* seqPlayer) {
    // Per-channel volume manipulation, etc.
    s32 seqId = AudioApi_GetSeqPlayerSeqId(seqPlayer);
    // ... modify seqPlayer->channels[i]->volume
}
```

**7. Importing additional functions from the Audio API:**
```c
RECOMP_IMPORT("magemods_audio_api", s32 AudioApi_GetSeqPlayerSeqId(SequencePlayer* seqPlayer));
```

### Stereo Audio Layout

When loading stereo files, the Audio API maps tracks to channels/layers:
- **Mono** (1 track): 1 channel, 1 layer, centered (pan=64)
- **Stereo** (2 tracks): 1 channel, 2 layers — Layer 0 = Left (pan=0), Layer 1 = Right (pan=127)
- **Multi-track**: even channels = one group, odd channels = another (useful for dual-audio crossfade mods)

### Dual-Audio Crossfade Pattern

A common advanced pattern: ship both a remastered and original soundtrack in the same file, using alternating stereo channels. Toggle between them at runtime with per-channel volume control:

```c
// Even channels = remaster, odd channels = original OST
for (int i = 0; i < ARRAY_COUNT(seqPlayer->channels); i++) {
    SequenceChannel* ch = seqPlayer->channels[i];
    if (ch == NULL) continue;
    ch->volume = (i % 2 == 0) ? remasterVolume : ostVolume;
    ch->changes.s.volume = true;
}
```

---

## Build System

- **Toolchain:** Zig cross-compiler (cmake/zig-toolchain-*.cmake)
- **Platforms:** Windows x86_64, Linux x86_64, macOS aarch64
- **C++ Standard:** C++23
- **Dependencies:** miniz, ogg, vorbis, opus, opusfile, dr_libs, plog, utfcpp
- **Build:** `make` or CMake directly
- **Output:** `build/mod.elf` → `dist/magemods_audio_api.nrm` + platform native libraries

---

## Scope Guidance for AI Agents

- **DO** focus on `src/` and `include/` for all tasks
- **DO** check `mm-decomp/src/audio/lib/` when comparing against vanilla behavior
- **DO NOT** try to understand all of `mm-decomp/` — it's 500k+ lines of decompiled game code
- **DO NOT** modify files in `thirdparty/`, `Zelda64RecompSyms/`, or `mm-decomp/` — they are git submodules
- **DO** look at `@mod` comments in patched functions to understand intentional deviations
- **DO** record debugging findings in `DEBUG.md`
