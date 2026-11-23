# Third-Party Notices and Technical Citations (`zcstream`)

`zcstream` is licensed under the **MIT License** (see [LICENSE](LICENSE)).

## First-Party Components

- **`zcrudp`** (`https://github.com/i-Charlys/zcrudp`) — MIT License, Copyright (c) 2026 CAILLON Charles.
- **`zcvid`** (`https://github.com/i-Charlys/zcvid`) — MIT License, Copyright (c) 2026 CAILLON Charles.
- **`zpsync`** (`https://github.com/i-Charlys/zpsync`) — MIT License, Copyright (c) 2026 CAILLON Charles.

## Third-Party Components

### FFmpeg `nv-codec-headers` (`nvEncodeAPI.h`)
- **Upstream**: https://github.com/FFmpeg/nv-codec-headers
- **License**: MIT License
- **Usage**: Optional NVIDIA Video Codec SDK (`NVENC`) interface header auto-fetched at CMake configure time when direct NVENC encoding is enabled on Windows x64.

### Pyrowave (Optional Vulkan Compute Wavelet Codec)
- **Author**: Hans-Kristian Arntzen (Themaister)
- **Upstream**: https://github.com/Themaister/pyrowave
- **License**: MIT License (and Granite submodule dependencies under MIT)
- **Usage**: Optional Vulkan Compute 9/7 CDF wavelet codec backend when built in `third_party/pyrowave-master`.

## Standards & Academic Citations

- **Transport & FEC**: IETF RFC 8439 (ChaCha20-Poly1305), IETF RFC 6582 (NewReno), IETF RFC 2018 (SACK), IETF RFC 8298 (SCReAM pacing), Bloemer et al. 1995 (Cauchy GF(2^8) MDS erasure coding).
- **Video Coding**: ITU-T H.264 / Malvar et al. 2003 (4×4 integer transform & quantization), ITU-T H.265 / HEVC (4-tap quarter-pel interpolation), Jayant 1973 (Adaptive Delta Modulation), Martin 1979 / Moffat et al. 1998 (Range coding).
- **P2P Rendezvous**: IETF RFC 7748 / Bernstein 2006 (X25519), IETF RFC 5389 / 8489 (STUN), BEP-0005 (Mainline DHT), NIST FIPS 180-4, IETF RFC 2104.
- **Platform APIs**: Microsoft Direct3D 11, DXGI Desktop Duplication, Windows Media Foundation, Winsock2 / Registered I/O (RIO).
