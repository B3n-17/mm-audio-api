#include <extlib/main.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#include <plog/Log.h>
#include <plog/Init.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Initializers/ConsoleInitializer.h>

#include <audio_api/types.h>

#include <extlib/lib_recomp.hpp>
#include <extlib/debug.hpp>
#include <extlib/resource/abstract.hpp>
#include <extlib/resource/audiofile.hpp>
#include <extlib/resource/generic.hpp>
#include <extlib/resource/samplebank.hpp>
#include <extlib/thread.hpp>

extern "C" {
    DLLEXPORT uint32_t recomp_api_version = RECOMP_API_VERSION;
}

namespace fs = std::filesystem;

static bool sIsInitialized = false;
static size_t sResourceCount = 0;

Vfs::Filesystem gVfs;
std::unordered_map<size_t, std::shared_ptr<Resource::Abstract>> gResourceData;
std::shared_mutex gResourceDataMutex;

static plog::ConsoleAppender<plog::TxtFormatter> sConsoleAppender;

RECOMP_DLL_FUNC(AudioApiNative_Init) {
    auto logLevel = RECOMP_ARG(uint32_t, 0);
    auto rootDirStr = RECOMP_ARG_U8STR(1);

    try {
        if (sIsInitialized) {
            throw std::runtime_error("Extlib already initialized");
        }

        plog::init((plog::Severity)logLevel, &sConsoleAppender);

        {
            auto rootDir = fs::canonical(fs::path(rootDirStr).parent_path());
            auto defaultDir = rootDir / "mod_data" / "audio";
            PLOG_INFO << "Root Dir: " << rootDir;
            PLOG_INFO << "Default Dir: " << defaultDir;

            gVfs.setDefaultDir(defaultDir);
            gVfs.addAllowedDir(rootDir / "mod_data");
            gVfs.addAllowedDir(rootDir / "mods");

            gVfs.addKnownZipExtension(".zip");
            gVfs.addKnownZipExtension(".nrm");
            gVfs.addKnownZipExtension(".mmrs");
        }

        {
            std::thread workerThread(workerThreadLoop);
            workerThread.detach();
        }

        sIsInitialized = true;
        RECOMP_RETURN(bool, true);

    } catch (const fs::filesystem_error& e) {
        PLOG_ERROR << "Init error: " << e.what();
    } catch (const std::invalid_argument& e) {
        PLOG_ERROR << "Init error: " << e.what();
    } catch (const std::runtime_error& e) {
        PLOG_ERROR << "Init error: " << e.what();
    } catch (...) {
        PLOG_ERROR << "Init error: Unknown error";
    }

    RECOMP_RETURN(bool, false);
}

