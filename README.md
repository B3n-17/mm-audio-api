# Audio API

A Majora's Mask: Recompiled mod that replaces the game's N64 audio engine, enabling extended audio capabilities for modders. It serves both as a standalone audio engine upgrade and as a public API library that other mods can import.

## Features

- **48kHz audio output** replacing the vanilla 32kHz pipeline
- **Extended sequence support** breaking the 255-sequence limit with 32-bit sequence IDs
- **Streamed audio files** supporting WAV, FLAC, MP3, Ogg Vorbis, and Opus
- **Custom soundfonts** with the ability to create instruments, drums, and sound effects from scratch or import vanilla ones
- **Procedural sequence generation** via the CSeq builder for constructing sequences programmatically
- **DMA callback system** for on-demand resource loading
- **Radio effect** optional band-pass filter for spatial BGM in shops (configurable in mod settings)

## Requirements

### Build Tools

| Tool | Notes |
|------|-------|
| `make` | Build orchestration |
| `cmake` (3.22+) | Native library configuration |
| `ninja` | CMake generator |
| `python` / `python3` | Build utilities |
| `zig` | Cross-compilation toolchain for native libraries |
| `clang` + `ld.lld` (LLVM) | MIPS C compiler and linker |
| `zip` | Packaging (Linux/macOS only) |

### Runtime Requirements

- [Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp) v1.2.1 or later

## Building

Clone the repository and initialize submodules:

```bash
git submodule update --init --recursive
```

Build everything (mod + native libraries for all platforms + distribution package):

```bash
make
```

### Individual Targets

| Target | Description |
|--------|-------------|
| `make` | Build everything: mod ELF, all extlibs, `.nrm`, and dist package |
| `make extlib-windows` | Cross-compile native library for Windows (x86_64) |
| `make extlib-linux` | Cross-compile native library for Linux (x86_64) |
| `make extlib-macos` | Cross-compile native library for macOS (aarch64) |
| `make nrm` | Generate the `.nrm` mod file |
| `make dist` | Create Thunderstore distribution package |
| `make clean` | Remove the build directory |

### Build Configuration

On first build, a `user_build_config.json` file is created at the repo root. Use it to configure:
- Compiler and linker paths (useful for non-system clang installs)
- CMake presets for each target platform

## Installation

### Thunderstore

Install the **Audio API** mod through your mod manager.

### Manual

Place `magemods_audio_api.nrm` and the platform-appropriate native library in the recomp mods directory.

## Using the API

### Setup

Include the API in your mod:

```c
#include <audio_api/all.h>
```

This pulls in all API headers. Individual headers can also be included selectively:

| Header | Purpose |
|--------|---------|
| `audio_api/types.h` | Type definitions, enums, info structs |
| `audio_api/base.h` | DMA callback registration |
| `audio_api/sequence.h` | Sequence management and playback |
| `audio_api/soundfont.h` | Soundfont, instrument, drum, and SFX management |
| `audio_api/samplebank.h` | Sample bank management |
| `audio_api/porcelain.h` | High-level filesystem loading and streamed audio |
| `audio_api/cseq.h` | Programmatic sequence builder |
| `audio_api/incbin.h` | Embed binary files in mod code |

Add `magemods_audio_api` as a dependency in your mod's `mod.toml`:

```toml
dependencies = [
    { id = "magemods_audio_api", version = "0.7.0" }
]
```

### Initialization Lifecycle

The API fires events during its 3-phase initialization. Register callbacks to set up your audio resources:

| Event | Phase | Use For |
|-------|-------|---------|
| `AudioApi_Init` | Queueing | Register sequences, soundfonts, sample banks, and audio files |
| `AudioApi_Ready` | Ready | Interact with the live audio system after all resources are loaded |

```c
RECOMP_CALLBACK("magemods_audio_api", AudioApi_Init)
void MyMod_AudioInit() {
    // Register your audio resources here
}

RECOMP_CALLBACK("magemods_audio_api", AudioApi_Ready)
void MyMod_AudioReady() {
    // Audio system is fully initialized
}
```

### Loading Audio Files from the Filesystem

The simplest way to add custom music is to load streamed audio files:

