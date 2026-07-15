#ifndef __RECOMP_SOUNDFONT__
#define __RECOMP_SOUNDFONT__

#include <global.h>

// To differentiate between a CustomSoundFont and vanilla soundfont, check the first byte of the
// memory address. For vanilla soundfonts the first value is a four byte offset, so the first byte
// is almost certainly a zero unless the soundfont is 16 MiB in size. Since the largest vanilla
// soundfont is only 32 KiB, this is a safe assumption.
typedef enum SoundFontType : u8 {
    SOUNDFONT_VANILLA = 0,
    SOUNDFONT_CUSTOM,
} SoundFontType;

// Number of s16 codebook entries actually present for a book. The RSP ucode
// (and the native voice mirror) decode every book with the fixed order-2 page
// layout, so header.order does not describe the data — banks in the wild ship
// lying order fields (e.g. an order=4 header over order-2 data). Sizing reads
// or copies by header.order over-reads past the real coefficients (and
// potentially past the containing allocation).
#define BOOK_CODEBOOK_NUM_ENTRIES(book) (SAMPLES_PER_FRAME * 2 * (book)->header.numPredictors)

typedef struct CustomSoundFont {
    SoundFontType type;
    u16 sampleBank1;
    u16 sampleBank2;
    u8 numInstruments;
    u8 numDrums;
    u16 numSfx;
    u8 instrumentsCapacity;
    u8 drumsCapacity;
    u16 sfxCapacity;
    Instrument** instruments;
    Drum** drums;
    SoundEffect* soundEffects;
} CustomSoundFont;

#endif