RECOMP_DLL_FUNC(AudioApiNative_Ready) {
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_Tick) {
    workerThreadNotify();
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugSetEnabled) {
    auto enabled = RECOMP_ARG(uint32_t, 0);
    Debug::setEnabled(enabled != 0);
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugSetMixerOpen) {
    auto open = RECOMP_ARG(uint32_t, 0);
    Debug::setMixerOpen(open != 0);
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugSetKhzMode) {
    auto enabled = RECOMP_ARG(uint32_t, 0) != 0;
    Debug::setKhzMode(enabled);
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugSetSnapshot) {
    auto initPhase = RECOMP_ARG(uint32_t, 0);
    auto mainSeqId = RECOMP_ARG(int32_t, 1);
    auto subSeqId = RECOMP_ARG(int32_t, 2);
    auto args = TO_PTR(int32_t, RECOMP_ARG(int32_t, 3));
    Debug::setSnapshot(initPhase, mainSeqId, subSeqId, args[0], args[1]);
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugSetSeqPlayer) {
    auto playerIndex = RECOMP_ARG(uint32_t, 0);
    auto seqId = RECOMP_ARG(int32_t, 1);
    auto args = TO_PTR(int32_t, RECOMP_ARG(int32_t, 2));
    Debug::setSeqPlayerPacked(playerIndex, seqId, args);
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugEvent) {
    auto tag = RECOMP_ARG(uint32_t, 0);
    auto a = RECOMP_ARG(int32_t, 1);
    auto b = RECOMP_ARG(int32_t, 2);
    auto args = TO_PTR(int32_t, RECOMP_ARG(int32_t, 3));
    Debug::pushEvent(tag, a, b, args[0], args[1]);
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugPushEvent) {
    auto tag = RECOMP_ARG(uint32_t, 0);
    auto a = RECOMP_ARG(int32_t, 1);
    auto b = RECOMP_ARG(int32_t, 2);
    auto args = TO_PTR(int32_t, RECOMP_ARG(int32_t, 3));
    Debug::pushEvent(tag, a, b, args[0], args[1]);
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugRegisterTag) {
    auto tag = RECOMP_ARG(uint32_t, 0);
    auto name = RECOMP_ARG_U8STR(1);
    Debug::registerTag(tag, reinterpret_cast<const char*>(name.c_str()));
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugGetFontPushEnabled) {
    RECOMP_RETURN(uint32_t, (Debug::isEnabled() && Debug::isFontPushEnabled()) ? 1u : 0u);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugGetMixOverride) {
    if (!Debug::isEnabled() || !Debug::isMixerOpen()) {
        RECOMP_RETURN(bool, false);
    }
    auto playerIndex  = RECOMP_ARG(uint32_t, 0);
    auto channelIndex = RECOMP_ARG(uint32_t, 1);
    auto out          = TO_PTR(int32_t, RECOMP_ARG(int32_t, 2));
    auto ov = Debug::getMixOverride(playerIndex, channelIndex);
    // out[0] = pan (-1 = passthrough), out[1] = vol_milli (-1 = passthrough),
    // out[2] = muted, out[3] = reverb (-1 = passthrough)
    if (!ov.active) {
        out[0] = -1;
        out[1] = -1;
        out[2] = 0;
        out[3] = -1;
    } else {
        out[0] = ov.pan;
        out[1] = ov.volMilli;
        out[2] = ov.muted ? 1 : 0;
        out[3] = ov.reverb;
    }
    RECOMP_RETURN(bool, true);
}

