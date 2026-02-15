#include <core/audio_cmd.h>
#include <recomp/modding.h>

/*
 * audio_cmd.c - Extended Audio Command System (patches thread.c / sequence.c)
 *
 * PROBLEM: MM's original seq cmd system bitpacks all args into one u32, limiting seqId to 8 bits
 *          (max 255 sequences). Global cmds waste space in their u32 fields.
 *
 * SOLUTION: Replace the seq cmd queue with RecompQueue (from queue.c) which stores op, arg0, arg1,
 *           and a pointer-width field (asInt) per entry. Extended ops (0x10+) use asInt for seqId,
 *           breaking the 255 limit. Original ops (<0x10) still work via fallthrough to vanilla code.
 *
 * ARCHITECTURE:
 *   Game thread -> AudioSeq_QueueSeqCmd / AudioApi_QueueExtendedSeqCmd -> sAudioSeqCmdQueue
 *   Audio thread -> AudioSeq_ProcessSeqCmds -> drains queue -> AudioApi_ProcessSeqCmd per entry
 *   Global cmds -> AudioApi_ProcessGlobalCmd (RECOMP_HOOK on AudioThread_ProcessGlobalCmd)
 *
 * CMD LAYOUT (RecompQueueCmd):
 *   op:    4-bit operation type (SEQCMD_OP_* or SEQCMD_EXTENDED_OP_*)
 *   arg0:  original bitpacked u32 (seqPlayerIndex in bits 24-27, fadeTimer in 16-23, etc.)
 *   arg1:  extra arg (used by extended setup cmds for subOp)
 *   asInt: full-width seqId for extended cmds, or pointer for setup play cmds
 *
 * KEY DATA STRUCTURES (external):
 *   gActiveSeqs[]     - vanilla per-seqPlayer state (fade, volume, font, setupFadeTimer, etc.)
 *   gExtActiveSeqs[]  - extended per-seqPlayer state (seqId, prevSeqId, setupCmd[], startAsyncSeqCmd)
 *   sExtSeqRequests[][] - priority queue of pending sequences per seqPlayer
 *   sNumSeqRequests[]   - count of entries in sExtSeqRequests per seqPlayer
 */

extern void AudioSeq_ProcessSeqCmd(u32 cmd);
extern void AudioThread_SetFadeInTimer(s32 seqPlayerIndex, s32 fadeTimer);
extern AudioTable* AudioLoad_GetLoadTable(s32 tableType);

RecompQueue* sAudioSeqCmdQueue;

RECOMP_CALLBACK(".", AudioApi_InitInternal) void AudioApi_AudioCmdInit() {
    sAudioSeqCmdQueue = RecompQueue_Create();
}

/* HOOK: Intercepts global audio thread cmds for extended ops (0x86-0xEA).
 * These run on the audio thread and handle low-level seq loading/init.
 * cmd->arg0 = seqPlayerIndex, cmd->asInt = seqId, cmd->opArgs & 0xFFFF = timer/flags */
