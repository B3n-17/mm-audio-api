/*
 * resource.c — Porcelain (mod-facing) API for loading audio resources from the filesystem.
 *
 * This is the public mod API layer. Mods call these RECOMP_EXPORT functions to register
 * audio assets (sequences, soundfonts, sample banks, decoded audio files) from loose files
 * on disk. Each function accepts an optional info struct for config (NULL = use defaults)
 * and delegates to a RECOMP_IMPORT native implementation that does the actual loading.
 *
 * Resource type hierarchy (all typedefs of AudioApiResourceInfo unless noted):
 *   AudioApiResourceInfo   — base: { resourceId, filesize, cacheStrategy }
 *   AudioApiSequenceInfo   — alias of ResourceInfo (binary .seq data)
 *   AudioApiSoundFontInfo  — alias of ResourceInfo (instrument bank metadata)
 *   AudioApiSampleBankInfo — alias of ResourceInfo (raw sample data), uses separate native loader
 *   AudioApiFileInfo       — distinct struct: adds codec, sampleRate, loopStart/End, channelType etc.
 *
 * Loading paths:
 *   Sequence/SoundFont → AddResourceFromFs → AudioApiNative_AddResource  (generic resource loader)
 *   SampleBank         → AddSampleBankFromFs → AudioApiNative_AddSampleBank (sample-specific loader)
 *   AudioFile          → AddAudioFileFromFs → AudioApiNative_AddAudioFile (decoded audio loader)
 *
 * GetResourceDevAddr: Returns a virtual "device address" for a loaded resource by registering
 *   the built-in NativeDmaCallback as the DMA handler. The returned uintptr_t is used by the
 *   audio engine to locate resource data during playback via DMA callbacks.
 */
#include <global.h>
#include <recomp/modding.h>

#include <audio_api/types.h>

/* Native implementations imported from the host recomp runtime (".") */
RECOMP_IMPORT(".", bool AudioApiNative_AddResource(AudioApiResourceInfo* info, char* dir, char* filename));
RECOMP_IMPORT(".", bool AudioApiNative_AddSampleBank(AudioApiSampleBankInfo* info, char* dir, char* filename));
RECOMP_IMPORT(".", bool AudioApiNative_AddAudioFile(AudioApiFileInfo* info, char* dir, char* filename));
RECOMP_IMPORT(".", uintptr_t AudioApi_AddDmaCallback(AudioApiDmaCallback callback, u32 arg0, u32 arg1, u32 arg2));
RECOMP_IMPORT(".", s32 AudioApi_NativeDmaCallback(void* ramAddr, size_t size, size_t offset, u32 arg0, u32 arg1, u32 arg2));

/* Generic resource loader. Sequences and soundfonts both route here. info=NULL → zero-init defaults. */
RECOMP_EXPORT bool AudioApi_AddResourceFromFs(AudioApiResourceInfo* info, char* dir, char* filename) {
    AudioApiResourceInfo defaultInfo = {0};

    if (info == NULL) {
        info = &defaultInfo;
    }

    return AudioApiNative_AddResource(info, dir, filename);
}

/* Thin wrappers — Sequence/SoundFont are identical to generic resource (same struct layout). */
RECOMP_EXPORT bool AudioApi_AddSequenceFromFs(AudioApiSequenceInfo* info, char* dir, char* filename) {
    return AudioApi_AddResourceFromFs((AudioApiResourceInfo*)info, dir, filename);
}

RECOMP_EXPORT bool AudioApi_AddSoundFontFromFs(AudioApiSoundFontInfo* info, char* dir, char* filename) {
    return AudioApi_AddResourceFromFs((AudioApiResourceInfo*)info, dir, filename);
}

/* Sample bank uses a separate native loader (may handle raw PCM data differently). */
RECOMP_EXPORT bool AudioApi_AddSampleBankFromFs(AudioApiSampleBankInfo* info, char* dir, char* filename) {
    AudioApiSampleBankInfo defaultInfo = {0};

    if (info == NULL) {
        info = &defaultInfo;
    }

    return AudioApiNative_AddSampleBank(info, dir, filename);
}

/* Decoded audio file (wav/flac/mp3/vorbis/opus). Distinct info struct with codec/sample params. */
RECOMP_EXPORT bool AudioApi_AddAudioFileFromFs(AudioApiFileInfo* info, char* dir, char* filename) {
    AudioApiFileInfo defaultInfo = {0};

    if (info == NULL) {
        info = &defaultInfo;
    }

    return AudioApiNative_AddAudioFile(info, dir, filename);
}

/* Returns a device address handle for a resource by binding NativeDmaCallback as its DMA source.
 * The audio engine uses this address to stream resource data during playback. */
RECOMP_EXPORT uintptr_t AudioApi_GetResourceDevAddr(u32 resourceId, u32 arg1, u32 arg2) {
    return AudioApi_AddDmaCallback(AudioApi_NativeDmaCallback, resourceId, arg1, arg2);
}