// Query for a pending sample patch without consuming it.
// Returns false if no patch is pending.
// Writes an 88-byte header to out_ptr (must be allocated by caller in rdram):
//   [0]  s32  fontId
//   [4]  s32  instId       (-1 = not instrument)
//   [8]  s32  drumId       (-1 = not drum)
//   [12] s32  sfxId        (-1 = not sfx)
//   [16] u8   pitchRegion  (0=low,1=normal,2=high)
//   [17] u8   pad[3]
//   [20] f32  tuning
//   [24] u32  numSamples
//   [28] u32  sampleDataSize (bytes)
//   [32] u32  codec
//   [36] s32  bookOrder
//   [40] s32  bookNumPredictors
//   [44] u32  bookCoeffCount  (number of s16 entries = 8 * order * numPred)
//   [48] u32  loopStart
//   [52] u32  loopEnd
//   [56] s32  loopCount
//   [60] s16  loopPredictorState[16]   (32 bytes)
RECOMP_DLL_FUNC(AudioApiNative_DebugQuerySamplePatch) {
    if (!Debug::isEnabled() || !Debug::hasSamplePatch()) {
        RECOMP_RETURN(bool, false);
    }

    // Peek without consuming.
    auto patch = Debug::peekSamplePatch();
    if (!patch.has_value()) {
        RECOMP_RETURN(bool, false);
    }
    const auto& p = patch.value();

    auto outPtr = RECOMP_ARG(int32_t, 0);
    auto* hdr = TO_PTR(uint8_t, outPtr);

    // RDRAM byte-swap rules (recomp N64 big-endian in host little-endian RDRAM):
    //   s32/u32/f32 (4-byte, 4-byte-aligned): no XOR — MEM_W reads straight *(int32_t*)
    //   s16/u16     (2-byte): host offset = logical_offset ^ 2  — MEM_H uses ^ 2
    //   s8/u8       (1-byte): host offset = logical_offset ^ 3  — MEM_B uses ^ 3
    auto writeS32 = [&](int offset, int32_t v)  { std::memcpy(hdr + offset, &v, 4); };
    auto writeU32 = [&](int offset, uint32_t v) { std::memcpy(hdr + offset, &v, 4); };
    auto writeF32 = [&](int offset, float v)    { std::memcpy(hdr + offset, &v, 4); };
    auto writeS16 = [&](int offset, int16_t v) {
        uint16_t u = static_cast<uint16_t>(v);
        hdr[offset]     = static_cast<uint8_t>(u >> 8);
        hdr[offset + 1] = static_cast<uint8_t>(u & 0xFF);
    };
    auto writeU8  = [&](int offset, uint8_t v)  { hdr[offset ^ 3] = v; };

    writeS32( 0, p.fontId);
    writeS32( 4, p.instId);
    writeS32( 8, p.drumId);
    writeS32(12, p.sfxId);
    writeU8 (16, p.pitchRegion);  // u8 pitchRegion at offset 0x10, pad[3] at 0x11-0x13
    writeU8 (17, 0); writeU8(18, 0); writeU8(19, 0);
    writeF32(20, p.tuning);
    writeU32(24, p.numSamples);
    writeU32(28, static_cast<uint32_t>(p.adpcmData.size()));
    writeU32(32, p.codec);
    writeS32(36, p.bookOrder);
    writeS32(40, p.bookNumPredictors);
    writeU32(44, static_cast<uint32_t>(p.bookCoeffs.size()));
    writeU32(48, p.loopStart);
    writeU32(52, p.loopEnd);
    writeS32(56, p.loopCount);
    for (int i = 0; i < 16; i++) writeS16(60 + i * 2, p.loopPredictorState[i]);

    RECOMP_RETURN(bool, true);
}