RECOMP_HOOK("AudioThread_ProcessGlobalCmd") void AudioApi_ProcessGlobalCmd(AudioCmd* cmd) {
    switch (cmd->op) {
    case AUDIOCMD_EXTENDED_OP_GLOBAL_SYNC_LOAD_SEQ_PARTS: // 0x86: sync load seq + fonts
        AudioLoad_SyncLoadSeqParts(cmd->asInt, cmd->arg0, cmd->opArgs & 0xFFFF, &gAudioCtx.externalLoadQueue);
        break;

    case AUDIOCMD_EXTENDED_OP_GLOBAL_INIT_SEQPLAYER: // 0x87: init seqPlayer + fade in
        AudioLoad_SyncInitSeqPlayer(cmd->arg0, cmd->asInt, 0);
        AudioThread_SetFadeInTimer(cmd->arg0, cmd->opArgs & 0xFFFF);
        break;

    case AUDIOCMD_EXTENDED_OP_GLOBAL_INIT_SEQPLAYER_SKIP_TICKS: { // 0x88: init + skip ahead in sequence
        // Mod seqs (in KSEG0/mod memory) have minimal scripts, can't skip ticks safely
        uintptr_t seqRomAddr = AudioLoad_GetLoadTable(SEQUENCE_TABLE)->entries[cmd->asInt].romAddr;
        if (IS_KSEG0(seqRomAddr)) {
            AudioLoad_SyncInitSeqPlayer(cmd->arg0, cmd->asInt, 0);
            AudioThread_SetFadeInTimer(cmd->arg0, 500);
        } else {
            AudioLoad_SyncInitSeqPlayerSkipTicks(cmd->arg0, cmd->asInt, cmd->opArgs & 0xFFFF);
            AudioThread_SetFadeInTimer(cmd->arg0, 500);
            AudioScript_SkipForwardSequence(&gAudioCtx.seqPlayers[cmd->arg0]);
        }
        break;
    }

    case AUDIOCMD_EXTENDED_OP_GLOBAL_DISCARD_SEQ_FONTS: // 0xF7: free font data for seq
        AudioLoad_DiscardSeqFonts(cmd->asInt);
        break;

    case AUDIOCMD_EXTENDED_OP_GLOBAL_ASYNC_LOAD_SEQ: // 0xEA: async load seq data
        AudioLoad_AsyncLoadSeq(cmd->asInt, cmd->arg0, cmd->opArgs & 0xFFFF, &gAudioCtx.externalLoadQueue);
        break;
    }
}

/* EXPORT: Queue an extended seq cmd. Stores op in top 4 bits of cmd + separately as queue op.
 * arg1 used for setup subOp. seqId stored in pointer-width field via RecompQueue_Push. */
RECOMP_EXPORT void AudioApi_QueueExtendedSeqCmd(u32 op, u32 cmd, u32 arg1, s32 seqId) {
    cmd |= (op & 0xF) << 28;
    RecompQueue_Push(sAudioSeqCmdQueue, op, cmd, arg1, (void**)&seqId);
}

/* PATCH: Replaces vanilla AudioSeq_QueueSeqCmd. Redirects all seq cmds into our RecompQueue.
 * Original ops (<0x10) get op from top 4 bits; no extended fields needed. */
RECOMP_PATCH void AudioSeq_QueueSeqCmd(u32 cmd) {
    u8 op = cmd >> 28;
    RecompQueue_Push(sAudioSeqCmdQueue, op, cmd, 0, 0);
}

/* PATCH: Replaces vanilla AudioSeq_ProcessSeqCmds. Drains entire queue each audio frame. */
RECOMP_PATCH void AudioSeq_ProcessSeqCmds(void) {
    RecompQueue_Drain(sAudioSeqCmdQueue, AudioApi_ProcessSeqCmd);
}

/* Main seq cmd dispatcher. Handles both vanilla ops (0x0-0xF) and extended ops (0x10+).
 * Extended ops read seqId from cmd->asInt; vanilla ops read from lower bits of cmd->arg0.
 * Unrecognized ops fall through to original AudioSeq_ProcessSeqCmd(cmd->arg0). */
