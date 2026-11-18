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

Third prototype: the [`zcvid`](../zcvid) compute shaders (`cs_5_0`)
encode and decode YCbCr in VRAM with per-tile motion vectors. Motion vectors
travel on the FEC-protected path, delta records on the unprotected one.

`--psnr` reports the luma PSNR and a closed-loop check: the number of pixels
whose encoder reference differs from the decoder output. It must be `0` on a
clean link; under simulated loss the two diverge until the leaky integrator
and the rolling Intra-Refresh bring them back.

The source is the desktop when Desktop Duplication is available, otherwise a
GPU-rendered test pattern, so bitrate depends on what is on screen. A desktop
of another size or pixel format than the stream (a game that switched the
display mode, an HDR desktop) is scaled and converted on the GPU, aspect ratio
kept with black bars; capture restarts by itself after a mode switch.

### Bitstream

The shaders emit 8-byte records. `gpu_pipeline_ternary_set_packed` repacks
them losslessly (sorted, gap-coded), and `gpu_pipeline_ternary_set_entropy`
adds an adaptive range coder on top (the one in `zcvid/include/ternary_entropy.h`):
trits are coded under the context of their left and upper neighbours, intra
samples as the error of a left/up/up-left prediction. Still lossless, and the
rate control spends the saving on more records. Measured on a 1920x1080 game
frame panned by (7, 5) px per frame at 120 fps:

| Target | Packed only | Packed + range coder |
| :--- | :--- | :--- |
| 10 Mbps | 23.6 dB at 12.5 Mbps | 31.6 dB at 8.5 Mbps |
| 25 Mbps | 31.2 dB at 20.9 Mbps | 38.5 dB at 13.5 Mbps |
| 50 Mbps | 37.5 dB at 34.4 Mbps | 39.5 dB at 20.0 Mbps |
| 400 Mbps | 40.7 dB at 128 Mbps | 40.9 dB at 40.0 Mbps |

The real bitrate stays under the target once the codec reaches its quality
floor (threshold 6).

A frame is cut into up to 16 chunks coded on as many threads (every logical
processor but one), about 1 ms each way at 25 Mbps. The coder is bounded in
time, not in size: each side measures what a record word costs it and spends
at most 30 % of the frame period. Records beyond that budget are not dropped
and the frame does not fall back as a whole: they follow, in the same frame,
in the packed layout. On the 400 Mbps pan above, the picture is the same
40.9 dB whether the coder runs on 23 threads (40.0 Mbps), on 1 thread
(40.9 Mbps) or with a budget of 20 000 record words (88.0 Mbps): a slower
machine pays in bitrate, never in quality. The decoder recognises every
layout from the frame header.

`zcrudp_stream` turns the range coder on only after the receiver has
announced it can read it (`ZCEC` on the control port, port + 1), so a sender
keeps working with a receiver built before the coder existed. The notice
carries the receiver's own decoding budget, which the sender never exceeds.

### Motion: quarter pel and transform intra

Tile vectors can be in quarter pels (`gpu_pipeline_ternary_set_qpel`). The
reference is then interpolated with a separable 4-tap filter (the HEVC chroma
taps), in integers on both sides so encoder and decoder stay bit-exact on any
GPU. Bilinear interpolation was tried first: its blur builds up frame after
frame through the closed loop, and cost 1 to 2 dB against the 4-tap filter.
The motion search refines its whole-pel winner to the half pel, then the
quarter pel.

Blocks that need intra (disocclusion) can be coded with the 4x4 integer
transform of H.264 instead of raw samples (`gpu_pipeline_ternary_set_transform`,
`CSIntraT` / `CSITrans`). Rolling refresh keeps the exact sample intra: making
it lossy costs more than it saves.

The same transform also codes the residual against the motion prediction
(`gpu_pipeline_ternary_set_transform_inter`): a hybrid mode where any 4x4
block whose coded residual brings it clearly closer to the source (by a
quarter of its error at least) is transform-coded, at a qp that follows the
rate control, and the ternary deltas refine everything else. Blocks where
the transform would not help (too coarse a qp) are left to the deltas, so
none can get stuck on a bad prediction.

Pans by a whole number of pixels per frame, which earlier tables used, hide
what sub-pixel motion costs; a real camera rarely moves that way. Same game
frame, 1920x1080 at 120 fps, 25 Mbps target, packed + range-coded; fractional
pans are rendered with bicubic resampling so every frame is equally sharp:

| Motion | Whole pel, deltas only | Quarter pel + transform intra | + hybrid (default) |
| :--- | :--- | :--- | :--- |
| pan (2.5, 1.25) px/frame | 23.8 dB | 29.1 dB | 33.0 dB |
| pan (0.3, 4.6) px/frame | 22.7 dB | 28.1 dB | 32.7 dB |
| zoom 0.2 %/frame | 26.2 dB | 32.2 dB | 36.7 dB |
| pan (0, 24) px/frame | 31.8 dB | 34.7 dB | 39.6 dB |
| pan (7, 5) px/frame | 41.7 dB | 43.5 dB | 47.5 dB |