// Consume the pending patch.
// arg0 = pointer to book coeffs buffer (s16[], size = bookCoeffCount * 2 bytes)
// arg1 = pointer to adpcm data buffer  (u8[], size = adpcmSize bytes)
// Both buffers must be pre-allocated by the caller (using adpcmSize/bookCoeffCount from QuerySamplePatch).
// Returns true and clears the pending patch.
RECOMP_DLL_FUNC(AudioApiNative_DebugTakeSamplePatch) {
    if (!Debug::isEnabled()) {
        RECOMP_RETURN(bool, false);
    }

    auto patch = Debug::takeSamplePatch();
    if (!patch.has_value()) {
        RECOMP_RETURN(bool, false);
    }
    const auto& p = patch.value();

    auto bookPtr = RECOMP_ARG(int32_t, 0);
    auto adpcmPtr = RECOMP_ARG(int32_t, 1);

    if (bookPtr != 0 && !p.bookCoeffs.empty()) {
        // The RSP reads the codebook via DMA as big-endian s16 values.
        auto* dst = TO_PTR(uint8_t, bookPtr);
        for (size_t i = 0; i < p.bookCoeffs.size(); i++) {
            uint16_t v = static_cast<uint16_t>(p.bookCoeffs[i]);
            dst[i * 2 + 0] = static_cast<uint8_t>(v >> 8);
            dst[i * 2 + 1] = static_cast<uint8_t>(v & 0xFF);
        }
    }

    if (adpcmPtr != 0 && !p.adpcmData.empty()) {
        // ADPCM data is read by the RSP via DMA (AudioApi_Dma_Mod → memcpy), not via MEM_B,
        // so it must be stored in natural byte order without XOR swapping.
        auto* dst = TO_PTR(uint8_t, adpcmPtr);
        std::memcpy(dst, p.adpcmData.data(), p.adpcmData.size());
    }

    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_DebugPushSoundFontInfos) {
    // arr layout (see debug.c for full spec):
    //   [0]                   = numPlayers
    //   [1..numPlayers]       = defaultFont per player
    //   [numPlayers+1]        = numFonts
    //   per font:
    //     [numInst, numDrums, numSfx,
    //      per-inst: regionMask, loTuning, midTuning, hiTuning,
    //      drumTunings..., sfxTunings...]
    auto arrPtr    = RECOMP_ARG(int32_t, 0);
    auto totalInts = RECOMP_ARG(uint32_t, 1);

    if (arrPtr == 0 || totalInts < 2) {
        RECOMP_RETURN(bool, false);
    }

    auto* arr = TO_PTR(int32_t, arrPtr);
    uint32_t idx = 0;

    int32_t numPlayers = arr[idx++];
    if (numPlayers < 0 || (uint32_t)numPlayers > totalInts) {
        RECOMP_RETURN(bool, false);
    }

    // Active fonts per player.
    std::ostringstream json;
    json << "{\"activeFonts\":[";
    for (int32_t p = 0; p < numPlayers; p++) {
        if (p != 0) json << ',';
        json << arr[idx++];
    }
    json << "],\"fonts\":[";

    if (idx >= totalInts) {
        json << "]}";
        Debug::updateSoundFontInfos(json.str());
        RECOMP_RETURN(bool, true);
    }

    int32_t numFonts = arr[idx++];
    for (int32_t i = 0; i < numFonts; i++) {
        if (idx + 3 > totalInts) break;
        int32_t numInst = arr[idx++];
        int32_t numDrum = arr[idx++];
        int32_t numSfx  = arr[idx++];
        std::vector<int32_t> instRegions;
        std::vector<int32_t> instTuningsLo;
        std::vector<int32_t> instTuningsMid;
        std::vector<int32_t> instTuningsHi;

        instRegions.reserve(numInst);
        instTuningsLo.reserve(numInst);
        instTuningsMid.reserve(numInst);
        instTuningsHi.reserve(numInst);

        for (int32_t j = 0; j < numInst; j++) {
            instRegions.push_back((idx < totalInts) ? arr[idx++] : 0);
            instTuningsLo.push_back((idx < totalInts) ? arr[idx++] : 0);
            instTuningsMid.push_back((idx < totalInts) ? arr[idx++] : 0);
            instTuningsHi.push_back((idx < totalInts) ? arr[idx++] : 0);
        }

        if (i != 0) json << ',';
        json << "{\"id\":" << i
             << ",\"numInstruments\":" << numInst
             << ",\"numDrums\":" << numDrum
             << ",\"numSfx\":" << numSfx
             << ",\"instRegions\":[";
        for (int32_t j = 0; j < numInst; j++) {
            if (j != 0) json << ',';
            json << instRegions[j];
        }
        json << "]"
             << ",\"instTuningsLo\":[";
        for (int32_t j = 0; j < numInst; j++) {
            if (j != 0) json << ',';
            json << instTuningsLo[j];
        }
        json << "]"
             << ",\"instTuningsMid\":[";
        for (int32_t j = 0; j < numInst; j++) {
            if (j != 0) json << ',';
            json << instTuningsMid[j];
        }
        json << "]"
             << ",\"instTuningsHi\":[";
        for (int32_t j = 0; j < numInst; j++) {
            if (j != 0) json << ',';
            json << instTuningsHi[j];
        }
        json << "]"
             << ",\"drumTunings\":[";
        for (int32_t j = 0; j < numDrum; j++) {
            if (j != 0) json << ',';
            json << ((idx < totalInts) ? arr[idx++] : 0);
        }
        json << "]"
             << ",\"sfxTunings\":[";
        for (int32_t j = 0; j < numSfx; j++) {
            if (j != 0) json << ',';
            json << ((idx < totalInts) ? arr[idx++] : 0);
        }
        json << "]}";
    }
    json << "]}";

    Debug::updateSoundFontInfos(json.str());
    RECOMP_RETURN(bool, true);
}

