#include "dmr_ambe_mapping.h"

#include <string.h>

static inline uint8_t bitMsb(const uint8_t* data, int bitIndex)
{
    return (data[bitIndex >> 3] >> (7 - (bitIndex & 7))) & 1U;
}

static inline void setBitMsb(uint8_t* data, int bitIndex, uint8_t value)
{
    const uint8_t mask = (uint8_t)(1U << (7 - (bitIndex & 7)));
    if (value)
        data[bitIndex >> 3] |= mask;
    else
        data[bitIndex >> 3] &= (uint8_t)~mask;
}

// DMR AMBE interleave schedule (DSD / dmr_utils lineage).
static constexpr uint8_t rW[36] = {
    0,1,0,1,0,1,
    0,1,0,1,0,1,
    0,1,0,1,0,1,
    0,1,0,1,0,2,
    0,2,0,2,0,2,
    0,2,0,2,0,2
};

static constexpr uint8_t rX[36] = {
    23,10,22,9,21,8,
    20,7,19,6,18,5,
    17,4,16,3,15,2,
    14,1,13,0,12,10,
    11,9,10,8,9,7,
    8,6,7,5,6,4
};

static constexpr uint8_t rY[36] = {
    0,2,0,2,0,2,
    0,2,0,3,0,3,
    1,3,1,3,1,3,
    1,3,1,3,1,3,
    1,3,1,3,1,3,
    1,3,1,3,1,3
};

static constexpr uint8_t rZ[36] = {
    5,3,4,2,3,1,
    2,0,1,13,0,12,
    22,11,21,10,20,9,
    19,8,18,7,17,6,
    16,5,15,4,14,3,
    13,2,12,1,11,0
};

void dmrInterleaved72ToMbelib(const uint8_t frame9[9],
                              char ambe_fr[4][24])
{
    memset(ambe_fr, 0, 4 * 24 * sizeof(char));

    int bitIndex = 0;
    for (int i = 0; i < 36; ++i) {
        const char bit1 = (char)bitMsb(frame9, bitIndex++);
        const char bit0 = (char)bitMsb(frame9, bitIndex++);

        ambe_fr[rW[i]][rX[i]] = bit1;
        ambe_fr[rY[i]][rZ[i]] = bit0;
    }
}

void dmrCanonical72ToMbelib(const uint8_t canonical9[9],
                            char ambe_fr[4][24])
{
    static constexpr int widths[4] = {24, 23, 11, 14};

    memset(ambe_fr, 0, 4 * 24 * sizeof(char));

    int bitIndex = 0;
    for (int row = 0; row < 4; ++row) {
        // mbelib treats the high matrix index as the MSB of each code vector.
        for (int col = widths[row] - 1; col >= 0; --col) {
            ambe_fr[row][col] = (char)bitMsb(canonical9, bitIndex++);
        }
    }
}

void dmrMbelibToCanonical72(const char ambe_fr[4][24],
                            uint8_t canonical9[9])
{
    static constexpr int widths[4] = {24, 23, 11, 14};

    memset(canonical9, 0, 9);

    int bitIndex = 0;
    for (int row = 0; row < 4; ++row) {
        for (int col = widths[row] - 1; col >= 0; --col) {
            setBitMsb(canonical9, bitIndex++,
                      (uint8_t)(ambe_fr[row][col] ? 1 : 0));
        }
    }
}

void dmrCanonical72ToInterleaved(const uint8_t canonical9[9],
                                 uint8_t frame9[9])
{
    char ambe_fr[4][24];
    dmrCanonical72ToMbelib(canonical9, ambe_fr);

    memset(frame9, 0, 9);

    int bitIndex = 0;
    for (int i = 0; i < 36; ++i) {
        setBitMsb(frame9, bitIndex++,
                  (uint8_t)(ambe_fr[rW[i]][rX[i]] ? 1 : 0));
        setBitMsb(frame9, bitIndex++,
                  (uint8_t)(ambe_fr[rY[i]][rZ[i]] ? 1 : 0));
    }
}

static bool roundTripOne(const uint8_t canonical[9])
{
    uint8_t interleaved[9] = {0};
    uint8_t recovered[9] = {0};
    char ambe_fr[4][24];

    dmrCanonical72ToInterleaved(canonical, interleaved);
    dmrInterleaved72ToMbelib(interleaved, ambe_fr);
    dmrMbelibToCanonical72(ambe_fr, recovered);

    return memcmp(canonical, recovered, 9) == 0;
}

bool dmrAmbeMappingSelfTest()
{
    static constexpr uint8_t patterns[][9] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
        {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
        {0xAA,0x55,0x81,0x7E,0x13,0xC4,0x69,0x96,0x0F},
        {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x5A},
    };

    for (const auto& p : patterns) {
        if (!roundTripOne(p))
            return false;
    }
    return true;
}
