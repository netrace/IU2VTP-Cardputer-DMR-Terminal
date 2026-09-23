#pragma once

#include <stdint.h>

// Shared DMR AMBE mapping.
//
// "Canonical" means the 72 AMBE+2 code-vector bits in this order:
//   C0: 24 bits MSB-first
//   C1: 23 bits MSB-first
//   C2: 11 bits MSB-first
//   C3: 14 bits MSB-first
//
// "Interleaved" means the 9-byte / 72-bit DMR air-order mapping used by the
// existing known-good RX path (rW/rX/rY/rZ lineage).

void dmrInterleaved72ToMbelib(const uint8_t frame9[9],
                              char ambe_fr[4][24]);

void dmrCanonical72ToMbelib(const uint8_t canonical9[9],
                            char ambe_fr[4][24]);

void dmrMbelibToCanonical72(const char ambe_fr[4][24],
                            uint8_t canonical9[9]);

void dmrCanonical72ToInterleaved(const uint8_t canonical9[9],
                                 uint8_t frame9[9]);

bool dmrAmbeMappingSelfTest();