RECOMP_DLL_FUNC(AudioApiNative_Dma) {
    auto ptr = RECOMP_ARG(int32_t, 0);
    size_t size = RECOMP_ARG(uint32_t, 1);
    size_t offset = RECOMP_ARG(uint32_t, 2);

    auto args = TO_PTR(uint32_t, RECOMP_ARG(int32_t, 3));
    size_t resourceId = args[0];

    try {
        std::shared_ptr<Resource::Abstract> resource;
        {
            std::shared_lock<std::shared_mutex> lock(gResourceDataMutex);

            auto it = gResourceData.find(resourceId);
            if (it == gResourceData.end()) {
                throw std::invalid_argument("Invalid resourceId " + std::to_string(resourceId));
            }

            resource = std::static_pointer_cast<Resource::Abstract>(it->second);
        }

        resource->dma(rdram, ptr, offset, size, args[1], args[2]);
        queuePreload(resourceId);

        if (Debug::isEnabled()) {
            auto af = std::dynamic_pointer_cast<Resource::Audiofile>(resource);
            if (af) {
                Debug::setStreamState(
                    static_cast<uint32_t>(resourceId),
                    args[1],
                    static_cast<int32_t>(offset),
                    af->metadata->sampleCount,
                    af->metadata->loopStart,
                    af->metadata->loopEnd,
                    af->metadata->loopCount
                );
            }
        }

        RECOMP_RETURN(bool, true);

    } catch (const fs::filesystem_error& e) {
        PLOG_ERROR << "DMA Error: " << e.what();
    } catch (const std::invalid_argument& e) {
        PLOG_ERROR << "DMA Error: " << e.what();
    } catch (const std::runtime_error& e) {
        PLOG_ERROR << "DMA Error: " << e.what();
    } catch (...) {
        PLOG_ERROR << "DMA Error: Unknown error";
    }

    RECOMP_RETURN(bool, false);
}

RECOMP_DLL_FUNC(AudioApiNative_AddResource) {
    auto info = RECOMP_ARG(AudioApiResourceInfo*, 0);
    auto baseDir = RECOMP_ARG_U8STR(1);
    auto path = RECOMP_ARG_U8STR(2);
    auto cacheStrategy = Resource::parseCacheStrategy(info->cacheStrategy);

    try {
        // TODO: if info->filesize exists, avoid opening file and just check that it exists
        auto file = gVfs.openFile(baseDir, path);
        auto resource = std::make_shared<Resource::Generic>(file, cacheStrategy);

        info->resourceId = sResourceCount++;
        info->cacheStrategy = static_cast<AudioApiCacheStrategy>(cacheStrategy);
        info->filesize = resource->size();
        file->close();

        {
            std::unique_lock<std::shared_mutex> lock(gResourceDataMutex);
            gResourceData[info->resourceId] = std::move(resource);
        }

        queuePreload(info->resourceId);

        RECOMP_RETURN(bool, true);

    } catch (const fs::filesystem_error& e) {
        PLOG_ERROR << "Error adding resource: " << e.what();
    } catch (const std::invalid_argument& e) {
        PLOG_ERROR << "Error adding resource: " << e.what();
    } catch (const std::runtime_error& e) {
        PLOG_ERROR << "Error adding resource: " << e.what();
    } catch (...) {
        PLOG_ERROR << "Error adding resource: Unknown error";
    }

    RECOMP_RETURN(bool, false);
}

