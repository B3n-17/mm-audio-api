/*
 * effects.c — Rescales audio effect parameters from the game's native 32kHz to 48kHz output.
 *
 * Context: MM Recomp mod. The N64 audio engine uses RSP microcode (Acmd) to process audio in
 * a small data memory (DMEM) scratchpad. Original game targets 32kHz; this mod upsamples to
 * 48kHz by multiplying sample-count-based parameters by FREQ_FACTOR (1.5, defined in init.h).
 *
 * Two systems are patched:
 *   1. AudioApi_EffectsInit — one-time rescale of Haas delay table + all reverb settings at init.
 *   2. AudioApi_ApplyCombFilter — runtime comb filter replacement with 48kHz-correct DMEM layout.
 *
 * DMEM layout (RSP scratch addresses):
 *   DMEM_TEMP      (0x3B0) — primary working buffer for current note samples
 *   DMEM_COMB_TEMP (0x750) — temporary buffer for comb filter processing
 *   combFilterDmem = DMEM_COMB_TEMP - combFilterSize  — holds previous frame's tail samples
 */

#include <core/effects.h>
#include <recomp/modding.h>
#include <core/init.h>

#define DMEM_TEMP 0x3B0
#define DMEM_COMB_TEMP 0x750

/* Vanilla Haas effect delay table (64 entries, in samples). Scaled once at init. */
extern u16 gHaasEffectDelaySize[64];

/* Vanilla per-scene reverb configurations (0x0–0xF). Each index holds 2-3 ReverbSettings
 * structs whose delayNumSamples/subDelay fields are in samples and need 48kHz scaling. */
extern ReverbSettings reverbSettings0[3];
extern ReverbSettings reverbSettings1[3];
extern ReverbSettings reverbSettings2[3];
extern ReverbSettings reverbSettings3[3];
extern ReverbSettings reverbSettings4[3];
extern ReverbSettings reverbSettings5[3];
extern ReverbSettings reverbSettings6[3];
extern ReverbSettings reverbSettings7[3];
extern ReverbSettings reverbSettings8[2];
extern ReverbSettings reverbSettings9[3];
extern ReverbSettings reverbSettingsA[3];
extern ReverbSettings reverbSettingsB[3];
extern ReverbSettings reverbSettingsC[3];
extern ReverbSettings reverbSettingsD[3];
extern ReverbSettings reverbSettingsE[3];
extern ReverbSettings reverbSettingsF[2];

/* Lookup table: scene reverb index -> pointer to its ReverbSettings array */
ReverbSettings* gReverbSettingsTableFull[] = {
    reverbSettings0, reverbSettings1, reverbSettings2, reverbSettings3,
    reverbSettings4, reverbSettings5, reverbSettings6, reverbSettings7,
    reverbSettings8, reverbSettings9, reverbSettingsA, reverbSettingsB,
    reverbSettingsC, reverbSettingsD, reverbSettingsE, reverbSettingsF,
};

/* Number of ReverbSettings entries per scene reverb index (parallel to table above) */
u8 gReverbSettingsTableCount[] = {
    3, 3, 3, 3, 3, 3, 3, 3, 2, 3, 3, 3, 3, 3, 3, 2,
};

/* RSP Acmd helpers — build audio microcode commands for DMEM operations */
void AudioSynth_DMemMove(Acmd* cmd, s32 dmemIn, s32 dmemOut, size_t size);
void AudioSynth_LoadBuffer(Acmd* cmd, s32 dmemDest, s32 size, void* addrSrc);
void AudioSynth_ClearBuffer(Acmd* cmd, s32 dmem, s32 size);
void AudioSynth_SaveBuffer(Acmd* cmd, s32 dmemSrc, s32 size, void* addrDest);
void AudioSynth_Mix(Acmd* cmd, size_t size, s32 gain, s32 dmemIn, s32 dmemOut);

/*
 * One-time init callback (runs after AudioApi_InitInternal):
 * Scales all sample-count parameters by FREQ_FACTOR so delays/reverbs sound correct at 48kHz.
 */
