#ifndef __AUDIO_API_DEBUG_H__
#define __AUDIO_API_DEBUG_H__

/*! \file audio_api/debug.h
    \brief Debug event API for downstream mods.

    Allows any mod that depends on magemods_audio_api to push custom events and
    register human-readable tag names that appear in the Audio API debug UI at
    http://127.0.0.1:18480/audio-debug.html.

    Tag namespace
    -------------
    Tags 1–4095 (0x000–0xFFF) are reserved for internal audio API use.
    Downstream mods must use tags in the range 0x1000–0xFFFFFFFF.

    To avoid collisions between unrelated mods, pick a distinctive base value
    and register your tags during AudioApi_Init:

        #define MY_MOD_TAG_BASE  0x4D594D00u   // 'MYM\0'
        #define MY_TAG_FOO       (MY_MOD_TAG_BASE + 0)
        #define MY_TAG_BAR       (MY_MOD_TAG_BASE + 1)

    Then in AudioApi_Init:
        AudioApi_DebugRegisterTag(MY_TAG_FOO, "MY_MOD_FOO");
        AudioApi_DebugRegisterTag(MY_TAG_BAR, "MY_MOD_BAR");
 */

#include "types.h"

/*! Push a custom debug event visible in the Audio API debug UI.
 *
 *  Has no effect when the debug HTTP server is not enabled.
 *  Thread-safe; drops the event (incrementing the dropped counter) if the
 *  internal lock is contended rather than blocking the caller.
 *
 *  \param tag  Your event type identifier (must be >= 0x1000).
 *  \param a    Arbitrary signed 32-bit payload.
 *  \param b    Arbitrary signed 32-bit payload.
 *  \param c    Arbitrary signed 32-bit payload.
 *  \param d    Arbitrary signed 32-bit payload.
 */
RECOMP_IMPORT("magemods_audio_api", void AudioApi_DebugPushEvent(u32 tag, s32 a, s32 b, s32 c, s32 d));

/*! Register a human-readable name for a custom event tag.
 *
 *  Call this during AudioApi_Init. The name is shown in the Events tab of the
 *  debug UI instead of the raw numeric tag value. Re-registering the same tag
 *  overwrites the previous name.
 *
 *  \param tag   The tag value to name (must be >= 0x1000).
 *  \param name  Null-terminated ASCII string. Copied internally; the pointer
 *               does not need to remain valid after the call.
 */
RECOMP_IMPORT("magemods_audio_api", void AudioApi_DebugRegisterTag(u32 tag, const char* name));

#endif
