# zcstream

Ultra-low-latency GPU video streaming prototypes built on top of [`zcrudp::nextgen`](../zcrudp), combining:
1. **Direct NVENC (`nvEncodeAPI64.dll`) + D3D11/DXGI Zero-Copy** (`zcrudp_stream_bench.exe`)
2. **Pyrowave (Vulkan Compute Wavelets) + Winsock RIO Batch TX** (`zcrudp_pyrowave_bench.exe`)

## Measured Pipeline Latency (NVIDIA TITAN RTX, 1920×1080 @ 120 fps, loopback)

| Pipeline | Encode + Decode GPU | Network TX + RX (`zcrudp`) | Total p50 |
| :--- | :--- | :--- | :--- |
| **NVENC Direct (H.264, 25 Mbps, ARGB Zero-Copy)** | ~2.6 ms | ~82 µs | **3.33 ms** *(min 2.23 ms)* |
| **Pyrowave (Wavelets, ~120 Mbps) + Winsock RIO** | **~0.50 ms** *(0.29 + 0.21 ms)* | ~785 µs *(~105 pkts/frame)* | **1.33 ms** *(min 1.20 ms)* |

Under simulated degraded network (`--nasty`: 5% random loss + 8% 3-pkt bursts + 15% reordering + 5% duplicates + 2–5 ms OWD/jitter), Cauchy $\mathrm{GF}(2^8)$ FEC (`--fec 3`) runs in **32 µs p50 TX** and recovers frames in 0-RTT.

## Ternary HLSL pipeline (`zcrudp_ternary_bench.exe`)

Third prototype: the [`tern_vid`](../tern_vid) compute shaders (`cs_5_0`)
encode and decode YCbCr in VRAM with per-tile motion vectors. Motion vectors
travel on the FEC-protected path, delta records on the unprotected one.

`--psnr` reports the luma PSNR and a closed-loop check: the number of pixels
whose encoder reference differs from the decoder output. It must be `0` on a
clean link; under simulated loss the two diverge until the leaky integrator
and the rolling Intra-Refresh bring them back.

The source is the desktop when Desktop Duplication is available at the
requested size, otherwise a GPU-rendered test pattern, so bitrate depends on
what is on screen.

## Benchmark options

All three benchmarks accept `--frames N`, `--width W`, `--height H`,
`--fec N` (0-4 parity shards per frame) and the impairment simulator:
`--loss PCT`, `--burst PCT LEN`, `--reorder PCT`, `--dup PCT`, `--owd US`,
`--jitter US`, or `--nasty` for the preset above.

| Benchmark | Extra options |
| :--- | :--- |
| `zcrudp_stream_bench` | `--bitrate BPS`, `--no-fec`, `--live` (show the window) |
| `zcrudp_pyrowave_bench` | `--target-bytes N` |
| `zcrudp_ternary_bench` | `--psnr`, `--no-rio`, `--no-fec`, `--bitrate BPS`, `--live` |

With `--fec N`, up to N lost fragments per frame are rebuilt from parity. A frame split in
more than 239 fragments reuses coefficient columns, so two losses a multiple
of 239 fragments apart cannot both be rebuilt.

Once Winsock RIO is enabled on a socket, every datagram (data, parity,
delayed packets) goes through RIO: ordinary `WSASendTo` calls were observed
to fail on the connected `WSA_FLAG_REGISTERED_IO` socket these benchmarks use.

## Build (Windows, Clang + CMake)

1. Place `zcrudp` at `../zcrudp` and `tern_vid` at `../tern_vid`.
2. Download `nvEncodeAPI.h` (from `FFmpeg/nv-codec-headers`) into `third_party/nvEncodeAPI.h`.
3. Optional for `zcrudp_pyrowave_bench`: place `Themaister/pyrowave` (with `Granite`, `volk`, `Vulkan-Headers`) in `third_party/pyrowave-master` and build `libpyrowave.a`.
4. Configure & build:
```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j 8
ctest --test-dir build   # fragment/FEC bridge tests, no GPU needed
```