All at the 25 Mbps target. The deltas-only codec, given 165 Mbps, reaches
32.1 dB on the (2.5, 1.25) pan and 36.7 dB on the zoom (at 157 Mbps): what
the default now does at 25 Mbps, about a sixth of the bitrate.

What makes the hybrid work is its own qp loop. Tied to the delta threshold,
the transform went as coarse as the deltas whenever they saturated and gave
up the blocks it codes best; now its qp only rises when a frame asks for more
than twice the budget, and falls as soon as it fits. Blocks are taken from a
different starting point every frame, so a full budget never starves the
same rows. Bench pattern: 49.1 dB at 35.7 Mbps (45.1 dB at 31.2 Mbps without
the new modes), 4K / 50 Mbps: 42.9 dB, `--nasty`: 46.7 dB, all 0 mismatched
pixels. The pan (0, 24) dips to 31.4 dB on some frames (39.6 dB average).

Against hardware H.264 (this repo's own NVENC path: P1 or P7, ultra-low
latency, CBR, one-frame VBV, IDR every 2 s), same sequences, 25 Mbps, both
measured against the source in luma. NVENC's decoded luma plane is compared
with the source converted the way NVENC converts it (fitted on a 300 Mbps
still: BT.709 limited range, 61.5 dB at 300 Mbps), in full-scale units:

| Motion | NVENC H.264 P1 | NVENC P7 | Ternary (default) |
| :--- | :--- | :--- | :--- |
| pan (2.5, 1.25) px/frame | 36.3 dB (min 27.3) | 37.0 dB | 33.5 dB (min 30.6) |
| pan (0.3, 4.6) px/frame | 36.8 dB | 37.5 dB | 33.2 dB |
| zoom 0.2 %/frame | 39.8 dB | 40.3 dB | 37.4 dB |
| pan (0, 24) px/frame | 47.5 dB at 33.9 Mbps | 48.0 dB at 31.0 Mbps | 41.8 dB at 24.8 Mbps |
| pan (7, 5) px/frame | 41.7 dB (min 27.5) | 42.0 dB | 47.2 dB |
| still | 46.2 dB at 20 Mbps | 46.3 dB | 49.1 dB at 1.3 Mbps |

H.264 stays 2.9 to 4.3 dB ahead on camera motion (the vertical pan is not
comparable: NVENC overshoots its rate by a quarter). The ternary codec wins
on whole-pel motion and still pictures, never dips (every NVENC IDR frame
falls to about 27.5 dB), and codes chroma at full resolution where H.264 has
4:2:0. Coarser chroma in the transform (qp + 15 instead of + 3) lifts the
vertical pan to 46.4 dB luma but costs 2 to 6 dB of chroma elsewhere, so it
is not the default. Coding every residual with the transform and no ternary
deltas at all was tried too: 0.1 to 0.8 dB worse than the hybrid. Transform
coefficients are range-coded block by block (gap to the previous coded
block in blocks, per-plane presence, qp and count, zigzag steps and levels
under context): a scattered block's position cost dropped from about 19
bits, the transform took about ten times more blocks, and quality rose by
0.5 to 1.3 dB. A block is only transform-coded when that removes a quarter
of its error and at least half a level per pixel, so converged content is
not recoded every frame. A smaller inter rounding offset (1/6, 1/4 as in
H.264) lost 0.1 to 0.7 dB. What is left is 4x4 blocks only, no intra
prediction and one vector per 32x32 tile.

Tried towards a full hybrid codec, measured, and not kept (25 Mbps, same
scenes, all bit-exact):

- 8x8 luma transform (H.264 High-profile core, four 4x4 per chroma plane):
  0.1 to 0.9 dB worse. The residual left after quarter-pel prediction is fine
  detail on edges, where 8x8 brings nothing, and larger disocclusion blocks
  recode pixels that were already right. H.264 picks 4x4 or 8x8 per block.
- CABAC-style significance map (per-position "nonzero" / "last" decisions,
  levels in reverse with a "greater than 1" flag): 0.3 to 0.7 dB worse than
  the count + zigzag-step coding, with about 4 sparse coefficients a block.
- HEVC 8-tap interpolation again, now that the transform carries most of the
  residual: still 1.3 to 1.9 dB worse than the 4-tap filter.

On a fractional pan the codec codes about 6 % of the 4x4 blocks per frame, at
about 22 bits each; H.264 touches every macroblock for the same budget. The
remaining difference is in how blocks are chosen and signalled, not in
transform size, coefficient coding or interpolation.

