#include "rewind_tx_protocol.h"

#include <string.h>

static inline void put16leLocal(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void put32leLocal(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void copyCall10(uint8_t* dst, const char* src)
{
    memset(dst, 0, 10);
    if (!src) return;
    for (size_t i = 0; i < 10 && src[i]; ++i)
        dst[i] = (uint8_t)src[i];
}

void rewindTxBuildHeader(uint8_t out[REWIND_TX_HEADER_LEN],
                         uint16_t type,
                         uint16_t flags,
                         uint32_t sequence,
                         uint16_t payloadLen)
{
    memset(out, 0, REWIND_TX_HEADER_LEN);
    memcpy(out, "REWIND01", 8);
    put16leLocal(out + 8, type);
    put16leLocal(out + 10, flags);
    put32leLocal(out + 12, sequence);
    put16leLocal(out + 16, payloadLen);
}

void rewindTxBuildSuperHeader(uint8_t out[REWIND_TX_SUPERHEADER_LEN],
                              uint32_t sessionType,
                              uint32_t sourceId,
                              uint32_t targetId,
                              const char* sourceCallsign,
                              const char* targetCallsign)
{
    memset(out, 0, REWIND_TX_SUPERHEADER_LEN);
    put32leLocal(out + 0, sessionType);
    put32leLocal(out + 4, sourceId);
    put32leLocal(out + 8, targetId);
    copyCall10(out + 12, sourceCallsign);
    copyCall10(out + 22, targetCallsign);
}


static uint8_t gfMulLocal(uint8_t a, uint8_t b)
{
    uint8_t result = 0;
    while (b) {
        if (b & 1U) result ^= a;
        b >>= 1U;
        const bool carry = (a & 0x80U) != 0;
        a <<= 1U;
        if (carry) a ^= 0x1DU; // GF(2^8), primitive polynomial 0x11D
    }
    return result;
}

static void rs129ParityLocal(const uint8_t msg[9], uint8_t parity[3])
{
    // DMR RS(12,9,4) generator:
    // g(x) = x^3 + 0x0E*x^2 + 0x38*x + 0x40.
    uint8_t p0 = 0;
    uint8_t p1 = 0;
    uint8_t p2 = 0;

    for (size_t i = 0; i < 9; ++i) {
        const uint8_t d = msg[i] ^ p2;
        p2 = p1 ^ gfMulLocal(0x0E, d);
        p1 = p0 ^ gfMulLocal(0x38, d);
        p0 = gfMulLocal(0x40, d);
    }

    parity[0] = p2;
    parity[1] = p1;
    parity[2] = p0;
}

void rewindTxBuildVoiceLc(uint8_t out[REWIND_TX_VOICE_LC_LEN],
                          uint32_t sourceId,
                          uint32_t targetId,
                          bool privateCall)
{
    memset(out, 0, REWIND_TX_VOICE_LC_LEN);

    // DMR Full Link Control:
    // FLCO, FID, service options, destination (24-bit BE), source (24-bit BE).
    out[0] = privateCall ? 0x03U : 0x00U;
    out[1] = 0x00U;
    out[2] = 0x00U;

    out[3] = (uint8_t)((targetId >> 16) & 0xFFU);
    out[4] = (uint8_t)((targetId >> 8) & 0xFFU);
    out[5] = (uint8_t)(targetId & 0xFFU);

    out[6] = (uint8_t)((sourceId >> 16) & 0xFFU);
    out[7] = (uint8_t)((sourceId >> 8) & 0xFFU);
    out[8] = (uint8_t)(sourceId & 0xFFU);

    uint8_t parity[3] = {0};
    rs129ParityLocal(out, parity);

    // Voice LC Header parity mask used by DMR.
    out[9]  = parity[0] ^ 0x96U;
    out[10] = parity[1] ^ 0x96U;
    out[11] = parity[2] ^ 0x96U;
}
