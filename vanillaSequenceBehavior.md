# Vanilla Sequence IO Behavior

Analysis of all Majora's Mask sequences that use IO ports or non-musical instructions.
Relevant for understanding what behavior must be replicated when replacing sequences with streamed audio.

## System Sequences (cannot be replaced with streamed audio)

| Seq | Name | IO Ports | Notes |
|-----|------|----------|-------|
| **0** | `NA_BGM_GENERAL_SFX` | 0-5 R+W | The entire SFX dispatcher engine. Not replaceable. |
| **1** | `NA_BGM_AMBIENCE` | 0,1,2,4,5 R+W | Channel enable bitmask + ambient variant selection. Not replaceable. |
| **122** | (launcher) | 2 Read | Spin-waits for `IO_PORT_2 >= 0`, then `runseq` to that sequence ID. Trampoline utility. |

## Music with Startup Path Selection

These sequences read an IO port once at startup to select a playback path. Simple to handle: just pass the right IO value before playing.

### seq_2 — `NA_BGM_TERMINA_FIELD`
IMPLEMENTED

- **Port**: IO_PORT_0 (Read)
- **Behavior**: Value 1 = normal start (load all channels). Other = alternate entry point (different arrangement, likely daytime-without-melody variant for subsequent loops).
- **Note**: Intro is Morning Jingle (handled via NB_BGM_MORNING). Ignore in audio versions.

```asm
ldio   IO_PORT_0
sub    1
rbeqz  SEQ_0013    ; IO_PORT_0 == 1 → normal start
rjump  SEQ_003D    ; else → alternate entry
```

### seq_24 — `NA_BGM_FILE_SELECT`
IMPLEMENTED

- **Port**: IO_PORT_7 (Read)
- **Behavior**: Value 1 = skip tempo intro (resume). Value 0 = play with gradual tempo ramp-up (50→120 BPM).
- **Note**: No handling requried since alternate version has it's own index: NA_BGM_FAIRY_FOUNTAIN

```asm
ldio   IO_PORT_7
sub    1
rbeqz  SEQ_004F    ; IO_PORT_7 == 1 → skip tempo intro
; else → gradual tempo: 50→52→55→56→59→60→...
```

### seq_29 — `NA_BGM_CLOCK_TOWN_MAIN_SEQUENCE`
IMPLEMENTED

Meta-sequence that launches one of three Clock Town day themes.

- **Ports**: IO_PORT_0 (Read), IO_PORT_4 (Read)
- **Behavior**:
  - IO_PORT_0 = 1: Full startup (load channels, then proceed). Other: skip to variant selection.
  - IO_PORT_4: Encodes target sequence ID as `value + 235`. So IO_PORT_4 = 21 → seq 21 (Day 1), 22 → Day 2, 23 → Day 3.
  - Dynamically patches a `runseq` command via `stseq`.
- **Note**: Ignore, each day has its own index.

```asm
ldio   IO_PORT_0
sub    1
rbeqz  SEQ_0013    ; IO_PORT_0 == 1 → full startup
rjump  SEQ_003B    ; else → skip to variant select

SEQ_003B:
ldio   IO_PORT_4
bgez   SEQ_0041    ; if >= 0, use it
ldi    0           ; else clamp to 0
SEQ_0041:
sub    235         ; subtract base offset
stseq  0, SEQ_0047+2  ; patch runseq target
runseq 255, NA_BGM_GENERAL_SFX  ; (patched at runtime)
```

## Ocarina Songs (all identical IO_PORT_7 instrument selection)

All playable ocarina songs read IO_PORT_7 at startup and self-patch the instrument index and volume via `stseq`.

**Sequences**: 50, 51, 52, 53, 71, 72, 73, 74, 81, 91, 92, 93, 94, 95

| Name | Seq |
|------|-----|
| `NA_BGM_OCARINA_EPONA` | 50 |
| `NA_BGM_OCARINA_SUNS` | 51 |
| `NA_BGM_OCARINA_TIME` | 52 |
| `NA_BGM_OCARINA_STORM` | 53 |
| `NA_BGM_OCARINA_SOARING` | 71 |
| `NA_BGM_OCARINA_HEALING` | 72 |
| `NA_BGM_INVERTED_SONG_OF_TIME` | 73 |
| `NA_BGM_SONG_OF_DOUBLE_TIME` | 74 |
| `NA_BGM_OCARINA_LULLABY_INTRO` | 81 |
| `NA_BGM_OCARINA_SONATA` | 91 |
| `NA_BGM_OCARINA_LULLABY` | 92 |
| `NA_BGM_OCARINA_NEW_WAVE` | 93 |
| `NA_BGM_OCARINA_ELEGY` | 94 |
| `NA_BGM_OCARINA_OATH` | 95 |