Fractional pans still saturate the rate control at 25 Mbps (threshold 14,
half horizontal resolution for the deltas): interpolation leaves a small
residual on every edge of every frame. Disabling the half-resolution fallback
was tried and loses 0.6 to 2.6 dB.

End-to-end p50 on the bench pattern goes from 0.96 to 1.31 ms (search
refinement and the transform pass); CPU decoding drops to 0.5-0.7 ms per
frame on camera motion, fewer delta records being left to unpack. When saturated (threshold at its maximum), frames are capped at the
target instead of 25 % above it.

`zcrudp_stream` enables them only when the receiver lists them in its `ZCEC`
notice (bit 0: quarter pel, bit 1: transform blocks, intra and inter). Start
the receiver first: it compiles its shaders for a few seconds before it listens. Streams that use them
are marked (a byte after the vectors, a header flag for the 9th record class),
so the plain layouts stay readable by older decoders.

### Reassembly, ordering and loss

The receiver reassembles up to 32 frames side by side. With a single slot,
the first datagram of a newer frame threw away an older one that was a
datagram short, so reordering or jitter alone, without any loss, was enough
to wreck the picture.

`zcrudp_stream` retransmits. The sender keeps its last 64 frames; the
receiver hands frames to the decoder in order, and when one is short (or
missing altogether) it asks for the fragments again (`ZCNK` on the control
port: after 3 ms, then every 10 ms) while later frames wait behind it, 100 ms
at most. Only a frame given up on after that triggers the 16-frame intra
resync (`ZCSY`). `--sim-loss PCT` and `--sim-burst PCT LEN` make the sender
drop its own datagrams to try it. 1200 frames at 120 fps over loopback,
1 parity shard:

| Sender drops | Frames lost | Resyncs |
| :--- | :--- | :--- |
| nothing | 0 | 0 |
| `--sim-loss 5` (5 % of datagrams) | 0 | 0 |
| `--sim-loss 5 --sim-burst 8 3` (about 31 %) | 0 | 0 |

`zcrudp_ternary_bench` under impairment sends the records as small
FEC-protected slices and sends again whatever is still missing, 8 times at
most (`--resend N`, 0 to disable). A resend does not wait for datagrams the
link merely delayed: past the ordinary transit time the missing slices go out
again and the first copy to arrive is used. The bench knows what its receiver
is missing without `ZCNK` datagrams travelling back, so its latency leaves out
that return trip; `zcrudp_stream` does the real exchange.

300 frames of the test pattern, 1 parity shard. The bitrate is everything put
on the wire: fragment headers, parity and resends included.

| Link | Y-PSNR | Mismatched pixels (last frame) | Bitrate | Latency p50 |
| :--- | :--- | :--- | :--- | :--- |
| clean | 42.0 dB | 0 | 22.7 Mbps | 1.0 ms |
| `--reorder 15` | 41.9 dB | 0 | 38.6 Mbps | 2.6 ms |
| `--loss 5` | 41.9 dB | 0 | 34.1 Mbps | 1.2 ms |
| `--nasty` (about 31 % of datagrams lost) | 42.0 dB | 0 | 40.3 Mbps | 15.6 ms |
| `--nasty --fec 3` | 41.9 dB | 0 | 41.5 Mbps | 10.3 ms |
| `--nasty --resend 0` | 17.2 dB | 347 027 | 28.1 Mbps | 24.9 ms |

Protection is not free: one parity shard per 1680-byte slice adds about half
the payload as soon as any impairment is configured, and early resends
duplicate part of what reordering only delayed. Under `--nasty` a third of the
datagrams are lost and every one spends 2 to 5 ms in transit.

At 3840x2160 the same pattern gives 38.1 dB with 0 mismatched pixels. The
4-pixel intra record addresses pixel pairs on 22 bits, which reaches 8.3 Mpx;
it used to address pixels and wrapped past 4.19 Mpx (11.0 dB at 4K).

### Live session details

- **Shader cache.** Compiled shaders are kept in
  `%LOCALAPPDATA%\zcrudp\shaders`, keyed by a hash of the source, entry point,
  profile and flags. The first start after a shader change compiles (about
  2.5 s); later starts load the bytecode in about 4 ms. Creating the ternary
  pipeline then takes about 190 ms, 145 of them in `D3D11CreateDevice`.
- **Adaptive FEC.** `zcrudp_stream` sends no parity while the receiver reports
  no loss; the first retransmission request or resync turns the `--fec`
  parity on, and 2 s without one turns it off again. The rate control is
  given what is left of the link once headers, parity and retransmissions are
  paid for (`zc_rate_budget_update`), so the wire stays at or under
  `--bitrate`.
