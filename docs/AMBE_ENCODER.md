# AMBE+2 encoder note for the PTT feature branch

The production repository remains MIT licensed.

## Candidate encoder

The current alpha3 reference implementation uses
`OpenBLIP25/blip25-vocoder` **only in host-side development tooling**.

The upstream crate is MIT licensed and exposes the half-rate 3600×2450
AMBE+2 codec used by P25 Phase 2 and, below the carrier-specific interleave,
DMR/NXDN.

It is not yet linked into the ESP32 firmware.

## Patent notice

The upstream project explicitly documents that its AMBE+2 implementation may
read on active patent claims and identifies US8359197 with an anticipated
expiration date of 2028-05-20.

This repository does not make an independent legal conclusion about that
analysis. Anyone building, running, or distributing an AMBE+2 encoder should
review the upstream PATENT_NOTICE and perform their own due diligence.

Upstream:
https://github.com/OpenBLIP25/blip25-vocoder

## Alpha3 boundary

Alpha3 provides:

- a stable embedded encoder interface: 160 PCM samples -> 9 AMBE bytes;
- an intentionally unavailable ESP32 backend;
- a host-side reference encoder and round-trip conformance check;
- no DMR voice network transmission.

The embedded backend will be enabled only after the Rust/ESP32 integration is
validated separately.
