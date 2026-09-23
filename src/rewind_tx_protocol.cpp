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

void rewindTxBuildGroupSuperHeader(uint8_t out[REWIND_TX_SUPERHEADER_LEN],
                                   uint32_t sourceId,
                                   uint32_t targetId,
                                   const char* sourceCallsign,
                                   const char* targetCallsign)
{
    memset(out, 0, REWIND_TX_SUPERHEADER_LEN);
    put32leLocal(out + 0, 7); // GroupVoice
    put32leLocal(out + 4, sourceId);
    put32leLocal(out + 8, targetId);
    copyCall10(out + 12, sourceCallsign);
    copyCall10(out + 22, targetCallsign);
}
