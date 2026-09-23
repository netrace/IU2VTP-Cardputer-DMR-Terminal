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

int main()
{
    if (!testVoiceLc()) return 10;
    uint8_t h[18];
    rewindTxBuildHeader(h, 0x0928, 0x0001, 0x11223344, 32);

    if (std::memcmp(h, "REWIND01", 8) != 0) return fail("signature");
    if (get16le(h + 8) != 0x0928) return fail("type");
    if (get16le(h + 10) != 0x0001) return fail("flags");
    if (get32le(h + 12) != 0x11223344) return fail("sequence");
    if (get16le(h + 16) != 32) return fail("length");

    uint8_t sh[32];
    rewindTxBuildGroupSuperHeader(sh, 2232489, 222, "IU2VTP", "TG222");

    if (get32le(sh + 0) != 7) return fail("session type");
    if (get32le(sh + 4) != 2232489) return fail("source id");
    if (get32le(sh + 8) != 222) return fail("target id");
    if (std::memcmp(sh + 12, "IU2VTP", 6) != 0) return fail("source call");
    if (std::memcmp(sh + 22, "TG222", 5) != 0) return fail("target call");

    uint8_t priv[32];
    rewindTxBuildSuperHeader(priv, 5, 2232489, 2221234, "IU2VTP", "2221234");

    if (get32le(priv + 0) != 5) return fail("private session type");
    if (get32le(priv + 4) != 2232489) return fail("private source id");
    if (get32le(priv + 8) != 2221234) return fail("private target id");
    if (std::memcmp(priv + 22, "2221234", 7) != 0) return fail("private target call");

    uint8_t audioHeader[18];
    rewindTxBuildHeader(audioHeader, 0x0920, 0x0001, 1, 27);
    if (get16le(audioHeader + 8) != 0x0920) return fail("audio type");
    if (get16le(audioHeader + 16) != 27) return fail("audio len");

    uint8_t termHeader[18];
    rewindTxBuildHeader(termHeader, 0x0912, 0x0001, 2, 0);
    if (get16le(termHeader + 8) != 0x0912) return fail("term type");
    if (get16le(termHeader + 16) != 0) return fail("term len");

    std::puts("Rewind TX framing OK");
    return 0;
}


static bool testVoiceLc()
{
    uint8_t lc[REWIND_TX_VOICE_LC_LEN] = {0};
    rewindTxBuildVoiceLc(lc, 0x2F9BE5U, 0x000C30U, false);

    // Core LC fields: group FLCO/FID/options, dst24, src24.
    if (lc[0] != 0x00 || lc[1] != 0x00 || lc[2] != 0x00)
        return false;
    if (lc[3] != 0x00 || lc[4] != 0x0C || lc[5] != 0x30)
        return false;
    if (lc[6] != 0x2F || lc[7] != 0x9B || lc[8] != 0xE5)
        return false;

    uint8_t privateLc[REWIND_TX_VOICE_LC_LEN] = {0};
    rewindTxBuildVoiceLc(privateLc, 2232489U, 2221234U, true);
    return privateLc[0] == 0x03;
}