- **Port**: IO_PORT_7 (Read)
- **IO_PORT_7 values**:
  - Negative: Play with defaults (no instrument patching)
  - 53 (0x35): Kokiri/Deku Ocarina timbre, volume 70
  - 92 (0x5C): Zora Guitar timbre, volume 105
  - 93 (0x5D): Alternate Zora tone, volume 85
  - Other positive: Volume 85, instrument = IO_PORT_7 value
- **Note**: seq_53, seq_73, seq_93 have a slight extra branch for Zora guitar handling.
- **Additional**: All use `testlayer 0` / `testlayer 1` polling to detect when layers finish (intra-sequence sync, not game-engine IO).

```asm
ldio   IO_PORT_7
rbltz  SEQ_0033      ; negative → use defaults
stseq  0, CHAN_005F+1 ; patch instrument byte
stseq  0, CHAN_0061+1 ; patch volume byte
ldio   IO_PORT_7
sub    53
rbeqz  SEQ_0025      ; == 53 → vol 70
ldio   IO_PORT_7
sub    92
rbeqz  SEQ_002D      ; == 92 → vol 105
ldio   IO_PORT_7
sub    93
rbeqz  SEQ_0029      ; == 93 → vol 85
; default → vol 85
```

**Impact**: TODO: Implement soundtrack selection based on IO

## Interactive Music (game reads/writes IO during playback)

These sequences have ongoing bidirectional IO with the game engine during playback. Most complex to handle when replacing with streamed audio.

### seq_54 — `NA_BGM_ZORA_HALL`
BACKLOG

The most complex non-SFX IO usage in the game.

- **Ports**: IO_PORT_7 (Read, entry), IO_PORT_6 (scratch), IO_PORT_3 (Write, current section)
- **Behavior**:
  - IO_PORT_7 on entry: Section to jump to (0/negative = intro, 1 = verse, 2 = chorus, 3+ = outro loop). Written to 255 after first read (marks as "has played before").
  - IO_PORT_3 written during playback: Reports current section index (0=intro, 1=verse, 2=chorus, 3=outro) to game engine. Used to synchronize Zora Hall visual events (lighting, NPC reactions).

```asm
ldio   IO_PORT_7
stio   IO_PORT_6    ; save to scratch
ldi    255
stio   IO_PORT_7    ; mark as visited
ldio   IO_PORT_6    ; reload original
rbeqz  SEQ_0020     ; 0 → intro
rbltz  SEQ_0020     ; negative → intro
sub    1
rbeqz  SEQ_0036     ; 1 → verse
sub    1
beqz   SEQ_004C     ; 2 → chorus
jump   SEQ_0062     ; else → outro loop

SEQ_0020: ldi 0; stio IO_PORT_3; /* intro, delay 1320, end */
SEQ_0036: ldi 1; stio IO_PORT_3; /* verse, delay 6144, → chorus */
SEQ_004C: ldi 2; stio IO_PORT_3; /* chorus, delay 1536, → outro */
SEQ_0062: ldi 3; stio IO_PORT_3; /* outro, delay 1536, → loops to verse */
```

**Note**: Can be ignored for now. Prevents playing from start after leaving the musicians rooms. TODO: Low Prio, add intro skip via marker.

### seq_77 — `NA_BGM_NEW_WAVE_BOSSA_NOVA`

Indigo-Go's performance with configurable instrumentation.

- **Port**: IO_PORT_4 (Read)
- **Behavior**:
  - 255: Play all channels (full arrangement)
  - 0: Stop channels 13+14 (no sax — vocals/backing only)
  - Other nonzero: Stop channel 1 (no main melody — backing only)

```asm
ldio   IO_PORT_4
sub    255
rbeqz  SEQ_0030     ; 255 → play all
ldio   IO_PORT_4
rbeqz  SEQ_002E     ; 0 → stop ch13+14 (no sax)
stopchan 1          ; other → stop ch1 (no melody)
rjump  SEQ_0030
SEQ_002E:
stopchan 13
stopchan 14
```

**Impact**: TODO: Need 3 pre-rendered stems (full, sax-only, vocals-only) + a full mix.

### seq_83 — `NA_BGM_BREMEN_MARCH`
IMPLEMENTED

Channel 15 polls chick count and derives sync values for actor behavior.