RECOMP_CALLBACK(".", AudioApi_InitInternal) void AudioApi_EffectsInit() {
    s32 i, j;

    for (i = 0; i < ARRAY_COUNT(gHaasEffectDelaySize); i++) {
        gHaasEffectDelaySize[i] *= FREQ_FACTOR;
    }

    for (i = 0; i < ARRAY_COUNT(gReverbSettingsTableFull); i++) {
        for (j = 0; j < gReverbSettingsTableCount[i]; j++) {
            ReverbSettings* settings = &gReverbSettingsTableFull[i][j];
            settings->delayNumSamples *= FREQ_FACTOR;
            settings->subDelay *= FREQ_FACTOR;
        }
    }
}

/*
 * Replacement comb filter that handles 48kHz-scaled sizes with DMEM alignment fixup.
 *
 * Problem: combFilterSize * FREQ_FACTOR may not be 16-byte aligned (RSP requires it).
 * Solution: ALIGN16 the size for buffer ops, track the alignment remainder (combFilterAlign),
 * and offset the mix destination by combFilterAlign so the mix lands at the correct sample.
 *
 * Per-frame flow (when filter is active):
 *   1. Copy current samples: DMEM_TEMP -> DMEM_COMB_TEMP
 *   2. Load previous frame's tail (combFilterSize bytes) into combFilterDmem
 *      (or clear on first frame)
 *   3. Save current frame's tail to DRAM for next iteration
 *   4. Mix: blend DMEM_COMB_TEMP into (combFilterDmem + combFilterAlign) with gain
 *   5. Move result back to DMEM_TEMP
 *
 * Returns advanced cmd pointer (caller chains further Acmds after this).
 */
Acmd* AudioApi_ApplyCombFilter(Acmd* cmd, NoteSampleState* sampleState, NoteSynthesisState* synthState,
                               s32 numSamplesPerUpdate) {
    u16 combFilterSize = ALIGN16((u16)(sampleState->combFilterSize * FREQ_FACTOR));
    u16 combFilterAlign = (u16)(sampleState->combFilterSize * FREQ_FACTOR) & 0xF;
    u16 combFilterGain = sampleState->combFilterGain;
    void* combFilterState = synthState->synthesisBuffers->combFilterState;
    s32 combFilterDmem;

    if ((combFilterSize != 0) && (sampleState->combFilterGain != 0)) {
        /* Step 1: snapshot current samples into comb temp region */
        AudioSynth_DMemMove(cmd++, DMEM_TEMP, DMEM_COMB_TEMP, numSamplesPerUpdate * SAMPLE_SIZE);
        combFilterDmem = DMEM_COMB_TEMP - combFilterSize;
        if (synthState->combFilterNeedsInit) {
            /* First frame: zero the delay buffer */
            AudioSynth_ClearBuffer(cmd++, combFilterDmem, combFilterSize);
            synthState->combFilterNeedsInit = false;
        } else {
            /* Step 2: load previous frame's tail samples from DRAM */
            AudioSynth_LoadBuffer(cmd++, combFilterDmem, combFilterSize, combFilterState);
        }
        /* Step 3: persist current frame's tail to DRAM for next iteration */
        AudioSynth_SaveBuffer(cmd++, DMEM_TEMP + (numSamplesPerUpdate * SAMPLE_SIZE) - combFilterSize,
                              combFilterSize, combFilterState);
        /* Step 4: mix — offset by combFilterAlign to correct for ALIGN16 padding */
        AudioSynth_Mix(cmd++, (numSamplesPerUpdate * (s32)SAMPLE_SIZE) >> 4, combFilterGain, DMEM_COMB_TEMP,
                       combFilterDmem + combFilterAlign);
        /* Step 5: move processed result back to primary working buffer */
        AudioSynth_DMemMove(cmd++, combFilterDmem + combFilterAlign, DMEM_TEMP, numSamplesPerUpdate * SAMPLE_SIZE);
    } else {
        /* Filter inactive — mark for re-init when it becomes active again */
        synthState->combFilterNeedsInit = true;
    }

    return cmd;
}
