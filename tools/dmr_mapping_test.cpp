#include <cstdio>
#include <cstdint>
#include <cstring>

#include "dmr_ambe_mapping.h"

int main()
{
    if (!dmrAmbeMappingSelfTest()) {
        std::fprintf(stderr, "DMR AMBE mapping self-test failed\n");
        return 1;
    }

    const uint8_t canonical[9] = {
        0xD3, 0x6A, 0x91, 0x05, 0xFE, 0x42, 0x18, 0xBC, 0x77
    };

    uint8_t dmr[9] = {0};
    uint8_t recovered[9] = {0};
    char frame[4][24];

    dmrCanonical72ToInterleaved(canonical, dmr);
    dmrInterleaved72ToMbelib(dmr, frame);
    dmrMbelibToCanonical72(frame, recovered);

    if (std::memcmp(canonical, recovered, sizeof(canonical)) != 0) {
        std::fprintf(stderr, "canonical -> DMR -> RX round-trip mismatch\n");
        return 2;
    }

    std::printf("DMR mapping round-trip OK: ");
    for (uint8_t b : dmr)
        std::printf("%02x", b);
    std::printf("\n");
    return 0;
}