- **Ports**: IO_PORT_0 (Write), IO_PORT_1 (Read+Write)
- **Behavior**: Every 48 tatums (35 delay + 13 cdelay), reads IO_PORT_1 (chick count written by game), does `sub 255` (effectively +1 unsigned), writes result to both IO_PORT_0 and IO_PORT_1.
- **Game reads**: `func_801A46F8` returns true when IO_PORT_0 ∈ {0, 8, 16, 24, 32}. `func_801A3950` reads and optionally clears IO_PORT_0.
- **Actors**: Dogs (`EnDg`) poll every frame until `func_801A46F8` returns true. Knights (`EnKnight`) check only at one specific frame when `timers[0] == 0`.

```asm
.channel CHAN_030C
ldi    0
stio   IO_PORT_0    ; reset to 0
stio   IO_PORT_1    ; reset to 0
CHAN_0310:
delay  35
ldio   IO_PORT_1    ; read chick count from game
sub    255          ; derive value (+1 unsigned wrap)
stio   IO_PORT_0    ; write derived value
stio   IO_PORT_1    ; write back
cdelay 13
rjump  CHAN_0310    ; loop (48-tatum cycle)
```

**Our streamed replacement**: Simplified channel 15 that writes `0x00` to IO_PORT_0 every tatum (`delay1(1)`) in a tight loop. Works because `func_801A46F8` accepts 0 as a valid value. Does NOT replicate the chick-count passthrough (IO_PORT_1 is ignored).

### seq_84 — `NA_BGM_BALLAD_OF_THE_WIND_FISH`

Milk Bar quartet with progressive instrument muting.

- **Port**: IO_PORT_4 (Read)
- **Behavior**: 4-bit bitmask where set bits mute instruments:
  - Bit 0: Mute Piano (channel 0)
  - Bit 1: Mute Drums (channel 1)
  - Bit 2: Mute Bass (channel 2)
  - Bit 3: Mute Guitar (channel 3)
  - Value 255: Play all (no muting)
  - Value 0: Skip to end / use existing assignment
- Checked at the start of each section (two sections with separate IO_PORT_4 reads).

```asm
ldio IO_PORT_4
sub  255
rbeqz SEQ_003B     ; 255 → play all
ldio IO_PORT_4; and 1; rbeqz → stopchan 0  ; bit 0 → mute piano
ldio IO_PORT_4; and 2; rbeqz → stopchan 1  ; bit 1 → mute drums
ldio IO_PORT_4; and 4; rbeqz → stopchan 2  ; bit 2 → mute bass
ldio IO_PORT_4; and 8; rbeqz → stopchan 3  ; bit 3 → mute guitar
```

**Impact**: Needs 4 separate audio stems (one per instrument) to replicate the muting behavior + complete mix.

### seq_90 — `NA_BGM_FROG_SONG`

Frog conducting minigame beat pulse.

- **Port**: IO_PORT_0 (Write)
- **Behavior**: Channel 15 alternates IO_PORT_0 between 0 and nonzero every 177 ticks (5 iterations). Used as a beat pulse signal for the game to check player input timing.

```asm
.channel CHAN_002D
ldi    0
stio   IO_PORT_0    ; initial = 0
loop   5
delay  177
sub    255          ; toggles accumulator (0 → 1, 1 → 2, etc.)
stio   IO_PORT_0    ; write toggled value
lldelay 15
loopend
end
```

**Impact**: For streamed replacement, need to emit beat pulses at the correct musical timing for the conducting minigame to work.

### seq_103 — `NA_BGM_NEW_WAVE_SAXOPHONE` (wrapper)

- Writes `IO_PORT_4 = 0`, then `runseq` seq_77. Selects sax-only variant (stops ch13+14 in seq_77).

### seq_104 — `NA_BGM_NEW_WAVE_VOCAL` (wrapper)

- Writes `IO_PORT_4 = 1`, then `runseq` seq_77. Selects vocals-only variant (stops ch1 in seq_77).

## Credits Cue Sequences

Channel 15 (silent, invalid instrument) writes IO_PORT_0 = 0 at precise timing offsets to trigger credits visual transitions.

### seq_116 — `NA_BGM_END_CREDITS` (part 1)

- **Port**: IO_PORT_0 (Write)
- **Consumer**: `CutsceneCmd_ChooseCreditsScenes` in z_demo.c reads via `func_801A3950(SEQ_PLAYER_BGM_MAIN, true)`. Triggers scene transition when value != 0xFF.
- **Tempo**: Variable (50 → 115 → 109 → 100 → 90 → 80 → 70 → 98 BPM). Final stable tempo is 98 BPM.
- **Total duration**: ~126.9s (~2:07)
- **8 cue writes**, channel 15 delays (tatums) and real-time positions:

