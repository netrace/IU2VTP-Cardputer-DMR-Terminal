#include "tx_ambe_encoder.h"

#if defined(IU2VTP_TX_AMBE_RUST)

#include "tx_ambe_blip25_ffi.h"

static Iu2vtpAmbeEncoder* g_encoder = nullptr;

bool txAmbeEncoderAvailable()
{
    return true;
}

const char* txAmbeEncoderBackendName()
{
    return "blip25-rust";
}

bool txAmbeEncoderBegin()
{
    if (g_encoder) return true;
    g_encoder = iu2vtp_ambe_encoder_create();
    return g_encoder != nullptr;
}

void txAmbeEncoderReset()
{
    if (g_encoder)
        iu2vtp_ambe_encoder_reset(g_encoder);
}

void txAmbeEncoderEnd()
{
    if (g_encoder) {
        iu2vtp_ambe_encoder_destroy(g_encoder);
        g_encoder = nullptr;
    }
}

bool txAmbeEncodePcm160(const int16_t pcm[TX_AMBE_PCM_SAMPLES],
                        uint8_t ambe[TX_AMBE_FRAME_BYTES])
{
    if (!g_encoder || !pcm || !ambe) return false;
    return iu2vtp_ambe_encode_pcm160(g_encoder, pcm, ambe);
}

#else

// Safe fallback used until an ESP32-S3 Rust staticlib is supplied.
// This keeps voice generation impossible by default.

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

#endif
