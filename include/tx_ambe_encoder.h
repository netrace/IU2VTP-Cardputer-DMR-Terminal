#pragma once

#include <stdint.h>
#include <stddef.h>

// AMBE+2 encoder boundary for the PTT/TX feature branch.
//
// Input:  160 signed PCM samples, mono, 8 kHz, 20 ms.
// Output: 9 bytes / 72 bits in canonical codec code-vector order.
//
// The embedded backend is deliberately optional. Alpha3 provides the stable
// interface and host-side conformance tooling without silently introducing a
// copyleft codec or enabling network voice TX.

static constexpr size_t TX_AMBE_PCM_SAMPLES = 160;
static constexpr size_t TX_AMBE_FRAME_BYTES = 9;

bool txAmbeEncoderAvailable();
const char* txAmbeEncoderBackendName();
bool txAmbeEncoderBegin();
void txAmbeEncoderReset();
void txAmbeEncoderEnd();
bool txAmbeEncodePcm160(const int16_t pcm[TX_AMBE_PCM_SAMPLES],
                        uint8_t ambe[TX_AMBE_FRAME_BYTES]);