```c
AudioApiFileInfo fileInfo = {0};

// Create a looping BGM from an audio file
s32 seqId = AudioApi_CreateStreamedBgm(&fileInfo, "mod_data/audio", "my_song.ogg", AUDIOAPI_SEQ_IO_NONE);

// Create a one-shot fanfare
s32 fanfareId = AudioApi_CreateStreamedFanfare(&fileInfo, "mod_data/audio", "my_fanfare.flac", AUDIOAPI_SEQ_IO_NONE);
```

Supported formats: WAV, FLAC, MP3, Ogg Vorbis, Opus.

You can configure codec, channel type, cache strategy, and loop points via `AudioApiFileInfo`:

```c
AudioApiFileInfo fileInfo = {
    .codec = AUDIOAPI_CODEC_VORBIS,        // Or AUDIOAPI_CODEC_AUTO to detect
    .channelType = AUDIOAPI_CHANNEL_TYPE_STEREO,
    .cacheStrategy = AUDIOAPI_CACHE_PRELOAD_ON_USE,
    .loopStart = 0,
    .loopEnd = 0,       // 0 = end of file
    .loopCount = -1,    // -1 = loop forever
};
```

### Loading Raw Resources

For lower-level control, load raw binary resources (sequences, soundfonts, sample banks):

```c
AudioApiSequenceInfo seqInfo = {0};
AudioApi_AddSequenceFromFs(&seqInfo, "mod_data/audio", "my_sequence.aseq");

AudioApiSoundFontInfo fontInfo = {0};
AudioApi_AddSoundFontFromFs(&fontInfo, "mod_data/audio", "my_font.soundfont");

AudioApiSampleBankInfo bankInfo = {0};
AudioApi_AddSampleBankFromFs(&bankInfo, "mod_data/audio", "my_samples.bank");
```

### Embedding Binary Data

Use `INCBIN` to embed binary files directly in your mod:

```c
#include <audio_api/incbin.h>

INCBIN(mySequenceData, "assets/my_sequence.aseq");

// mySequenceData is a u8[] array, mySequenceData_end marks the end
size_t size = mySequenceData_end - mySequenceData;
```

### Sequence Management

```c
// Add a new sequence to the table
s32 seqId = AudioApi_AddSequence(&entry);

// Replace an existing sequence
AudioApi_ReplaceSequence(seqId, &newEntry);

// Restore the original sequence
AudioApi_RestoreSequence(seqId);

// Manage sequence fonts
s32 fontId = AudioApi_GetSequenceFont(seqId, 0);
AudioApi_AddSequenceFont(seqId, myFontId);
AudioApi_ReplaceSequenceFont(seqId, 0, myFontId);

// Set sequence flags
AudioApi_SetSequenceFlags(seqId, SEQ_FLAG_FANFARE);
```

#### Sequence Flags

| Flag | Description |
|------|-------------|
| `SEQ_FLAG_ENEMY` | Enemy music |
| `SEQ_FLAG_FANFARE` | Fanfare sequence |
| `SEQ_FLAG_FANFARE_KAMARO` | Kamaro-style fanfare |
| `SEQ_FLAG_RESTORE` | Restore previous BGM after playback |
| `SEQ_FLAG_RESUME` | Resume from where it left off |
| `SEQ_FLAG_RESUME_PREV` | Resume previous sequence |
| `SEQ_FLAG_SKIP_HARP_INTRO` | Skip harp intro |
| `SEQ_FLAG_NO_AMBIENCE` | Suppress ambience |

### Sequence Playback

```c
// Play as main BGM
AudioApi_PlayMainBgm(seqId);

// Play as fanfare
AudioApi_PlayFanfare(seqId, 0);

// Play as sub-BGM
AudioApi_PlaySubBgm(seqId, 0);

// Spatial playback (volume/pan based on position)
AudioApi_PlaySubBgmAtPos(&pos, seqId, 500.0f);
AudioApi_PlayObjSoundBgm(&pos, seqId);
AudioApi_PlayObjSoundFanfare(&pos, seqId);

// Direct player control
AudioApi_StartSequence(SEQ_PLAYER_BGM_MAIN, seqId, 0, 20);
```