RECOMP_DLL_FUNC(AudioApiNative_AddAudioFile) {
    auto info = RECOMP_ARG(AudioApiFileInfo*, 0);
    auto baseDir = RECOMP_ARG_U8STR(1);
    auto path = RECOMP_ARG_U8STR(2);
    auto codec = Decoder::parseType(info->codec);
    auto cacheStrategy = Resource::parseCacheStrategy(info->cacheStrategy);

    try {
        auto file = gVfs.openFile(baseDir, path);
        auto resource = std::make_shared<Resource::Audiofile>(file, codec, cacheStrategy);

        if (info->trackCount && info->sampleCount) {
            resource->metadata->setTrackCount(info->trackCount);
            resource->metadata->setSampleRate(info->sampleRate);
            resource->metadata->setSampleCount(info->sampleCount);
            resource->metadata->setLoopInfo(info->loopStart, info->loopEnd, info->loopCount);
            file->close();
        } else {
            resource->open();
            resource->probe();
            resource->close();
        }

        info->resourceId  = sResourceCount++;
        info->trackCount  = resource->metadata->trackCount;
        info->sampleRate  = resource->metadata->sampleRate;
        info->sampleCount = resource->metadata->sampleCount;
        info->loopStart   = resource->metadata->loopStart;
        info->loopEnd     = resource->metadata->loopEnd;
        info->loopCount   = resource->metadata->loopCount;
        info->cacheStrategy = static_cast<AudioApiCacheStrategy>(cacheStrategy);

        PLOG_DEBUG << "Added: " << file->fullpath();
        PLOG_DEBUG << "sampleRate: " << info->sampleRate << " sampleCount: " << info->sampleCount
                   << " trackCount: " << info->trackCount << " loopCount: " << info->loopCount
                   << " loopStart: " << info->loopStart << " loopEnd: " << info->loopEnd;

        {
            std::unique_lock<std::shared_mutex> lock(gResourceDataMutex);
            gResourceData[info->resourceId] = std::move(resource);
        }

        queuePreload(info->resourceId);

        RECOMP_RETURN(bool, true);

    } catch (const fs::filesystem_error& e) {
        PLOG_ERROR << "Error probing file: " << e.what();
    } catch (const std::invalid_argument& e) {
        PLOG_ERROR << "Error probing file: " << e.what();
    } catch (const std::runtime_error& e) {
        PLOG_ERROR << "Error probing file: " << e.what();
        //PLOG_ERROR << "Error probing file: " << baseDir << "/" << path << " Reason: " << e.what();
    } catch (...) {
        PLOG_ERROR << "Error probing file: Unknown error";
    }

    RECOMP_RETURN(bool, false);
}

RECOMP_DLL_FUNC(AudioApiNative_AddSampleBank) {
    auto info = RECOMP_ARG(AudioApiSampleBankInfo*, 0);
    auto baseDir = RECOMP_ARG_U8STR(1);
    auto path = RECOMP_ARG_U8STR(2);
    auto cacheStrategy = Resource::parseCacheStrategy(info->cacheStrategy);

    try {
        // TODO: if info->filesize exists, avoid opening file and just check that it exists
        auto file = gVfs.openFile(baseDir, path);
        auto resource = std::make_shared<Resource::SampleBank>(file, cacheStrategy);

        info->resourceId = sResourceCount++;
        info->cacheStrategy = static_cast<AudioApiCacheStrategy>(cacheStrategy);
        info->filesize = resource->size();
        file->close();

        {
            std::unique_lock<std::shared_mutex> lock(gResourceDataMutex);
            gResourceData[info->resourceId] = std::move(resource);
        }

        queuePreload(info->resourceId);

        RECOMP_RETURN(bool, true);

    } catch (const fs::filesystem_error& e) {
        PLOG_ERROR << "Error adding sample bank: " << e.what();
    } catch (const std::invalid_argument& e) {
        PLOG_ERROR << "Error adding sample bank: " << e.what();
    } catch (const std::runtime_error& e) {
        PLOG_ERROR << "Error adding sample bank: " << e.what();
    } catch (...) {
        PLOG_ERROR << "Error adding sample bank: Unknown error";
    }

    RECOMP_RETURN(bool, false);
}
