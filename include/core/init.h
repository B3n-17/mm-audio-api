#ifndef __AUDIO_API_INIT__
#define __AUDIO_API_INIT__

#include <global.h>

// Runtime values — set from config in AudioLoad_Init before heap/effects init.
// When disabled, FREQ_FACTOR=1.0 and NUM_SUB_UPDATES=1 (vanilla 32kHz behaviour).
extern bool gAudioApi48kHzEnabled;
#define FREQ_FACTOR     (gAudioApi48kHzEnabled ? 1.5f : 1.0f)
#define NUM_SUB_UPDATES (gAudioApi48kHzEnabled ? 2    : 1)

#undef AIBUF_LEN
#undef AIBUF_SIZE
#define AIBUF_LEN (88 * SAMPLES_PER_FRAME * NUM_SUB_UPDATES) // number of samples
#define AIBUF_SIZE (AIBUF_LEN * SAMPLE_SIZE) // number of bytes

typedef enum {
    AUDIOAPI_INIT_NOT_READY,
    AUDIOAPI_INIT_QUEUEING,
    AUDIOAPI_INIT_QUEUED,
    AUDIOAPI_INIT_READY,
} AudioApiInitPhase;

extern AudioApiInitPhase gAudioApiInitPhase;

#endif