#### Extended Sequence Commands

For sequence IDs beyond 255, use the extended command macros:

```c
SEQCMD_EXTENDED_PLAY_SEQUENCE(SEQ_PLAYER_BGM_MAIN, fadeIn, seqArgs, seqId);
SEQCMD_EXTENDED_QUEUE_SEQUENCE(SEQ_PLAYER_BGM_MAIN, fadeIn, priority, seqId);
SEQCMD_EXTENDED_UNQUEUE_SEQUENCE(SEQ_PLAYER_BGM_MAIN, fadeOut, seqId);
SEQCMD_EXTENDED_SETUP_PLAY_SEQUENCE(setupPlayer, targetPlayer, seqArgs, seqId);
```

### Soundfont Management

```c
// Create a new empty soundfont
s32 fontId = AudioApi_CreateEmptySoundFont();

// Or import a vanilla soundfont
s32 fontId = AudioApi_ImportVanillaSoundFont(fontData, bank1, bank2, nInst, nDrum, nSfx);

// Add instruments, drums, sound effects
s32 instId = AudioApi_AddInstrument(fontId, &myInstrument);
s32 drumId = AudioApi_AddDrum(fontId, &myDrum);
s32 sfxId  = AudioApi_AddSoundEffect(fontId, &mySfx);

// Replace existing entries
AudioApi_ReplaceInstrument(fontId, instId, &newInstrument);
AudioApi_ReplaceDrum(fontId, drumId, &newDrum);
AudioApi_ReplaceSoundEffect(fontId, sfxId, &newSfx);

// Set sample banks
AudioApi_SetSoundFontSampleBank(fontId, 0, bankId);
```

### Sample Bank Management

```c
s32 bankId = AudioApi_AddSampleBank(&entry);
AudioApi_ReplaceSampleBank(bankId, &newEntry);
AudioApi_RestoreSampleBank(bankId);
```

### DMA Callbacks

Register a callback to provide sample data on demand:

```c
s32 MyDmaCallback(void* ramAddr, size_t size, size_t offset, u32 arg0, u32 arg1, u32 arg2) {
    // Copy sample data to ramAddr
    return 0;
}

uintptr_t devAddr = AudioApi_AddDmaCallback(MyDmaCallback, arg0, arg1, arg2);
// Use devAddr as the sample address in your instruments/drums
```

### CSeq: Programmatic Sequence Builder

Build sequences in C code instead of writing binary MML:

```c
CSeqContainer* seq = cseq_create();

CSeqSection* sequence = cseq_sequence_create(seq);
CSeqSection* channel  = cseq_channel_create(seq);
CSeqSection* layer    = cseq_layer_create(seq);

// Set up sequence
cseq_initchan(sequence, 0x0001);   // Enable channel 0
cseq_ldchan(sequence, 0, channel); // Load channel
cseq_tempo(sequence, 120);

// Set up channel
cseq_instr(channel, 0);
cseq_vol(channel, 127);
cseq_pan(channel, 64);
cseq_ldlayer(channel, 0, layer);   // Load layer

// Write notes
cseq_notedvg(layer, 60, 48, 100, 200);  // C4, delay=48, vel=100, gate=200
cseq_notedvg(layer, 64, 48, 100, 200);  // E4
cseq_notedvg(layer, 67, 96, 100, 200);  // G4

// End sections
cseq_section_end(layer);
cseq_section_end(channel);
cseq_section_end(sequence);

// Compile and use
cseq_compile(seq, 0);
// seq->buffer->data contains the compiled sequence
// seq->buffer->size contains the size

// Clean up when done
cseq_destroy(seq);
```

## Configuration

The mod exposes the following settings in the recomp mod settings UI:

| Setting | Options | Default | Description |
|---------|---------|---------|-------------|
| Radio Effect in Shops | Off / On | Off | Applies a band-pass filter and gain to spatial BGM in shops, simulating a radio playing in the room. Takes effect on the next scene change. |

## Credits

Originally created by [magemods](https://github.com/magemods/mm-audio-api). This fork contains bug fixes and additional features to support full soundtrack replacement.

## License

See [LICENSE](LICENSE) for details.
