#include <cstdio>
#include <cstdint>
#include <cstring>

#include "rewind_tx_protocol.h"

static uint16_t get16le(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32le(const uint8_t* p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int fail(const char* msg)
{
    std::fprintf(stderr, "%s\n", msg);
    return 1;
}

static bool testVoiceLc()
{
    uint8_t groupLc[REWIND_TX_VOICE_LC_LEN] = {0};
    rewindTxBuildVoiceLc(groupLc, 2232489U, 222998U, false);

    if (groupLc[0] != 0x00 || groupLc[1] != 0x00 || groupLc[2] != 0x00)
        return false;
    if (groupLc[3] != 0x03 || groupLc[4] != 0x67 || groupLc[5] != 0x16)
        return false;
    if (groupLc[6] != 0x22 || groupLc[7] != 0x10 || groupLc[8] != 0xE9)
        return false;

    uint8_t privateLc[REWIND_TX_VOICE_LC_LEN] = {0};
    rewindTxBuildVoiceLc(privateLc, 2232489U, 2221234U, true);

    if (privateLc[0] != 0x03)
        return false;
    if (privateLc[3] != 0x21 || privateLc[4] != 0xE4 || privateLc[5] != 0xB2)
        return false;

    // Parity bytes must be populated after RS(12,9) + Voice-LC 0x96 mask.
    if (groupLc[9] == 0 && groupLc[10] == 0 && groupLc[11] == 0)
        return false;

    return true;
}

int main()
{
    uint8_t h[18];

    // Verified Z3DMR/live-server TX opener: DMR Voice LC (0x0911), realtime.
    rewindTxBuildHeader(h, 0x0911, 0x0001, 1, REWIND_TX_VOICE_LC_LEN);
    if (std::memcmp(h, "REWIND01", 8) != 0) return fail("signature");
    if (get16le(h + 8) != 0x0911) return fail("voice-lc type");
    if (get16le(h + 10) != 0x0001) return fail("voice-lc flags");
    if (get32le(h + 12) != 1) return fail("voice-lc sequence");
    if (get16le(h + 16) != REWIND_TX_VOICE_LC_LEN) return fail("voice-lc length");

    if (!testVoiceLc()) return fail("voice-lc payload");

    uint8_t audioHeader[18];
    rewindTxBuildHeader(audioHeader, 0x0920, 0x0001, 3, 27);
    if (get16le(audioHeader + 8) != 0x0920) return fail("audio type");
    if (get16le(audioHeader + 10) != 0x0001) return fail("audio flags");
    if (get16le(audioHeader + 16) != 27) return fail("audio len");

    uint8_t termHeader[18];
    rewindTxBuildHeader(termHeader, 0x0912, 0x0001, 4, 0);
    if (get16le(termHeader + 8) != 0x0912) return fail("term type");
    if (get16le(termHeader + 10) != 0x0001) return fail("term flags");
    if (get16le(termHeader + 16) != 0) return fail("term len");

    std::puts("Rewind TX framing OK: 0x0911 x2 -> 0x0920 -> 0x0912");
    return 0;
}
