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

## Build (Windows, Clang + CMake)

1. Place `zcrudp` at `../zcrudp`.
2. Download `nvEncodeAPI.h` (from `FFmpeg/nv-codec-headers`) into `third_party/nvEncodeAPI.h`.
3. Optional for `zcrudp_pyrowave_bench`: place `Themaister/pyrowave` (with `Granite`, `volk`, `Vulkan-Headers`) in `third_party/pyrowave-master` and build `libpyrowave.a`.
4. Configure & build:
```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j 8
```
