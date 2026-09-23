#include "tx_ambe_encoder.h"

// Alpha3 embedded placeholder.
//
// The reference AMBE+2 encoder used by tools/ambe_ref is MIT-licensed
// blip25-vocoder. It is intentionally not linked into the ESP32 firmware yet:
// the target-side Rust/FFI build needs to be integrated and validated first.
//
// Keeping this backend explicitly unavailable guarantees that alpha3 cannot
// accidentally transmit voice while the codec port is incomplete.

bool txAmbeEncoderAvailable()
{
    return false;
}

const char* txAmbeEncoderBackendName()
{
    return "none";
}

bool txAmbeEncoderBegin()
{
    return false;
}

void txAmbeEncoderReset()
{
}

void txAmbeEncoderEnd()
{
}

bool txAmbeEncodePcm160(const int16_t pcm[TX_AMBE_PCM_SAMPLES],
                        uint8_t ambe[TX_AMBE_FRAME_BYTES])
{
    (void)pcm;
    (void)ambe;
    return false;
}
