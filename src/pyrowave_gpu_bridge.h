#ifndef PYROWAVE_GPU_BRIDGE_H
#define PYROWAVE_GPU_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zc_pyro_ctx_s zc_pyro_ctx_t;

int zc_pyro_create(zc_pyro_ctx_t **out_ctx, uint32_t width, uint32_t height,
                   const uint8_t luid[8], bool enable_encoder, bool enable_decoder,
                   char *out_gpu_name, size_t gpu_name_cap);

int zc_pyro_setup_d3d11_enc_interop(zc_pyro_ctx_t *ctx, void *tex_nt_handle,
                                    void *fence_nt_handle, uint32_t tex_w,
                                    uint32_t tex_h);

bool zc_pyro_has_d3d11_enc_interop(const zc_pyro_ctx_t *ctx);

int zc_pyro_setup_d3d11_dec_interop(zc_pyro_ctx_t *ctx, void *y_nt_handle,
                                    void *cb_nt_handle, void *cr_nt_handle,
                                    void *fence_nt_handle, uint32_t width,
                                    uint32_t height);

bool zc_pyro_has_d3d11_dec_interop(const zc_pyro_ctx_t *ctx);

int zc_pyro_encode_gpu(zc_pyro_ctx_t *ctx, uint64_t wait_fence_val,
                       uint64_t signal_fence_val, uint32_t target_bytes,
                       uint8_t *out_buf, size_t out_cap, size_t *out_len);

int zc_pyro_encode_nv12(zc_pyro_ctx_t *ctx, const uint8_t *y_plane,
                        size_t y_stride, const uint8_t *uv_plane,
                        size_t uv_stride, uint32_t target_bytes,
                        uint8_t *out_buf, size_t out_cap, size_t *out_len);

int zc_pyro_decode_gpu(zc_pyro_ctx_t *ctx, const uint8_t *bitstream,
                       size_t len, uint64_t wait_fence_val,
                       uint64_t signal_fence_val);

int zc_pyro_decode_yuv420p(zc_pyro_ctx_t *ctx, const uint8_t *bitstream,
                           size_t len, uint8_t *y_out, size_t y_stride,
                           uint8_t *cb_out, size_t cb_stride, uint8_t *cr_out,
                           size_t cr_stride);

void zc_pyro_destroy(zc_pyro_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PYROWAVE_GPU_BRIDGE_H */