- **Capture hints.** With desktop duplication, the move and dirty rectangles
  DXGI reports become per-tile hints: tiles nothing touched skip the motion
  search, tiles inside a moved rectangle get its vector as a candidate. On
  Windows 11 (build 26300) DXGI reports no move rectangles at all, even for a
  window dragged with the mouse, only dirty ones: the unchanged-tile hint
  works, the moved-tile one is never exercised (the motion search finds such
  vectors anyway).
- **Pacing.** The pacer runs at twice `--bitrate` (up to 4x when the delay
  feedback allows): at exactly the bitrate, any frame larger than average
  queued the frames behind it. The delay fed back to it is measured from the
  last datagram of a frame (not from before the pacer) and never from a frame
  that needed a retransmission: counting either made the pacer cut its own
  rate on every large frame, down to its minimum. Bench at 25 Mbps,
  120 fps: TX p50 / p99 from 5.3 / 15 ms to 0.17 / 0.24 ms, end to end
  0.83 / 1.37 ms; with 5 % loss and 2 ms ± 0.5 ms delay, end to end
  3.0 / 7.0 ms (was 6.3 / 12.3 ms).
- **End of session.** The sender keeps answering retransmission requests for
  200 ms after its last frame, and sends that frame's last fragment again
  (a tail probe): a last frame that lost every datagram leaves no gap for the
  receiver to notice.
- **No allocation after start-up.** Host buffers (record sort, range coder,
  decoder, capture metadata, TX history, RX slots) are sized and touched at
  creation; growing one later still works and is reported once on stderr.
- **Display deblocking.** `CSYCbCrToRGBA` smooths luma steps of at most 6
  across 4x4 block edges, after the closed loop (no drift possible). Effect
  on PSNR: 0 to +0.08 dB.
- **Dither** (`gpu_pipeline_ternary_set_dither`, off by default): ordered 4x4
  dither on the 6-bit rolling refresh. Removes banding on gradients, costs
  about 2.6 dB of PSNR on the bench pattern.

### Motion bench

`ternary_motion_bench` (built with the rest, needs a D3D11 GPU) moves a
picture through the encoder and decoder without the network: whole-pixel and
sub-pixel pans, zoom, PSNR, bitrate, timings, reference mismatch and decoder
fuzzing. Its header lists the options; the codec settings `zcrudp_stream`
uses are `PACK=1 EC=1 QPEL=1 TQ=1 TQI=1`:

```powershell
$env:PACK=1; $env:EC=1; $env:QPEL=1; $env:TQ=1; $env:TQI=1; $env:FPAN="2.5,1.25"
.\build\ternary_motion_bench.exe - 0 0 25    # generated picture, 25 Mbps
```

The ternary bench's simulated losses have no resync: a slice lost beyond
the resends leaves the last frame a few pixels off the encoder (seen once,
17 pixels, under `--nasty`). `zcrudp_stream` resyncs and lost no frame in
the same conditions.

## Benchmark options

All three benchmarks accept `--frames N`, `--width W`, `--height H`,
`--fec N` (0-4 parity shards per frame) and the impairment simulator:
`--loss PCT`, `--burst PCT LEN`, `--reorder PCT`, `--dup PCT`, `--owd US`,
`--jitter US`, or `--nasty` for the preset above.

| Benchmark | Extra options |
| :--- | :--- |
| `zcrudp_stream_bench` | `--bitrate BPS`, `--no-fec`, `--live` (show the window) |
| `zcrudp_pyrowave_bench` | `--target-bytes N` |
| `zcrudp_ternary_bench` | `--psnr`, `--synth`, `--no-rio`, `--no-fec`, `--resend N`, `--no-qpel`, `--no-transform`, `--bitrate BPS`, `--live` |

With `--fec N`, up to N lost fragments per frame are rebuilt from parity. A frame split in
more than 239 fragments reuses coefficient columns, so two losses a multiple
of 239 fragments apart cannot both be rebuilt.

Once Winsock RIO is enabled on a socket, every datagram (data, parity,
delayed packets) goes through RIO: ordinary `WSASendTo` calls were observed
to fail on the connected `WSA_FLAG_REGISTERED_IO` socket these benchmarks use.

## Build (Windows, Clang + CMake)

1. Place `zcrudp` at `../zcrudp` and `zcvid` at `../zcvid`.
2. Download `nvEncodeAPI.h` (from `FFmpeg/nv-codec-headers`) into `third_party/nvEncodeAPI.h`.
3. Optional for `zcrudp_pyrowave_bench`: place `Themaister/pyrowave` (with `Granite`, `volk`, `Vulkan-Headers`) in `third_party/pyrowave-master` and build `libpyrowave.a`.
4. Configure & build:
```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j 8
ctest --test-dir build   # fragment/FEC bridge tests, no GPU needed
```
