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
 * Creates a GPU pipeline backed by tern_vid HLSL compute shaders (cs_5_0)
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