void AudioApi_ProcessSeqCmd(RecompQueueCmd* cmd) {
    s32 priority;
    u16 fadeTimer;
    u8 subOp;
    u8 seqPlayerIndex;
    s32 seqId;
    u8 seqArgs;
    u8 found;
    u8 i;
    u32 outNumFonts;

    seqPlayerIndex = (cmd->arg0 & SEQCMD_SEQPLAYER_MASK) >> 24;

    switch (cmd->op) {
    case SEQCMD_OP_PLAY_SEQUENCE:
    case SEQCMD_EXTENDED_OP_PLAY_SEQUENCE:
        /* PLAY: Start a sequence on seqPlayerIndex.
         * seqArgs<0x80 = sync (immediate play). seqArgs>=0x80 = async (load fonts first, then replay).
         * fadeTimer is pre-scaled >>13 (not >>16) because AudioSeq_StartSequence scales further.
         * Async path: saves cmd to startAsyncSeqCmd, stops current seq, requests font load.
         * Font load completion triggers replay with seqArgs<0x80 (SEQCMD_ASYNC_ACTIVE set). */
        seqId = cmd->op == SEQCMD_EXTENDED_OP_PLAY_SEQUENCE ? cmd->asInt : (cmd->arg0 & SEQCMD_SEQID_MASK);
        seqArgs = (cmd->arg0 & 0xFF00) >> 8;
        fadeTimer = (cmd->arg0 & 0xFF0000) >> 13;
        if (!gActiveSeqs[seqPlayerIndex].isWaitingForFonts && !sStartSeqDisabled) {
            if (seqArgs < 0x80) {
                AudioApi_StartSequence(seqPlayerIndex, seqId, seqArgs, fadeTimer);
            } else {
                cmd->arg0 = (cmd->arg0 & ~(SEQ_FLAG_ASYNC | SEQCMD_ASYNC_ACTIVE)) + SEQCMD_ASYNC_ACTIVE;
                gExtActiveSeqs[seqPlayerIndex].startAsyncSeqCmd = *cmd;

                gActiveSeqs[seqPlayerIndex].isWaitingForFonts = true;
                gActiveSeqs[seqPlayerIndex].fontId = *AudioThread_GetFontsForSequence(seqId, &outNumFonts);
                AudioSeq_StopSequence(seqPlayerIndex, 1);

                if (gExtActiveSeqs[seqPlayerIndex].prevSeqId != NA_BGM_DISABLED) {
                    if (*AudioThread_GetFontsForSequence(seqId, &outNumFonts) !=
                        *AudioThread_GetFontsForSequence(gExtActiveSeqs[seqPlayerIndex].prevSeqId, &outNumFonts)) {
                        AUDIOCMD_EXTENDED_GLOBAL_DISCARD_SEQ_FONTS(seqId);
                    }
                }

                AUDIOCMD_GLOBAL_ASYNC_LOAD_FONT(*AudioThread_GetFontsForSequence(seqId, &outNumFonts),
                                                (u8)((seqPlayerIndex + 1) & 0xFF));
            }
        }
        break;

    case SEQCMD_OP_QUEUE_SEQUENCE:
    case SEQCMD_EXTENDED_OP_QUEUE_SEQUENCE:
        /* QUEUE: Insert seq into sExtSeqRequests[] sorted by priority (descending).
         * If already queued and first -> play immediately. If new and inserted at index 0 -> play.
         * If queue full, lowest-priority entry is evicted. priority = seqArgs byte. */
        seqId = cmd->op == SEQCMD_EXTENDED_OP_PLAY_SEQUENCE ? cmd->asInt : (cmd->arg0 & SEQCMD_SEQID_MASK);
        seqArgs = (cmd->arg0 & 0xFF00) >> 8;
        fadeTimer = (cmd->arg0 & 0xFF0000) >> 13;
        priority = seqArgs;

        for (i = 0; i < sNumSeqRequests[seqPlayerIndex]; i++) {
            if (sExtSeqRequests[seqPlayerIndex][i].seqId == seqId) {
                if (i == 0) {
                    AudioApi_StartSequence(seqPlayerIndex, seqId, seqArgs, fadeTimer);
                }
                return;
            }
        }

        found = sNumSeqRequests[seqPlayerIndex];
        for (i = 0; i < sNumSeqRequests[seqPlayerIndex]; i++) {
            if (priority >= sExtSeqRequests[seqPlayerIndex][i].priority) {
                found = i;
                break;
            }
        }

        if (sNumSeqRequests[seqPlayerIndex] < ARRAY_COUNT(sExtSeqRequests[seqPlayerIndex])) {
            sNumSeqRequests[seqPlayerIndex]++;
        }

        for (i = sNumSeqRequests[seqPlayerIndex] - 1; i != found; i--) {
            sExtSeqRequests[seqPlayerIndex][i].priority = sExtSeqRequests[seqPlayerIndex][i - 1].priority;
            sExtSeqRequests[seqPlayerIndex][i].seqId = sExtSeqRequests[seqPlayerIndex][i - 1].seqId;
        }

        sExtSeqRequests[seqPlayerIndex][found].priority = seqArgs;
        sExtSeqRequests[seqPlayerIndex][found].seqId = seqId;

        if (found == 0) {
            AudioApi_StartSequence(seqPlayerIndex, seqId, seqArgs, fadeTimer);
        }
        break;

    case SEQCMD_OP_UNQUEUE_SEQUENCE:
    case SEQCMD_EXTENDED_OP_UNQUEUE_SEQUENCE:
        /* UNQUEUE: Remove seq from sExtSeqRequests[], shift remaining entries forward.
         * If removed seq was at index 0 (currently playing), stop it and start next in queue. */
        seqId = cmd->op == SEQCMD_EXTENDED_OP_PLAY_SEQUENCE ? cmd->asInt : (cmd->arg0 & SEQCMD_SEQID_MASK);
        fadeTimer = (cmd->arg0 & 0xFF0000) >> 13;

        found = sNumSeqRequests[seqPlayerIndex];
        for (i = 0; i < sNumSeqRequests[seqPlayerIndex]; i++) {
            if (sExtSeqRequests[seqPlayerIndex][i].seqId == seqId) {
                found = i;
                break;
            }
        }

        if (found != sNumSeqRequests[seqPlayerIndex]) {
            for (i = found; i < sNumSeqRequests[seqPlayerIndex] - 1; i++) {
                sExtSeqRequests[seqPlayerIndex][i].priority = sExtSeqRequests[seqPlayerIndex][i + 1].priority;
                sExtSeqRequests[seqPlayerIndex][i].seqId = sExtSeqRequests[seqPlayerIndex][i + 1].seqId;
            }
            sNumSeqRequests[seqPlayerIndex]--;
        }

        if (found == 0) {
            AudioSeq_StopSequence(seqPlayerIndex, fadeTimer);
            if (sNumSeqRequests[seqPlayerIndex] != 0) {
                AudioApi_StartSequence(seqPlayerIndex, sExtSeqRequests[seqPlayerIndex][0].seqId,
                                       sExtSeqRequests[seqPlayerIndex][0].priority, fadeTimer);
            }
        }
        break;

    case SEQCMD_OP_SETUP_CMD:
    case SEQCMD_EXTENDED_OP_SETUP_CMD:
        /* SETUP: Queue a sub-command into gExtActiveSeqs[].setupCmd[] to run when the seqPlayer
         * becomes disabled (sequence ends). Extended ops store subOp in arg1, seqId in asInt.
         * setupCmdTimer=2 delays execution by 2 frames so a newly-started seq is enabled before
         * the setup system checks player state. PLAY_SEQ subOp stores seqId in the cmd's asInt.
         * RESET_SETUP_CMDS clears all pending setup cmds for this seqPlayer. */
        subOp = cmd->op == SEQCMD_EXTENDED_OP_SETUP_CMD ? cmd->arg1 : ((cmd->arg0 & 0xF00000) >> 20);
        seqId = cmd->op == SEQCMD_EXTENDED_OP_SETUP_CMD ? cmd->asInt : (cmd->arg0 & SEQCMD_SEQID_MASK);

        if (subOp != SEQCMD_SUB_OP_SETUP_RESET_SETUP_CMDS) {
            found = gExtActiveSeqs[seqPlayerIndex].setupCmdNum++;
            if (found < ARRAY_COUNT(gExtActiveSeqs[seqPlayerIndex].setupCmd)) {
                if (subOp == SEQCMD_SUB_OP_SETUP_PLAY_SEQ) {
                    gExtActiveSeqs[seqPlayerIndex].setupCmd[found] =
                        (RecompQueueCmd){ subOp, cmd->arg0, seqPlayerIndex, (void**)(intptr_t)seqId };
                } else {
                    gExtActiveSeqs[seqPlayerIndex].setupCmd[found] =
                        (RecompQueueCmd){ subOp, cmd->arg0, seqPlayerIndex, 0 };
                }
                gExtActiveSeqs[seqPlayerIndex].setupCmdTimer = 2;
            }
        } else {
            gExtActiveSeqs[seqPlayerIndex].setupCmdNum = 0;
        }
        break;


    default:
        AudioSeq_ProcessSeqCmd(cmd->arg0); // vanilla fallthrough for ops 0x1-0xF
        break;
    }
}