| Cue | Delay (tatums) | Cumulative | Real time | @25 BPM delay |
|-----|---------------|------------|-----------|---------------|
| 1 | 1488 | 1488 | 20.71s | 414 |
| 2 | 2200 | 3688 | 49.02s | 566 |
| 3 | 1176 | 4864 | 64.02s | 300 |
| 4 | 1176 | 6040 | 79.02s | 300 |
| 5 | 1176 | 7216 | 94.02s | 300 |
| 6 | 1176 | 8392 | 109.02s | 300 |
| 7 | 1368 | 9760 | 126.46s | 349 |
| 8 | 32 | 9792 | 126.87s | 8 |

- After cue 8: `runseq` launches seq_127 (Credits Part 2).

### seq_127 — `NA_BGM_END_CREDITS_SECOND_HALF` (part 2)

- **Port**: IO_PORT_0 (Write)
- **Tempo**: Variable (115 → 109 → 100 → 89 → 80 → 75 → 70 → 64 → 100 → 70 → 100 BPM).
- **Total duration**: ~252.7s (~4:13)
- **12 cue writes**, channel 15 delays (tatums) and real-time positions:

| Cue | Delay (tatums) | Cumulative | Real time | @25 BPM delay |
|-----|---------------|------------|-----------|---------------|
| 1 | 1188 | 1188 | 12.91s | 258 |
| 2 | 1380 | 2568 | 27.91s | 300 |
| 3 | 1380 | 3948 | 42.91s | 300 |
| 4 | 1380 | 5328 | 57.91s | 300 |
| 5 | 1284 | 6612 | 71.87s | 279 |
| 6 | 1380 | 7992 | 86.87s | 300 |
| 7 | 1380 | 9372 | 101.87s | 300 |
| 8 | 1380 | 10752 | 116.87s | 300 |
| 9 | 1224 | 11976 | 132.32s | 309 |
| 10 | 3648 | 15624 | 178.76s | 929 |
| 11 | 1185 | 16809 | 199.28s | 411 |
| 12 | 3247 | 20056 | 252.67s | 1067 |

**Impact**: Must emit IO_PORT_0 = 0 signals at the exact same real-time offsets if replacing with streamed audio, or credits visuals will desync. The "@25 BPM delay" column gives the pre-computed delay values for use in a meta-sequence running at 25 BPM (20 tatums/sec).

## Summary: IO Port Roles Across All Sequences

| IO Port | Primary Roles |
|---------|---------------|
| **IO_PORT_0** | SFX request ID (seq_0), startup path selector (seq_2, seq_29), beat pulse (seq_90), credits cue (seq_116/127), Bremen sync (seq_83) |
| **IO_PORT_1** | SFX completion signal (seq_0), ambience sync (seq_1), Bremen chick count (seq_83) |
| **IO_PORT_2** | Volume/pan/reverb override (seq_0), ambience type selector (seq_1), launcher target (seq_122) |
| **IO_PORT_3** | Effect channel flag (seq_0), Zora Hall section report (seq_54) |
| **IO_PORT_4** | SFX index bits (seq_0), ambience channel enable (seq_1), Clock Town day (seq_29), New Wave instrument mask (seq_77), Ballad quartet mask (seq_84) |
| **IO_PORT_5** | SFX index high bits (seq_0), ambience channel enable (seq_1) |
| **IO_PORT_6** | Zora Hall scratch register (seq_54) |
| **IO_PORT_7** | Ocarina instrument type (14 sequences), File Select resume (seq_24), Zora Hall section entry (seq_54) |

## Complexity Tiers for Streamed Audio Replacement

**Tier 1 — Trivial** (startup flag only, no ongoing IO):
seq_2, seq_24, seq_29, all ocarina songs (50-95), seq_103, seq_104

**Tier 2 — Medium** (write-only IO during playback):
seq_83 (Bremen March), seq_90 (Frog Song), seq_116 (Credits pt1), seq_127 (Credits pt2)

**Tier 3 — Complex** (bidirectional IO or multi-stem):
seq_54 (Zora Hall), seq_77 (New Wave Bossa Nova), seq_84 (Ballad of Wind Fish)

**Tier 4 — Not replaceable**:
seq_0 (SFX engine), seq_1 (Ambience), seq_122 (launcher)
