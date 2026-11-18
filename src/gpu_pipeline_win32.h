#ifndef GPU_PIPELINE_WIN32_H
#define GPU_PIPELINE_WIN32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gpu_pipeline_s gpu_pipeline_t;

typedef struct {
  double capture_us;
  double encode_us;
  double net_tx_us;
  double net_rx_us;
  double decode_us;
  double present_us;
  double total_us;
  size_t encoded_bytes;
  bool keyframe;
  bool fec_used;
} frame_timing_t;

typedef struct {
  char adapter_name[128];
  char encoder_name[128];
  char decoder_name[128];
  bool using_desktop_dup;
  bool tearing_supported;
  uint32_t width;
  uint32_t height;
} gpu_pipeline_info_t;

int gpu_pipeline_create(gpu_pipeline_t **out_pipe, uint32_t width,
                        uint32_t height, uint32_t fps, uint32_t bitrate_bps,
                        bool show_window, gpu_pipeline_info_t *out_info);

/**
 * Creates a GPU pipeline backed by zcvid HLSL compute shaders (cs_5_0)
 * for zero-copy VRAM ternary encoding/decoding.
 */
int gpu_pipeline_create_ternary(gpu_pipeline_t **out_pipe, uint32_t width,
                                uint32_t height, uint32_t fps,
                                uint32_t threshold, uint32_t delta_step,
                                uint32_t gamma_q8, uint32_t neutral_y,
                                bool show_window, gpu_pipeline_info_t *out_info);

/** Optional PSNR (luma Y) check between captured VRAM texture and decoded VRAM texture. */
int gpu_pipeline_ternary_compute_psnr(gpu_pipeline_t *pipe, double *out_psnr_db);

/**
 * Counts pixels whose encoder reference word (Y/Cb/Cr/step) differs from the
 * decoder reconstruction of the last decoded frame. Always 0 on a lossless
 * link; anything else means the two HLSL closed loops have diverged.
 */
int gpu_pipeline_ternary_ref_mismatch(gpu_pipeline_t *pipe, uint64_t *out_mismatch_px);

void gpu_pipeline_set_bitrate(gpu_pipeline_t *pipe, uint32_t bitrate_bps);

void gpu_pipeline_force_synth(gpu_pipeline_t *pipe, bool enable);

void gpu_pipeline_ternary_request_keyframe(gpu_pipeline_t *pipe);

/**
 * Makes the ternary encoder emit its compact bitstream: records sorted and
 * gap-coded, vectors run-length coded. Lossless, roughly half the bytes, and
 * the rate control spends the saving on more records. The decoder recognises
 * either format on its own. Off by default: the raw layout
 * [u32 record_words][u32 mv_bytes][mvs][records] is what the benches slice.
 */
void gpu_pipeline_ternary_set_packed(gpu_pipeline_t *pipe, bool enable);

/**
 * With the compact bitstream on, also range-codes the records (adaptive
 * contexts, see tern_ec_pack_records): still lossless, and a record then
 * costs what its trits are worth, so the same bitrate carries about twice as
 * many. The coder takes a bounded share of the frame period: records it has
 * no time for follow in the plain compact layout, in the same frame.
 */
void gpu_pipeline_ternary_set_entropy(gpu_pipeline_t *pipe, bool enable);

/**
 * Codes intra (rolling refresh, disocclusion) with a 4x4 integer transform
 * instead of raw samples. Needs a decoder built with CSITrans: streams that
 * use it are marked in the frame header (packed layouts gain a 9th class).
 */
void gpu_pipeline_ternary_set_transform(gpu_pipeline_t *pipe, bool enable);

/**
 * With the transform on, also transform-codes blocks whose motion-compensated
 * residual is large (zoom, rotation, motion inside a tile) instead of leaving
 * them to converge over several frames of ternary deltas.
 */
void gpu_pipeline_ternary_set_transform_inter(gpu_pipeline_t *pipe, bool enable);

/**
 * Tile vectors in quarter pels (bilinear motion compensation, integer and
 * bit-exact on both sides). Marked in the stream by a byte after the
 * vectors; a decoder built before it would apply them as whole pels.
 */
void gpu_pipeline_ternary_set_qpel(gpu_pipeline_t *pipe, bool enable);

/**
 * Ordered (Bayer 4x4) dither instead of rounding when the rolling refresh
 * falls back to 6-bit samples: no banding on gradients, at the price of fine
 * noise (about 2.6 dB of PSNR on the bench pattern). Encoder only, off by
 * default.
 */
void gpu_pipeline_ternary_set_dither(gpu_pipeline_t *pipe, bool enable);

/** Capture hint counters since creation: desktop frames that carried move /
 *  dirty rectangles, move rectangles, tiles hinted as moved, tiles hinted as
 *  unchanged. */
void gpu_pipeline_ternary_hint_stats(gpu_pipeline_t *pipe, uint64_t *frames, uint64_t *move_rects,
                                     uint64_t *moved_tiles, uint64_t *static_tiles);

/**
 * Record words per frame this pipeline can range-decode within its share of
 * the frame period, from what decoding has cost it so far. A receiver sends
 * it to the sender, which passes it to gpu_pipeline_ternary_set_entropy_limit
 * so that it never range-codes more than the receiver can follow (0 = no
 * limit). Records past the limit still travel, in the plain compact layout.
 */
uint32_t gpu_pipeline_ternary_entropy_budget(gpu_pipeline_t *pipe);
void gpu_pipeline_ternary_set_entropy_limit(gpu_pipeline_t *pipe, uint32_t record_words);

void gpu_pipeline_destroy(gpu_pipeline_t *pipe);

/** Captures (or GPU-renders) one frame in VRAM and encodes it to H.264. */
int gpu_pipeline_capture_and_encode(gpu_pipeline_t *pipe, uint32_t frame_idx,
                                    const uint8_t **out_bitstream,
                                    size_t *out_len, bool *out_keyframe,
                                    double *out_cap_us, double *out_enc_us);

/** Decodes H.264 bitstream in VRAM (D3D11VA) and presents via FLIP_DISCARD. */
int gpu_pipeline_decode_and_present(gpu_pipeline_t *pipe,
                                    const uint8_t *bitstream, size_t len,
                                    double *out_dec_us,
                                    double *out_present_us);

/** Pumps Win32 window messages; returns false if user closed the window. */
bool gpu_pipeline_pump_events(gpu_pipeline_t *pipe);

/** Optional FILE* for mirroring GPU pipeline error logs to a log file. */
void gpu_pipeline_set_log_file(void *stdio_file);

uint64_t qpc_now_us(void);

#ifdef __cplusplus
}
#endif

#endif /* GPU_PIPELINE_WIN32_H */
