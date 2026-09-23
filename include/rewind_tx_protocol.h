#pragma once

#include <stddef.h>
#include <stdint.h>

static constexpr size_t REWIND_TX_HEADER_LEN = 18;
static constexpr size_t REWIND_TX_SUPERHEADER_LEN = 32;

void rewindTxBuildHeader(uint8_t out[REWIND_TX_HEADER_LEN],
                         uint16_t type,
                         uint16_t flags,
                         uint32_t sequence,
                         uint16_t payloadLen);

void rewindTxBuildSuperHeader(uint8_t out[REWIND_TX_SUPERHEADER_LEN],
                              uint32_t sessionType,
                              uint32_t sourceId,
                              uint32_t targetId,
                              const char* sourceCallsign,
                              const char* targetCallsign);

inline void rewindTxBuildGroupSuperHeader(uint8_t out[REWIND_TX_SUPERHEADER_LEN],
                                          uint32_t sourceId,
                                          uint32_t targetId,
                                          const char* sourceCallsign,
                                          const char* targetCallsign)
{
    rewindTxBuildSuperHeader(out, 7, sourceId, targetId,
                             sourceCallsign, targetCallsign);
}