/* Process setup sub-commands (deferred cmds that run when a seqPlayer finishes/disables).
 * Called from the seq update loop for each pending setupCmd in gExtActiveSeqs[].setupCmd[].
 * setupCmd->arg1 = seqPlayerIndex that owns the setup, arg0 encodes targetSeqPlayer + values.
 * Bit layout of arg0: targetSeqPlayerIndex [19:16], setupVal2 [15:8], setupVal1 [7:0] */
void AudioApi_ProcessSeqSetupCmd(RecompQueueCmd* setupCmd) {
    u8 seqPlayerIndex;
    u8 targetSeqPlayerIndex;
    u8 setupVal2;
    u8 setupVal1;
    s32 seqId;
    u16 channelMask;

    seqPlayerIndex = setupCmd->arg1;
    targetSeqPlayerIndex = (setupCmd->arg0 & 0xF0000) >> 16;
    setupVal2 = (setupCmd->arg0 & 0xFF00) >> 8;
    setupVal1 = setupCmd->arg0 & 0xFF;

    switch (setupCmd->op) {
    case SEQCMD_SUB_OP_SETUP_RESTORE_SEQPLAYER_VOLUME:
        AudioSeq_SetVolumeScale(targetSeqPlayerIndex, VOL_SCALE_INDEX_FANFARE, 0x7F, setupVal1);
        break;

    case SEQCMD_SUB_OP_SETUP_RESTORE_SEQPLAYER_VOLUME_IF_QUEUED:
        // Only restore if queue count matches expected value (guards against stale restores)
        if (setupVal1 == sNumSeqRequests[seqPlayerIndex]) {
            AudioSeq_SetVolumeScale(targetSeqPlayerIndex, VOL_SCALE_INDEX_FANFARE, 0x7F, setupVal2);
        }
        break;

    case SEQCMD_SUB_OP_SETUP_SEQ_UNQUEUE:
        // @bug vanilla: used gActiveSeqs[].seqId which is already reset by this point.
        // @mod fix: reads from gExtActiveSeqs[].seqId which persists across stop.
        SEQCMD_EXTENDED_UNQUEUE_SEQUENCE((u8)(seqPlayerIndex + (SEQCMD_ASYNC_ACTIVE >> 24)), 0,
                                         gExtActiveSeqs[seqPlayerIndex].seqId);
        break;

    case SEQCMD_SUB_OP_SETUP_RESTART_SEQ:
        // Restart current seq on target with full volume. @mod reads seqId from gExtActiveSeqs.
        SEQCMD_EXTENDED_PLAY_SEQUENCE((u8)(targetSeqPlayerIndex + (SEQCMD_ASYNC_ACTIVE >> 24)),
                                      1, 0, gExtActiveSeqs[targetSeqPlayerIndex].seqId);
        gActiveSeqs[targetSeqPlayerIndex].fadeVolUpdate = true;
        gActiveSeqs[targetSeqPlayerIndex].volScales[1] = 0x7F;
        break;

    case SEQCMD_SUB_OP_SETUP_TEMPO_SCALE:
        SEQCMD_SCALE_TEMPO((u8)(targetSeqPlayerIndex + (SEQCMD_ASYNC_ACTIVE >> 24)), setupVal2, setupVal1);
        break;

    case SEQCMD_SUB_OP_SETUP_TEMPO_RESET:
        SEQCMD_RESET_TEMPO((u8)(targetSeqPlayerIndex + (SEQCMD_ASYNC_ACTIVE >> 24)), setupVal1);
        break;

    case SEQCMD_SUB_OP_SETUP_PLAY_SEQ:
        // Play seq using fade timer from prior SET_FADE_TIMER cmd. @mod reads seqId from asInt.
        seqId = setupCmd->asInt;
        SEQCMD_EXTENDED_PLAY_SEQUENCE((u8)(targetSeqPlayerIndex + (SEQCMD_ASYNC_ACTIVE >> 24)),
                                      gActiveSeqs[targetSeqPlayerIndex].setupFadeTimer,
                                      setupVal2 << 8, seqId);
        AudioSeq_SetVolumeScale(targetSeqPlayerIndex, VOL_SCALE_INDEX_FANFARE, 0x7F, 0);
        gActiveSeqs[targetSeqPlayerIndex].setupFadeTimer = 0;
        break;

    case SEQCMD_SUB_OP_SETUP_SET_FADE_TIMER:
        // Stores fade timer for a subsequent SETUP_PLAY_SEQ on this seqPlayer
        gActiveSeqs[seqPlayerIndex].setupFadeTimer = setupVal2;
        break;

    case SEQCMD_SUB_OP_SETUP_RESTORE_SEQPLAYER_VOLUME_WITH_SCALE_INDEX:
        // Like RESTORE_VOLUME but with configurable scaleIndex (setupVal2)
        AudioSeq_SetVolumeScale(targetSeqPlayerIndex, setupVal2, 0x7F, setupVal1);
        break;

    case SEQCMD_SUB_OP_SETUP_POP_PERSISTENT_CACHE:
        // Pop audio heap caches. setupVal1 is bitmask: bit0=SEQ, bit1=FONT, bit2=SAMPLE
        if (setupVal1 & (1 << SEQUENCE_TABLE)) {
            AUDIOCMD_GLOBAL_POP_PERSISTENT_CACHE(SEQUENCE_TABLE);
        }
        if (setupVal1 & (1 << FONT_TABLE)) {
            AUDIOCMD_GLOBAL_POP_PERSISTENT_CACHE(FONT_TABLE);
        }
        if (setupVal1 & (1 << SAMPLE_TABLE)) {
            AUDIOCMD_GLOBAL_POP_PERSISTENT_CACHE(SAMPLE_TABLE);
        }
        break;

    case SEQCMD_SUB_OP_SETUP_SET_CHANNEL_DISABLE_MASK:
        // @mod channel mask built from lower 16 bits of arg0 (vanilla used split bytes)
        channelMask = setupCmd->arg0 & 0xFFFF;
        SEQCMD_SET_CHANNEL_DISABLE_MASK((u8)(targetSeqPlayerIndex + (SEQCMD_ASYNC_ACTIVE >> 24)),
                                        channelMask);
        break;

    case SEQCMD_SUB_OP_SETUP_SET_SEQPLAYER_FREQ:
        // Scale freq on all channels. setupVal1*10 = duration in frames
        SEQCMD_SET_SEQPLAYER_FREQ((u8)(targetSeqPlayerIndex + (SEQCMD_ASYNC_ACTIVE >> 24)),
                                  setupVal2, setupVal1 * 10);
        break;

    default:
        break;
    }
}

/* PATCH: Check if any pending seq cmd matches (arg0 & cmdMask) == cmdVal.
 * Returns true if NO match found (i.e., the cmd is NOT queued). Used by game code
 * to avoid duplicate commands (e.g., preventing double-play of same sequence). */
RECOMP_PATCH s32 AudioSeq_IsSeqCmdNotQueued(u32 cmdVal, u32 cmdMask) {
    for (s32 i = 0; i < sAudioSeqCmdQueue->numEntries; i++) {
        RecompQueueCmd* cmd = &sAudioSeqCmdQueue->entries[i];
        if ((cmd->arg0 & cmdMask) == cmdVal) {
            return false;
        }
    }
    return true;
}
