#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vulkan/vulkan.h>
#include "pyrowave.h"
#include "pyrowave_gpu_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

struct zc_pyro_ctx_s {
  pyrowave_device dev = nullptr;
  pyrowave_encoder enc = nullptr;
  pyrowave_decoder dec = nullptr;
  pyrowave_sync_object shared_sync = nullptr;
  pyrowave_image enc_img = nullptr;
  pyrowave_image_view enc_view = {};
  bool has_enc_interop = false;
  pyrowave_image dec_imgs[3] = {nullptr, nullptr, nullptr};
  pyrowave_gpu_buffers dec_buffers = {};
  bool has_dec_interop = false;
  uint32_t width = 0;
  uint32_t height = 0;
};

int zc_pyro_create(zc_pyro_ctx_t **out_ctx, uint32_t width, uint32_t height,
                   const uint8_t luid[8], bool enable_encoder, bool enable_decoder,
                   char *out_gpu_name, size_t gpu_name_cap) {
  if (!out_ctx || width < 16 || height < 16) return -1;
  /* Pyrowave 4:2:0 requires even dimensions. */
  width &= ~1u;
  height &= ~1u;

  zc_pyro_ctx_t *c = new (std::nothrow) zc_pyro_ctx_t();
  if (!c) return -2;
  c->width = width;
  c->height = height;

  pyrowave_result res = PYROWAVE_ERROR_GENERIC;
  if (luid) {
    pyrowave_luid pl = {};
    memcpy(pl.luid, luid, 8);
    res = pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, &pl,
                                           VK_QUEUE_GLOBAL_PRIORITY_MEDIUM, &c->dev);
  }
  if (res != PYROWAVE_SUCCESS || !c->dev) {
    res = pyrowave_create_default_device(&c->dev);
  }
  if (res != PYROWAVE_SUCCESS || !c->dev) {
    fprintf(stderr, "[PYROWAVE] Failed to create Vulkan device (rc=%d)\n", (int)res);
    delete c;
    return -3;
  }

  if (out_gpu_name && gpu_name_cap > 0) {
    snprintf(out_gpu_name, gpu_name_cap, "Pyrowave Vulkan Wavelet GPU");
  }

  if (enable_encoder) {
    pyrowave_encoder_create_info ei = {};
    ei.device = c->dev;
    ei.width = (int)width;
    ei.height = (int)height;
    ei.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    res = pyrowave_encoder_create(&ei, &c->enc);
    if (res != PYROWAVE_SUCCESS || !c->enc) {
      fprintf(stderr, "[PYROWAVE] pyrowave_encoder_create failed (rc=%d)\n", (int)res);
      zc_pyro_destroy(c);
      return -4;
    }
  }

  if (enable_decoder) {
    pyrowave_decoder_create_info di = {};
    di.device = c->dev;
    di.width = (int)width;
    di.height = (int)height;
    di.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    di.fragment_path = pyrowave_decoder_device_prefers_fragment_path(c->dev);
    res = pyrowave_decoder_create(&di, &c->dec);
    if (res != PYROWAVE_SUCCESS || !c->dec) {
      fprintf(stderr, "[PYROWAVE] pyrowave_decoder_create failed (rc=%d)\n", (int)res);
      zc_pyro_destroy(c);
      return -5;
    }
  }

  *out_ctx = c;
  return 0;
}

static int ensure_d3d11_sync(zc_pyro_ctx_t *ctx, void *fence_nt_handle) {
  if (ctx->shared_sync) return 0;
  if (!fence_nt_handle) return -1;
  HANDLE dup_fence = nullptr;
  HANDLE self = GetCurrentProcess();
  if (!DuplicateHandle(self, (HANDLE)fence_nt_handle, self, &dup_fence, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    return -4;
  }
  pyrowave_sync_object_create_info si = {};
  si.device = ctx->dev;
  si.external_handle = (pyrowave_os_handle)dup_fence;
  si.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
  si.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
  pyrowave_result res = pyrowave_sync_object_create(&si, &ctx->shared_sync);
  if (res != PYROWAVE_SUCCESS || !ctx->shared_sync) {
    ctx->shared_sync = nullptr;
    return -7;
  }
  return 0;
}

int zc_pyro_setup_d3d11_enc_interop(zc_pyro_ctx_t *ctx, void *tex_nt_handle,
                                    void *fence_nt_handle, uint32_t tex_w,
                                    uint32_t tex_h) {
  if (!ctx || !ctx->dev || !ctx->enc || !tex_nt_handle || !fence_nt_handle) {
    return -1;
  }
  if (!pyrowave_device_confirm_interop_support(ctx->dev)) {
    return -2;
  }

  /* Duplicate handle because pyrowave takes ownership and closes it on import. */
  HANDLE dup_tex = nullptr;
  HANDLE self = GetCurrentProcess();
  if (!DuplicateHandle(self, (HANDLE)tex_nt_handle, self, &dup_tex, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    return -3;
  }

  VkImageCreateInfo ici = {};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.extent = {tex_w, tex_h, 1};
  ici.format = VK_FORMAT_B8G8R8A8_UNORM;
  ici.mipLevels = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.arrayLayers = 1;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  pyrowave_image_create_info ii = {};
  ii.device = ctx->dev;
  ii.external_handle = (pyrowave_os_handle)dup_tex;
  ii.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
  ii.image_create_info = &ici;

  pyrowave_result res = pyrowave_image_create(&ii, &ctx->enc_img);
  if (res != PYROWAVE_SUCCESS || !ctx->enc_img) {
    return -5;
  }

  res = pyrowave_image_get_image_view(ctx->enc_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_USAGE_SAMPLED_BIT, &ctx->enc_view);
  if (res != PYROWAVE_SUCCESS) {
    pyrowave_image_destroy(ctx->enc_img);
    ctx->enc_img = nullptr;
    return -6;
  }

  int sync_rc = ensure_d3d11_sync(ctx, fence_nt_handle);
  if (sync_rc != 0) {
    pyrowave_image_destroy(ctx->enc_img);
    ctx->enc_img = nullptr;
    return sync_rc;
  }

  ctx->has_enc_interop = true;
  return 0;
}

bool zc_pyro_has_d3d11_enc_interop(const zc_pyro_ctx_t *ctx) {
  return ctx && ctx->has_enc_interop;
}

int zc_pyro_setup_d3d11_dec_interop(zc_pyro_ctx_t *ctx, void *y_nt_handle,
                                    void *cb_nt_handle, void *cr_nt_handle,
                                    void *fence_nt_handle, uint32_t width,
                                    uint32_t height) {
  if (!ctx || !ctx->dev || !ctx->dec || !y_nt_handle || !cb_nt_handle ||
      !cr_nt_handle || !fence_nt_handle || width < 16 || height < 16) {
    return -1;
  }
  if (!pyrowave_device_confirm_interop_support(ctx->dev)) {
    return -2;
  }

  int sync_rc = ensure_d3d11_sync(ctx, fence_nt_handle);
  if (sync_rc != 0) {
    return sync_rc;
  }

  void *handles[3] = {y_nt_handle, cb_nt_handle, cr_nt_handle};
  HANDLE self = GetCurrentProcess();
  for (int i = 0; i < 3; ++i) {
    uint32_t pw = (i == 0) ? width : (width >> 1);
    uint32_t ph = (i == 0) ? height : (height >> 1);

    HANDLE dup_tex = nullptr;
    if (!DuplicateHandle(self, (HANDLE)handles[i], self, &dup_tex, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
      for (int j = 0; j < i; ++j) {
        if (ctx->dec_imgs[j]) {
          pyrowave_image_destroy(ctx->dec_imgs[j]);
          ctx->dec_imgs[j] = nullptr;
        }
      }
      memset(&ctx->dec_buffers, 0, sizeof(ctx->dec_buffers));
      ctx->has_dec_interop = false;
      return -3;
    }

    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent = {pw, ph, 1};
    ici.format = VK_FORMAT_R8_UNORM;
    ici.mipLevels = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.arrayLayers = 1;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    pyrowave_image_create_info ii = {};
    ii.device = ctx->dev;
    ii.external_handle = (pyrowave_os_handle)dup_tex;
    ii.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    ii.image_create_info = &ici;

    pyrowave_result res = pyrowave_image_create(&ii, &ctx->dec_imgs[i]);
    if (res != PYROWAVE_SUCCESS || !ctx->dec_imgs[i]) {
      ctx->dec_imgs[i] = nullptr;
      for (int j = 0; j < i; ++j) {
        if (ctx->dec_imgs[j]) {
          pyrowave_image_destroy(ctx->dec_imgs[j]);
          ctx->dec_imgs[j] = nullptr;
        }
      }
      memset(&ctx->dec_buffers, 0, sizeof(ctx->dec_buffers));
      ctx->has_dec_interop = false;
      return -5;
    }

    res = pyrowave_image_get_image_view(ctx->dec_imgs[i], VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_IMAGE_USAGE_STORAGE_BIT,
                                        &ctx->dec_buffers.planes[i]);
    if (res != PYROWAVE_SUCCESS) {
      for (int j = 0; j <= i; ++j) {
        if (ctx->dec_imgs[j]) {
          pyrowave_image_destroy(ctx->dec_imgs[j]);
          ctx->dec_imgs[j] = nullptr;
        }
      }
      memset(&ctx->dec_buffers, 0, sizeof(ctx->dec_buffers));
      ctx->has_dec_interop = false;
      return -6;
    }
  }

  ctx->has_dec_interop = true;
  return 0;
}

bool zc_pyro_has_d3d11_dec_interop(const zc_pyro_ctx_t *ctx) {
  return ctx && ctx->has_dec_interop;
}

static int packetize_single(zc_pyro_ctx_t *ctx, uint32_t target_bytes,
                            uint8_t *out_buf, size_t out_cap, size_t *out_len) {
  (void)target_bytes;
  if (out_cap < 2048) return -10;
  size_t need_pkts = 0;
  pyrowave_result res = pyrowave_encoder_compute_num_packets(ctx->enc, out_cap, &need_pkts);
  if (res != PYROWAVE_SUCCESS || need_pkts != 1) {
    return -10;
  }
  pyrowave_packet pkt = {};
  size_t npkts = 0;
  res = pyrowave_encoder_packetize(ctx->enc, &pkt, out_cap, &npkts, out_buf, out_cap);
  if (res != PYROWAVE_SUCCESS || npkts == 0 || pkt.size == 0) {
    return -11;
  }
  if (pkt.offset != 0) {
    memmove(out_buf, out_buf + pkt.offset, pkt.size);
  }
  *out_len = pkt.size;
  return 0;
}

int zc_pyro_encode_gpu(zc_pyro_ctx_t *ctx, uint64_t wait_fence_val,
                       uint64_t signal_fence_val, uint32_t target_bytes,
                       uint8_t *out_buf, size_t out_cap, size_t *out_len) {
  if (!ctx || !ctx->enc || !ctx->has_enc_interop || !ctx->shared_sync ||
      !out_buf || !out_len) {
    return -1;
  }
  pyrowave_gpu_external_reference ext_ref = {};
  ext_ref.image = ctx->enc_img;
  ext_ref.queue_family_index = VK_QUEUE_FAMILY_EXTERNAL;

  VkSemaphore sem = pyrowave_sync_object_get_semaphore(ctx->shared_sync);

  pyrowave_gpu_sync_operation acquire = {};
  acquire.sync.semaphore = sem;
  acquire.sync.value = wait_fence_val;
  acquire.num_images = 1;
  acquire.images = &ext_ref;

  pyrowave_gpu_sync_operation release = {};
  release.sync.semaphore = sem;
  release.sync.value = signal_fence_val;
  /* Hand the shared D3D11 texture back to the external (D3D11) queue family:
   * every frame acquires it from VK_QUEUE_FAMILY_EXTERNAL, so it must be
   * released symmetrically or the next acquire is invalid per the Vulkan spec. */
  release.num_images = 1;
  release.images = &ext_ref;

  pyrowave_scaled_encode_info si = {};
  si.view = ctx->enc_view;
  si.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  si.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  si.intermediate_plane_format = VK_FORMAT_R8_UNORM;
  si.ycbcr_chroma_midpoint = 128.0f / 255.0f;
  si.ycbcr_range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

  pyrowave_rate_control rc = {};
  rc.maximum_bitstream_size = target_bytes;

  pyrowave_result res = pyrowave_encoder_encode_gpu_scaled(ctx->enc, &acquire,
                                                           &release, &si, &rc);
  if (res != PYROWAVE_SUCCESS) {
    return -2;
  }
  return packetize_single(ctx, target_bytes, out_buf, out_cap, out_len);
}

int zc_pyro_encode_nv12(zc_pyro_ctx_t *ctx, const uint8_t *y_plane,
                        size_t y_stride, const uint8_t *uv_plane,
                        size_t uv_stride, uint32_t target_bytes,
                        uint8_t *out_buf, size_t out_cap, size_t *out_len) {
  if (!ctx || !ctx->enc || !y_plane || !uv_plane || !out_buf || !out_len) {
    return -1;
  }
  pyrowave_cpu_buffer cb = {};
  cb.data[0] = const_cast<uint8_t *>(y_plane);
  cb.data[1] = const_cast<uint8_t *>(uv_plane);
  cb.row_stride_in_bytes[0] = y_stride;
  cb.row_stride_in_bytes[1] = uv_stride;
  cb.plane_size_in_bytes[0] = y_stride * ctx->height;
  cb.plane_size_in_bytes[1] = uv_stride * (ctx->height / 2);
  cb.width = (int)ctx->width;
  cb.height = (int)ctx->height;
  cb.format = PYROWAVE_CPU_BUFFER_FORMAT_NV12;

  pyrowave_rate_control rc = {};
  rc.maximum_bitstream_size = target_bytes;

  pyrowave_result res = pyrowave_encoder_encode_cpu(ctx->enc, &cb, &rc);
  if (res != PYROWAVE_SUCCESS) {
    return -2;
  }
  return packetize_single(ctx, target_bytes, out_buf, out_cap, out_len);
}

int zc_pyro_decode_gpu(zc_pyro_ctx_t *ctx, const uint8_t *bitstream,
                       size_t len, uint64_t wait_fence_val,
                       uint64_t signal_fence_val) {
  if (!ctx || !ctx->dec || !ctx->has_dec_interop || !ctx->shared_sync ||
      !bitstream || len == 0) {
    return -1;
  }
  pyrowave_decoder_clear(ctx->dec);
  pyrowave_result res = pyrowave_decoder_push_packet(ctx->dec, bitstream, len);
  if (res != PYROWAVE_SUCCESS) {
    return -2;
  }
  if (!pyrowave_decoder_decode_is_ready(ctx->dec, true)) {
    return -3;
  }

  VkSemaphore sem = pyrowave_sync_object_get_semaphore(ctx->shared_sync);
  pyrowave_gpu_external_reference acquire_ref[3] = {
      {ctx->dec_imgs[0], VK_QUEUE_FAMILY_IGNORED},
      {ctx->dec_imgs[1], VK_QUEUE_FAMILY_IGNORED},
      {ctx->dec_imgs[2], VK_QUEUE_FAMILY_IGNORED},
  };
  pyrowave_gpu_external_reference release_ref[3] = {
      {ctx->dec_imgs[0], VK_QUEUE_FAMILY_EXTERNAL},
      {ctx->dec_imgs[1], VK_QUEUE_FAMILY_EXTERNAL},
      {ctx->dec_imgs[2], VK_QUEUE_FAMILY_EXTERNAL},
  };

  pyrowave_gpu_sync_operation acquire = {};
  acquire.sync.semaphore = sem;
  acquire.sync.value = wait_fence_val;
  acquire.num_images = 3;
  acquire.images = acquire_ref;

  pyrowave_gpu_sync_operation release = {};
  release.sync.semaphore = sem;
  release.sync.value = signal_fence_val;
  release.num_images = 3;
  release.images = release_ref;

  res = pyrowave_decoder_decode_gpu_buffer(ctx->dec, &acquire, &release,
                                           &ctx->dec_buffers);
  return (res == PYROWAVE_SUCCESS) ? 0 : -4;
}

int zc_pyro_decode_yuv420p(zc_pyro_ctx_t *ctx, const uint8_t *bitstream,
                           size_t len, uint8_t *y_out, size_t y_stride,
                           uint8_t *cb_out, size_t cb_stride, uint8_t *cr_out,
                           size_t cr_stride) {
  if (!ctx || !ctx->dec || !bitstream || len == 0 || !y_out || !cb_out || !cr_out) {
    return -1;
  }
  pyrowave_decoder_clear(ctx->dec);
  pyrowave_result res = pyrowave_decoder_push_packet(ctx->dec, bitstream, len);
  if (res != PYROWAVE_SUCCESS) {
    return -2;
  }
  if (!pyrowave_decoder_decode_is_ready(ctx->dec, true)) {
    return -3;
  }

  pyrowave_cpu_buffer cb = {};
  cb.data[0] = y_out;
  cb.data[1] = cb_out;
  cb.data[2] = cr_out;
  cb.row_stride_in_bytes[0] = y_stride;
  cb.row_stride_in_bytes[1] = cb_stride;
  cb.row_stride_in_bytes[2] = cr_stride;
  cb.plane_size_in_bytes[0] = y_stride * ctx->height;
  cb.plane_size_in_bytes[1] = cb_stride * (ctx->height / 2);
  cb.plane_size_in_bytes[2] = cr_stride * (ctx->height / 2);
  cb.width = (int)ctx->width;
  cb.height = (int)ctx->height;
  cb.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

  res = pyrowave_decoder_decode_cpu_buffer_synchronous(ctx->dec, &cb);
  return (res == PYROWAVE_SUCCESS) ? 0 : -4;
}

void zc_pyro_destroy(zc_pyro_ctx_t *ctx) {
  if (!ctx) return;
  /* Destroy encoder and decoder first so GPU work drains before freeing views/images/sync. */
  if (ctx->enc) pyrowave_encoder_destroy(ctx->enc);
  if (ctx->dec) pyrowave_decoder_destroy(ctx->dec);
  for (int i = 0; i < 3; ++i) {
    if (ctx->dec_imgs[i]) pyrowave_image_destroy(ctx->dec_imgs[i]);
  }
  if (ctx->enc_img) pyrowave_image_destroy(ctx->enc_img);
  if (ctx->shared_sync) pyrowave_sync_object_destroy(ctx->shared_sync);
  if (ctx->dev) pyrowave_device_destroy(ctx->dev);
  delete ctx;
}
