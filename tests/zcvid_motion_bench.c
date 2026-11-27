/* Zcvid codec motion bench: moves a 1920x1080 picture through the zcvid
 * encoder and decoder (same process, no network) and reports luma PSNR
 * against the source, bitrate, encode / decode time and the encoder /
 * decoder reference mismatch, which must be 0.
 *
 *   zcvid_motion_bench <image.raw | -> <vx> <vy> <mbps> [frames] [dump.bmp]
 *
 * image.raw: 1920x1080 BGRA, 8 bits per channel, no header; "-" generates a
 * textured test picture. The first 120 frames are still, then the picture
 * moves by (vx, vy) whole pixels per frame (wrapping), unless one of:
 *   FPAN=vx,vy   sub-pixel pan, bicubic resampling (BILINEAR=1: bilinear)
 *   ZOOM=n       zoom in by n per mille per frame (bilinear, cycles)
 * Codec options (as zcrudp_stream uses them: PACK=1 EC=1 QPEL=1 TQ=1 TQI=1):
 *   PACK EC QPEL TQ TQI     packed records, range coder, quarter-pel vectors,
 *                           transform blocks, transform inter (hybrid)
 *   ECLIMIT=n ECTHREADS=n   range coder budget / threads
 * Diagnostics:
 *   FUZZ=1       decode corrupted copies of the coded records (must not crash)
 *   BREAKDOWN=1  bytes per record class      CHROMA=1  Cb/Cr PSNR as well
 *   TRACE=1      rate control state per frame
 *   CPUDEC=1     verify bit-exactness of every GPU-encoded frame against zcvid_gpu_cpu_dec.h
 * Unity build over gpu_pipeline_win32.c, to reach the capture texture. */
#include "gpu_pipeline_win32.c"
#include "zcvid_gpu_cpu_dec.h"
#include <math.h>

static int g_W = 1920;
static int g_H = 1080;
#define W g_W
#define H g_H

static bool env_flag(const char *name, bool def_val) {
  const char *v = getenv(name);
  if (!v || !v[0]) return def_val;
  return atoi(v) != 0;
}

static void dump_bmp(gpu_pipeline_t *p, const char *path) {
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
  td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.Format = p->zcvid_dec_use_copy ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  ID3D11Texture2D *stg = NULL;
  if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &stg))) return;
  ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)stg, (ID3D11Resource *)p->zcvid_dec_tex);
  D3D11_MAPPED_SUBRESOURCE m = {0};
  if (SUCCEEDED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
    FILE *f = fopen(path, "wb");
    if (f) {
      uint8_t hdr[54] = {'B', 'M'};
      uint32_t sz = 54 + W * H * 3, off = 54, ih = 40, w = W, h = H;
      uint16_t planes = 1, bpp = 24;
      memcpy(hdr + 2, &sz, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &ih, 4);
      memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); memcpy(hdr + 26, &planes, 2);
      memcpy(hdr + 28, &bpp, 2);
      fwrite(hdr, 1, 54, f);
      uint8_t *row = (uint8_t *)malloc(W * 3);
      for (int y = H - 1; y >= 0; --y) {
        const uint8_t *s = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        for (int x = 0; x < W; ++x) {
          row[x * 3 + 0] = s[x * 4 + (p->zcvid_dec_use_copy ? 0 : 2)];
          row[x * 3 + 1] = s[x * 4 + 1];
          row[x * 3 + 2] = s[x * 4 + (p->zcvid_dec_use_copy ? 2 : 0)];
        }
        fwrite(row, 1, W * 3, f);
      }
      free(row);
      fclose(f);
    }
    ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)stg, 0);
  }
  ID3D11Texture2D_Release(stg);
}

static void build_cinema_scene(uint8_t *dst, int w, int h, int scene) {
  for (int y = 0; y < h; ++y) {
    double fy = (double)y / (double)(h > 1 ? h - 1 : 1);
    for (int x = 0; x < w; ++x) {
      double fx = (double)x / (double)(w > 1 ? w - 1 : 1);
      double r, g, b;
      if (scene == 0) {
        /* Dark cinema night sky / shadow gradient (Y ~ 8..58) + warm lamp glow */
        double grad = 10.0 + 44.0 * (0.65 * fy + 0.35 * fx);
        double dx = fx - 0.72, dy = fy - 0.38;
        double glow = 85.0 * exp(-(dx * dx + dy * dy) * 28.0);
        r = grad * 0.85 + glow * 1.15;
        g = grad * 0.95 + glow * 0.78;
        b = grad * 1.18 + glow * 0.32;
        if (y > (int)(h * 0.78) && ((x / 48) % 5 == 0)) { r *= 0.45; g *= 0.45; b *= 0.5; }
      } else if (scene == 1) {
        /* Abrupt cut: sunlit architectural scene with sharp edges & fine texture */
        double sky = 155.0 + 65.0 * (1.0 - fy);
        double wave = 22.0 * sin(x * 0.019) * cos(y * 0.023) + 12.0 * sin((x + y) * 0.17);
        r = sky * 0.92 + wave;
        g = sky * 0.96 + wave * 0.9;
        b = sky * 1.05 + wave * 0.7;
        if (((x / 120) + (y / 90)) % 2 == 0 && y > h / 3) { r -= 55.0; g -= 48.0; b -= 40.0; }
        if ((x % 181) < 2 || (y % 137) < 2) { r = 24.0; g = 26.0; b = 30.0; }
      } else {
        /* Abrupt cut: moody warm interior + dark shadow vignette + skin-tone gradient */
        double vx = fx - 0.5, vy = fy - 0.5;
        double vig = exp(-(vx * vx + vy * vy) * 4.5);
        double base = 14.0 + 135.0 * vig + 14.0 * sin(x * 0.011 + y * 0.013);
        r = base * 1.12;
        g = base * 0.88;
        b = base * 0.72;
      }
      uint8_t *px = dst + ((size_t)y * w + x) * 4;
      px[0] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : (int)(b + 0.5));
      px[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : (int)(g + 0.5));
      px[2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : (int)(r + 0.5));
      px[3] = 255;
    }
  }
}

static void add_film_grain(uint8_t *frm, int w, int h, uint32_t frame_seed, int amp) {
  if (amp <= 0) return;
  uint32_t st = frame_seed * 2654435761u ^ 0x9E3779B9u;
  size_t n = (size_t)w * (size_t)h;
  for (size_t i = 0; i < n; ++i) {
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    int g = (int)(st % (uint32_t)(2 * amp + 1)) - amp;
    uint8_t *p = frm + i * 4;
    int b = (int)p[0] + g; p[0] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    int gr = (int)p[1] + g; p[1] = (uint8_t)(gr < 0 ? 0 : gr > 255 ? 255 : gr);
    int r = (int)p[2] + g; p[2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
  }
}

static void compute_bgra_psnr(const uint8_t *src, const uint8_t *dec, uint32_t dec_pitch,
                              int w, int h, bool dec_is_bgra, double *out_y, double *out_c) {
  double se_y = 0.0, se_cb = 0.0, se_cr = 0.0;
  for (int y = 0; y < h; ++y) {
    const uint8_t *s = src + (size_t)y * w * 4;
    const uint8_t *d = dec + (size_t)y * dec_pitch;
    for (int x = 0; x < w; ++x) {
      double sb = s[x * 4 + 0], sg = s[x * 4 + 1], sr = s[x * 4 + 2];
      double db = d[x * 4 + (dec_is_bgra ? 0 : 2)], dg = d[x * 4 + 1], dr = d[x * 4 + (dec_is_bgra ? 2 : 0)];
      double sy = 0.299 * sr + 0.587 * sg + 0.114 * sb;
      double dy = 0.299 * dr + 0.587 * dg + 0.114 * db;
      double scb = -0.169 * sr - 0.331 * sg + 0.500 * sb + 128.0;
      double dcb = -0.169 * dr - 0.331 * dg + 0.500 * db + 128.0;
      double scr = 0.500 * sr - 0.419 * sg - 0.081 * sb + 128.0;
      double dcr = 0.500 * dr - 0.419 * dg - 0.081 * db + 128.0;
      double ey = sy - dy, ecb = scb - dcb, ecr = scr - dcr;
      se_y += ey * ey;
      se_cb += ecb * ecb;
      se_cr += ecr * ecr;
    }
  }
  double npx = (double)w * (double)h;
  if (out_y) *out_y = 10.0 * log10(255.0 * 255.0 / (se_y / npx + 1e-9));
  if (out_c) {
    double pcb = 10.0 * log10(255.0 * 255.0 / (se_cb / npx + 1e-9));
    double pcr = 10.0 * log10(255.0 * 255.0 / (se_cr / npx + 1e-9));
    *out_c = 0.5 * (pcb + pcr);
  }
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <image.raw | -> <vx> <vy> <mbps> [frames] [dump.bmp]" "\n", argv[0]);
    return 2;
  }
  const char *raw = argv[1];
  int vx = atoi(argv[2]), vy = atoi(argv[3]);
  uint32_t mbps = (uint32_t)atoi(argv[4]);
  uint32_t total = (argc > 5) ? (uint32_t)atoi(argv[5]) : 360U;
  const char *dump = (argc > 6) ? argv[6] : NULL;
  if (getenv("WIDTH")) g_W = atoi(getenv("WIDTH"));
  if (getenv("HEIGHT")) g_H = atoi(getenv("HEIGHT"));
  uint32_t fps = getenv("FPS") ? (uint32_t)atoi(getenv("FPS")) : 120U;
  if (fps == 0U) fps = 120U;

  uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4);
  uint8_t *frm = (uint8_t *)malloc((size_t)W * H * 4);
  if (strcmp(raw, "-") == 0) {
    /* Generated picture: smooth gradients, fine texture and sharp edges. */
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        uint8_t *px = img + ((size_t)y * W + x) * 4;
        double v = 128 + 50 * sin(x * 0.013) * cos(y * 0.017) + 25 * sin((x + 2 * y) * 0.21);
        if (((x / 96) + (y / 72)) % 2 == 0) v += 30;           /* edges */
        if ((x % 211) < 3 || (y % 157) < 2) v = 20;            /* thin dark lines */
        double vb = v + 40 * sin(y * 0.009), vr = v + 40 * cos(x * 0.007);
        px[0] = (uint8_t)(vb < 0 ? 0 : vb > 255 ? 255 : vb);
        px[1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        px[2] = (uint8_t)(vr < 0 ? 0 : vr > 255 ? 255 : vr);
        px[3] = 255;
      }
    }
  } else {
    FILE *f = fopen(raw, "rb");
    if (!f || fread(img, 1, (size_t)W * H * 4, f) != (size_t)W * H * 4) {
      fprintf(stderr, "cannot read %s (1920x1080 BGRA expected)" "\n", raw);
      return 2;
    }
    fclose(f);
  }

  bool cinema_mode = env_flag("CINEMA", false);
  bool nvenc_mode = env_flag("NVENC", false);
  bool pyro_mode = !nvenc_mode && env_flag("PYROWAVE", false);
  int grain_amp = getenv("GRAIN") ? atoi(getenv("GRAIN")) : (cinema_mode ? 2 : 0);
  uint8_t *cinema_scenes[3] = {NULL, NULL, NULL};
  if (cinema_mode) {
    for (int sc = 0; sc < 3; ++sc) {
      cinema_scenes[sc] = (uint8_t *)malloc((size_t)W * H * 4);
      build_cinema_scene(cinema_scenes[sc], W, H, sc);
    }
    memcpy(img, cinema_scenes[0], (size_t)W * H * 4);
  }

  qpc_now_us();
  gpu_pipeline_t *gpu = NULL;
  gpu_pipeline_info_t info;
  if (nvenc_mode) {
    if (gpu_pipeline_create(&gpu, (uint32_t)W, (uint32_t)H, fps, mbps * 1000000U, false, &info) != 0) return 1;
  } else if (pyro_mode) {
    if (gpu_pipeline_create_pyrowave(&gpu, (uint32_t)W, (uint32_t)H, fps, mbps * 1000000U, false, &info) != 0) return 1;
  } else {
    if (gpu_pipeline_create_zcvid(&gpu, (uint32_t)W, (uint32_t)H, fps, 6U, 4U, 256U, 128U, false, &info) != 0) return 1;
    gpu_pipeline_set_bitrate(gpu, mbps * 1000000U);
    /* Unified codec is enabled by default; env vars still allow A/B testing (0 or 1). */
    if (getenv("PACK")) gpu_pipeline_zcvid_set_packed(gpu, env_flag("PACK", true));
    if (getenv("EC")) gpu_pipeline_zcvid_set_entropy(gpu, env_flag("EC", true));
    if (getenv("TQ")) gpu_pipeline_zcvid_set_transform(gpu, env_flag("TQ", true));
    if (getenv("TQI")) gpu_pipeline_zcvid_set_transform_inter(gpu, env_flag("TQI", true));
    if (getenv("DCT8")) gpu_pipeline_zcvid_set_dct8(gpu, env_flag("DCT8", true));
    if (getenv("QPEL")) gpu_pipeline_zcvid_set_qpel(gpu, env_flag("QPEL", true));
    if (getenv("DITHER")) gpu_pipeline_zcvid_set_dither(gpu, env_flag("DITHER", true));
    if (getenv("ECLIMIT")) gpu_pipeline_zcvid_set_entropy_limit(gpu, (uint32_t)atoi(getenv("ECLIMIT")));
    if (getenv("ECTHREADS")) gpu->zcvid_ec_threads = (uint32_t)atoi(getenv("ECTHREADS"));
  }
  gpu_pipeline_force_synth(gpu, true);
  gpu->dupl_has_frame = true; /* keep the test pattern off: we upload frames */

  ID3D11Texture2D *nv_stg = NULL;
  bool measure_chroma = env_flag("CHROMA", false);
  if (nvenc_mode || pyro_mode || measure_chroma) {
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)W; td.Height = (UINT)H; td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
#ifdef ZC_HAVE_PYROWAVE
    if (pyro_mode && !gpu->pyro_dec_use_copy) td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
#endif
    if (!nvenc_mode && !pyro_mode && !gpu->zcvid_dec_use_copy) td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D11Device_CreateTexture2D(gpu->dev, &td, NULL, &nv_stg);
  }

  double psnr_sum = 0, psnr_min = 1e9, enc_sum = 0, dec_sum = 0, enc_max = 0;
  uint64_t bytes = 0, mm_total = 0, cpu_mm_total = 0;
  bool verify_cpudec = !nvenc_mode && !pyro_mode && env_flag("CPUDEC", false);
  zcvid_gpu_dec_state_t cpu_dec = {0};
  zcvid_mv_t *cpu_mvs = NULL;
  ID3D11Buffer *cpu_cmp_stg = NULL;
  if (verify_cpudec) {
    if (zcvid_gpu_dec_init(&cpu_dec, (uint32_t)W, (uint32_t)H) != 0) {
      fprintf(stderr, "zcvid_gpu_dec_init failed\n");
      return 1;
    }
    zcvid_gpu_dec_reset(&cpu_dec, gpu->zcvid_neutral_y, gpu->zcvid_delta_min);
    cpu_mvs = (zcvid_mv_t *)calloc((size_t)gpu->zcvid_tiles_x * gpu->zcvid_tiles_y, sizeof(zcvid_mv_t));
    D3D11_BUFFER_DESC cbd = {0};
    cbd.ByteWidth = (UINT)((size_t)W * H * sizeof(uint32_t));
    cbd.Usage = D3D11_USAGE_STAGING;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateBuffer(gpu->dev, &cbd, NULL, &cpu_cmp_stg);
  }
  double bd_bytes[3] = {0, 0, 0}; uint64_t bd_tq_rec = 0, bd_tq_blk = 0, bd_tq_chroma = 0; uint64_t bd_intra_px = 0, bd_delta_rec = 0; uint32_t bd_frames = 0;
  double cpsnr_sum = 0; uint32_t cpsnr_n = 0;
  uint32_t n = 0, ec_frames = 0, rt_bad = 0, dec_err = 0, fuzz_runs = 0;
  uint32_t *rt_w = (uint32_t *)malloc(64u << 20);
  uint8_t *rt_a = (uint8_t *)malloc(64u << 20), *rt_b = (uint8_t *)malloc(64u << 20);
  const uint32_t measure_start = cinema_mode ? 20U : 150U;
  for (uint32_t fr = 0; fr < total; ++fr) {
    /* still for the first 120 frames, then pan (in cinema mode: slow pan + scene cuts at 60 & 120) */
    int k = cinema_mode ? (int)(fr % 60U) : ((fr < 120U) ? 0 : (int)(fr - 120U));
    const uint8_t *src_img = cinema_mode ? cinema_scenes[(fr / 60U) % 3U] : img;
    int ox = ((k * vx) % W + W) % W, oy = ((k * vy) % H + H) % H;
    if (getenv("FPAN")) {
      /* Sub-pixel camera pan: FPAN="vx,vy" in pixels per frame (fractions
       * allowed), Catmull-Rom bicubic (BILINEAR=1: bilinear), wrapping around. */
      double fvx = 0, fvy = 0;
      sscanf(getenv("FPAN"), "%lf,%lf", &fvx, &fvy);
      double offx = fmod(k * fvx, W), offy = fmod(k * fvy, H);
      if (offx < 0) offx += W;
      if (offy < 0) offy += H;
      int bx = (int)offx, by = (int)offy;
      double wx = offx - bx, wy = offy - by;
      float cwx[4], cwy[4];
      for (int t = 0; t < 2; ++t) {
        double u = t ? wy : wx;
        float *cw = t ? cwy : cwx;
        cw[0] = (float)(((-u + 2) * u - 1) * u * 0.5);
        cw[1] = (float)(((3 * u - 5) * u * u + 2) * 0.5);
        cw[2] = (float)(((-3 * u + 4) * u + 1) * u * 0.5);
        cw[3] = (float)((u - 1) * u * u * 0.5);
      }
      if (getenv("BILINEAR")) {
        cwx[0] = cwx[3] = cwy[0] = cwy[3] = 0.0f;
        cwx[1] = (float)(1.0 - wx); cwx[2] = (float)wx;
        cwy[1] = (float)(1.0 - wy); cwy[2] = (float)wy;
      }
      for (int y = 0; y < H; ++y) {
        const uint8_t *r0 = src_img + (size_t)((y + by - 1 + H) % H) * W * 4;
        const uint8_t *r1 = src_img + (size_t)((y + by + 0 + H) % H) * W * 4;
        const uint8_t *r2 = src_img + (size_t)((y + by + 1 + H) % H) * W * 4;
        const uint8_t *r3 = src_img + (size_t)((y + by + 2 + H) % H) * W * 4;
        uint8_t *d = frm + (size_t)y * W * 4;
        for (int x = 0; x < W; ++x) {
          int x0 = (x + bx - 1 + W) % W * 4;
          int x1 = (x + bx + 0 + W) % W * 4;
          int x2 = (x + bx + 1 + W) % W * 4;
          int x3 = (x + bx + 2 + W) % W * 4;
          for (int c = 0; c < 3; ++c) {
            float h0 = cwx[0] * r0[x0 + c] + cwx[1] * r0[x1 + c] + cwx[2] * r0[x2 + c] + cwx[3] * r0[x3 + c];
            float h1 = cwx[0] * r1[x0 + c] + cwx[1] * r1[x1 + c] + cwx[2] * r1[x2 + c] + cwx[3] * r1[x3 + c];
            float h2 = cwx[0] * r2[x0 + c] + cwx[1] * r2[x1 + c] + cwx[2] * r2[x2 + c] + cwx[3] * r2[x3 + c];
            float h3 = cwx[0] * r3[x0 + c] + cwx[1] * r3[x1 + c] + cwx[2] * r3[x2 + c] + cwx[3] * r3[x3 + c];
            float v = cwy[0] * h0 + cwy[1] * h1 + cwy[2] * h2 + cwy[3] * h3;
            d[x * 4 + c] = (uint8_t)(v < 0.0f ? 0 : v > 255.0f ? 255 : (int)(v + 0.5f));
          }
          d[x * 4 + 3] = 255;
        }
      }
    } else
    if (getenv("ZOOM")) {
      /* Camera moving forward: scale about the centre, ZOOM per mille per
       * frame, bilinear, cycling every 240 frames so it never runs out. */
      double rate = atof(getenv("ZOOM")) / 1000.0;
      double sc = 1.0 + rate * (double)(k % 240);
      for (int y = 0; y < H; ++y) {
        double fy = (y - H / 2.0) / sc + H / 2.0;
        int y0 = (int)floor(fy); double wy = fy - y0;
        if (y0 < 0) { y0 = 0; wy = 0; } if (y0 > H - 2) { y0 = H - 2; wy = 1; }
        uint8_t *d = frm + (size_t)y * W * 4;
        for (int x = 0; x < W; ++x) {
          double fx = (x - W / 2.0) / sc + W / 2.0;
          int x0 = (int)floor(fx); double wx = fx - x0;
          if (x0 < 0) { x0 = 0; wx = 0; } if (x0 > W - 2) { x0 = W - 2; wx = 1; }
          const uint8_t *a = src_img + ((size_t)y0 * W + x0) * 4, *b = a + W * 4;
          for (int c = 0; c < 4; ++c) {
            double v = (a[c] * (1 - wx) + a[4 + c] * wx) * (1 - wy) + (b[c] * (1 - wx) + b[4 + c] * wx) * wy;
            d[x * 4 + c] = (uint8_t)(v + 0.5);
          }
        }
      }
    } else
    for (int y = 0; y < H; ++y) {
      const uint8_t *s = src_img + (size_t)((y + oy) % H) * W * 4;
      uint8_t *d = frm + (size_t)y * W * 4;
      memcpy(d, s + (size_t)ox * 4, (size_t)(W - ox) * 4);
      memcpy(d + (size_t)(W - ox) * 4, s, (size_t)ox * 4);
    }
    if (grain_amp > 0) add_film_grain(frm, W, H, fr + 1U, grain_amp);
    /* Emulate a freshly acquired desktop frame: the pipeline swaps capture and
     * previous-source textures on entry and only keeps the new one when a
     * frame was acquired. Do that swap here, so the upload lands in the
     * texture the encoder reads and the previous one keeps frame N-1. */
    if (!nvenc_mode && !pyro_mode && gpu->zcvid_has_prev_src) {
      ID3D11Texture2D *tt = gpu->cap_bgra_tex; gpu->cap_bgra_tex = gpu->zcvid_prev_src_tex; gpu->zcvid_prev_src_tex = tt;
      ID3D11RenderTargetView *tr = gpu->cap_rtv; gpu->cap_rtv = gpu->zcvid_prev_src_rtv; gpu->zcvid_prev_src_rtv = tr;
      ID3D11ShaderResourceView *ts = gpu->cap_bgra_srv; gpu->cap_bgra_srv = gpu->zcvid_prev_src_srv; gpu->zcvid_prev_src_srv = ts;
      gpu->zcvid_has_prev_src = false; /* no second swap, no copy-over inside */
    }
    ID3D11DeviceContext_UpdateSubresource(gpu->ctx, (ID3D11Resource *)gpu->cap_bgra_tex,
                                          0, NULL, frm, W * 4, 0);
    const uint8_t *bs = NULL;
    size_t len = 0;
    bool kf = false;
    double c = 0, e = 0, d = 0, p = 0;
    if (gpu_pipeline_capture_and_encode(gpu, fr, &bs, &len, &kf, &c, &e) != 0) continue;
    int drc = gpu_pipeline_decode_and_present(gpu, bs, len, &d, &p);
    if (drc < 0) dec_err++;
    if (verify_cpudec && drc == 0) {
      uint32_t hw = 0, mvb = 0;
      memcpy(&hw, bs, 4);
      memcpy(&mvb, bs + 4, 4);
      bool pk = (hw & ZCVID_PACKED_FLAG) != 0U;
      bool pk_rec = pk && (hw & ZCVID_RAW_RECORDS_FLAG) == 0U;
      uint32_t rw = pk ? (hw & ZCVID_RECORD_WORDS_MASK) : hw;
      uint32_t ncls = (pk && (hw & ZCVID_V2_FLAG) != 0U) ? 9U : 8U;
      uint32_t nt = gpu->zcvid_tiles_x * gpu->zcvid_tiles_y;
      bool mv_qpel = false;
      bool ec_mv = pk && ((hw & ZCVID_EC_MV_FLAG) != 0U);
      bool wide_mv = pk ? ((hw & ZCVID_WIDE_MV_FLAG) != 0U)
                        : (mvb == nt * 4U || mvb == nt * 4U + 1U);
      if (mvb == 0U) {
        memset(cpu_mvs, 0, (size_t)nt * sizeof(zcvid_mv_t));
      } else if (ec_mv) {
        if (!zcvid_ec_unpack_mvs(bs + 8U, mvb, (int32_t *)cpu_mvs,
                                gpu->zcvid_tiles_x, gpu->zcvid_tiles_y, &mv_qpel)) {
          dec_err++;
        }
      } else {
        uint32_t run_step = wide_mv ? 5U : 3U;
        mv_qpel = pk ? (mvb % run_step == 1U) : (mvb == nt * (wide_mv ? 4U : 2U) + 1U);
        if (pk) {
          uint32_t plen = mv_qpel ? (mvb - 1U) : mvb;
          if (!zcvid_unpack_mvs(bs + 8U, plen, (int32_t *)cpu_mvs, nt, wide_mv)) dec_err++;
        } else if (wide_mv) {
          const int16_t *smv = (const int16_t *)(bs + 8U);
          for (uint32_t i = 0; i < nt; ++i) { cpu_mvs[i].x = smv[i * 2U]; cpu_mvs[i].y = smv[i * 2U + 1U]; }
        } else {
          const int8_t *smv = (const int8_t *)(bs + 8U);
          for (uint32_t i = 0; i < nt; ++i) { cpu_mvs[i].x = smv[i * 2U]; cpu_mvs[i].y = smv[i * 2U + 1U]; }
        }
      }
      uint32_t got_w = rw;
      const uint32_t *recs_ptr = NULL;
      if (rw > 0U) {
        if (pk_rec) {
          bool tq_v4 = (hw & ZCVID_TQ_V4_FLAG) != 0U;
          if (hw & ZCVID_EC_FLAG) {
            got_w = zcvid_ec_unpack_records(gpu, bs + 8U + mvb, len - 8U - mvb, rt_w, rw, ncls, tq_v4);
          } else {
            got_w = zcvid_unpack_records(bs + 8U + mvb, len - 8U - mvb, rt_w, rw, ncls, tq_v4);
          }
          if (got_w == UINT32_MAX) { dec_err++; got_w = 0U; }
          recs_ptr = rt_w;
        } else {
          recs_ptr = (const uint32_t *)(bs + 8U + mvb);
        }
      }
      zcvid_gpu_dec_params_t cp = {
          (uint32_t)W, (uint32_t)H, gpu->zcvid_delta_min, gpu->zcvid_delta_max,
          gpu->zcvid_gamma_q8, gpu->zcvid_neutral_y, mv_qpel ? 1U : 0U, ZCVID_POST_THR
      };
      if (zcvid_gpu_dec_decode_frame(&cpu_dec, &cp, cpu_mvs, recs_ptr, got_w) != 0) dec_err++;
      if ((fr + 1U == total || fr == 60U || fr == 120U) && cpu_cmp_stg) {
        ID3D11DeviceContext_CopyResource(gpu->ctx, (ID3D11Resource *)cpu_cmp_stg,
                                         (ID3D11Resource *)gpu->zcvid_dec_yuvd_buf[gpu->zcvid_dec_ping & 1U]);
        D3D11_MAPPED_SUBRESOURCE cm = {0};
        if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)cpu_cmp_stg, 0, D3D11_MAP_READ, 0, &cm))) {
          const uint32_t *gref = (const uint32_t *)cm.pData;
          size_t npx = (size_t)W * (size_t)H;
          uint64_t diff = 0;
          for (size_t i = 0; i < npx; ++i) {
            if (gref[i] != cpu_dec.prev_yuvd[i]) diff++;
          }
          cpu_mm_total += diff;
          ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)cpu_cmp_stg, 0);
        }
      }
    }
    if (nvenc_mode || pyro_mode) {
      if (drc == 0 && fr >= measure_start && nv_stg) {
        ID3D11Texture2D *src_dec = NULL;
        bool release_src = false;
        bool dec_is_bgra = true;
        if (nvenc_mode) {
          if (SUCCEEDED(IDXGISwapChain1_GetBuffer(gpu->swapchain, 0, &IID_ID3D11Texture2D, (void **)&src_dec))) {
            release_src = true;
          }
        } else {
#ifdef ZC_HAVE_PYROWAVE
          src_dec = gpu->pyro_dec_tex;
          dec_is_bgra = gpu->pyro_dec_use_copy;
#endif
        }
        if (src_dec) {
          ID3D11DeviceContext_CopyResource(gpu->ctx, (ID3D11Resource *)nv_stg, (ID3D11Resource *)src_dec);
          if (release_src) ID3D11Texture2D_Release(src_dec);
          D3D11_MAPPED_SUBRESOURCE nm = {0};
          if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)nv_stg, 0, D3D11_MAP_READ, 0, &nm))) {
            double ydb = 0.0, cdb = 0.0;
            compute_bgra_psnr(frm, (const uint8_t *)nm.pData, nm.RowPitch, W, H, dec_is_bgra, &ydb, &cdb);
            ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)nv_stg, 0);
            psnr_sum += ydb;
            if (ydb < psnr_min) psnr_min = ydb;
            if (measure_chroma) { cpsnr_sum += cdb; cpsnr_n++; }
            if (cinema_mode && (fr == 59U || fr == 60U || fr == 61U || fr == 62U || fr == 64U || fr == 120U || fr == 121U || fr == 122U || fr == 124U)) {
              printf("  [%s f%3u] y_psnr=%.2f c_psnr=%.2f len=%zu enc=%.0fus\n",
                     pyro_mode ? "pyro " : "nvenc", fr, ydb, cdb, len, e);
            }
          }
        }
        bytes += len;
        enc_sum += e; dec_sum += d; if (e > enc_max) enc_max = e;
        n++;
      }
      continue;
    }
    {
      uint32_t hw = 0, mvb = 0;
      memcpy(&hw, bs, 4); memcpy(&mvb, bs + 4, 4);
      if (hw & ZCVID_EC_FLAG) {
        ec_frames++;
        uint32_t rw = hw & ZCVID_RECORD_WORDS_MASK;
        bool tq_v4 = (hw & ZCVID_TQ_V4_FLAG) != 0U;
        uint32_t got = zcvid_ec_unpack_records(gpu, bs + 8 + mvb, len - 8 - mvb, rt_w, rw, (hw & ZCVID_V2_FLAG) ? 9U : 8U, tq_v4);
        D3D11_MAPPED_SUBRESOURCE m2 = {0};
        if (got != rw) rt_bad++;
        if (getenv("FUZZ") && fr % 8U == 0U) {
          size_t bl = len - 8 - mvb;
          uint8_t *fz = (uint8_t *)malloc(bl + 1);
          for (unsigned it = 0; it < 40U; ++it) {
            memcpy(fz, bs + 8 + mvb, bl);
            unsigned nflip = 1U + (unsigned)rand() % 8U;
            for (unsigned q = 0; q < nflip; ++q) fz[(size_t)rand() * 7919U % bl] ^= (uint8_t)(1U << (rand() & 7));
            size_t cut = (it & 1U) ? bl : (size_t)rand() * 131U % (bl + 1);
            if (it >= 30U) for (size_t q = 0; q < bl; ++q) fz[q] = (uint8_t)rand();
            uint32_t g = zcvid_ec_unpack_records(gpu, fz, cut, rt_w, rw, (hw & ZCVID_V2_FLAG) ? 9U : 8U, tq_v4);
            if (g != UINT32_MAX && g > rw) rt_bad += 1000;
            fuzz_runs++;
          }
          free(fz);
          if ((hw & ZCVID_EC_MV_FLAG) && mvb > 0U) {
            uint8_t *fmv = (uint8_t *)malloc(mvb + 1);
            int32_t *mv_out = (int32_t *)malloc((size_t)gpu->zcvid_tiles_x * gpu->zcvid_tiles_y * 2U * sizeof(int32_t));
            if (fmv && mv_out) {
              for (unsigned it = 0; it < 20U; ++it) {
                memcpy(fmv, bs + 8, mvb);
                unsigned nflip = 1U + (unsigned)rand() % 6U;
                for (unsigned q = 0; q < nflip; ++q) fmv[(size_t)rand() * 7919U % mvb] ^= (uint8_t)(1U << (rand() & 7));
                uint32_t cut = (it & 1U) ? mvb : (uint32_t)((size_t)rand() * 131U % (mvb + 1U));
                if (it >= 15U) for (uint32_t q = 0; q < mvb; ++q) fmv[q] = (uint8_t)rand();
                bool fq = false;
                (void)zcvid_ec_unpack_mvs(fmv, cut, mv_out, gpu->zcvid_tiles_x, gpu->zcvid_tiles_y, &fq);
                fuzz_runs++;
              }
            }
            free(mv_out);
            free(fmv);
          }
        }
        else if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)gpu->zcvid_out_records_stg[(gpu->zcvid_stg_idx + ZCVID_STG_RING - 1U) % ZCVID_STG_RING], 0, D3D11_MAP_READ, 0, &m2))) {
          size_t la = zcvid_pack_records(gpu, (const uint32_t *)m2.pData, gpu->zcvid_last_raw_words / 2, rt_a);
          size_t lb = zcvid_pack_records(gpu, rt_w, rw / 2, rt_b);
          if (la != lb || memcmp(rt_a, rt_b, la) != 0) rt_bad++;
          ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)gpu->zcvid_out_records_stg[(gpu->zcvid_stg_idx + ZCVID_STG_RING - 1U) % ZCVID_STG_RING], 0);
        }
      }
    }
    if (fr >= measure_start) {
      double psnr = 0, cdb = 0;
      if (measure_chroma && nv_stg && gpu->zcvid_dec_tex) {
        ID3D11DeviceContext_CopyResource(gpu->ctx, (ID3D11Resource *)nv_stg, (ID3D11Resource *)gpu->zcvid_dec_tex);
        D3D11_MAPPED_SUBRESOURCE nm = {0};
        if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)nv_stg, 0, D3D11_MAP_READ, 0, &nm))) {
          compute_bgra_psnr(frm, (const uint8_t *)nm.pData, nm.RowPitch, W, H, gpu->zcvid_dec_use_copy, &psnr, &cdb);
          ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)nv_stg, 0);
          cpsnr_sum += cdb;
          cpsnr_n++;
        }
      } else {
        gpu_pipeline_zcvid_compute_psnr(gpu, &psnr);
      }
      psnr_sum += psnr;
      if (psnr < psnr_min) psnr_min = psnr;
      bytes += len;
      enc_sum += e; dec_sum += d; if (e > enc_max) enc_max = e;
      n++;
      if (cinema_mode && (fr == 59U || fr == 60U || fr == 61U || fr == 62U || fr == 64U || fr == 120U || fr == 121U || fr == 122U || fr == 124U)) {
        printf("  [zcvid  f%3u] y_psnr=%.2f c_psnr=%.2f len=%zu tq_qp=%u intra8=%d enc=%.0fus\n",
               fr, psnr, cdb, len, gpu->zcvid_tq_qp, gpu->zcvid_intra8 ? 1 : 0, e);
      }
    }
    if (getenv("TRACE") && ((fr >= 118U && fr < 132U) || fr % 40U == 0U)) {
      D3D11_MAPPED_SUBRESOURCE mm = {0};
      uint32_t good = 0, zero = 0;
      if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)gpu->zcvid_enc_mvs_stg[(gpu->zcvid_stg_idx + ZCVID_STG_RING - 1U) % ZCVID_STG_RING], 0, D3D11_MAP_READ, 0, &mm))) {
        const int32_t *mv = (const int32_t *)mm.pData;
        uint32_t nt = gpu->zcvid_tiles_x * gpu->zcvid_tiles_y;
        for (uint32_t i = 0; i < nt; ++i) {
          if (mv[i*2] == -vx && mv[i*2+1] == -vy) good++;
          if (mv[i*2] == 0 && mv[i*2+1] == 0) zero++;
        }
        ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)gpu->zcvid_enc_mvs_stg[(gpu->zcvid_stg_idx + ZCVID_STG_RING - 1U) % ZCVID_STG_RING], 0);
      }
      double ps = 0; gpu_pipeline_zcvid_compute_psnr(gpu, &ps);
      printf("f%3u len=%6zu good=%4u zero=%4u thr=%2u 2x=%d ierr=%3u static=%d psnr=%.1f enc=%.0fus\n", fr, len, good, zero,
             gpu->zcvid_rc_threshold, gpu->zcvid_rc_2x, gpu->zcvid_rc_intra_err_thr, gpu->zcvid_static_scene, ps, e); if (getenv("TRACE")) printf("      tq_qp=%u" "\n", gpu->zcvid_tq_qp); if (getenv("TRACE")) printf("      raw_words=%u ec_ratio=%u pack_ratio=%u kept=%u\n", gpu->zcvid_last_raw_words, gpu->zcvid_ec_ratio_q16, gpu->zcvid_pack_ratio_q16, gpu->zcvid_sort_kept);
    }
    if (getenv("BREAKDOWN") && fr >= 150U) {
      /* Split this frame's records by class and range-code each part alone. */
      D3D11_MAPPED_SUBRESOURCE m3 = {0};
      uint32_t hw = 0; memcpy(&hw, bs, 4);
      uint32_t rw = hw & ZCVID_RECORD_WORDS_MASK;
      ID3D11Resource *stg = (ID3D11Resource *)gpu->zcvid_out_records_stg[(gpu->zcvid_stg_idx + ZCVID_STG_RING - 1U) % ZCVID_STG_RING];
      if (rw > 0 && SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, stg, 0, D3D11_MAP_READ, 0, &m3))) {
        const uint32_t *r = (const uint32_t *)m3.pData;
        static uint32_t part[3][4u << 20];
        uint32_t n[3] = {0, 0, 0};
        for (uint32_t i = 0; i + 1 < rw; i += 2) {
          int intra = (r[i] >> 30) == 3u;
          if (r[i] == 0xFFFFFFFFu) continue;
          if ((r[i] >> 28) == 0xFu && ((r[i] & 0x0FFFFFFFu) % 48u) >= 16u) bd_tq_chroma++;
          if ((r[i] >> 28) == 0xFu) { bd_tq_rec++; if ((r[i] & 0x0FFFFFFFu) % 48u == 0u) bd_tq_blk++; part[2][n[2]++] = r[i]; part[2][n[2]++] = r[i + 1]; continue; }
          part[intra][n[intra]++] = r[i]; part[intra][n[intra]++] = r[i + 1];
          if (intra) bd_intra_px += ((r[i] >> 29) & 1u) ? 4 : 2; else bd_delta_rec++;
        }
        for (int k = 0; k < 3; ++k) if (n[k]) {
          size_t b = zcvid_ec_pack_records(gpu, part[k], n[k] / 2, n[k] / 2, rt_a, 64u << 20);
          bd_bytes[k] += b;
        }
        bd_frames++;
        ID3D11DeviceContext_Unmap(gpu->ctx, stg, 0);
      }
    }
    if (fr == 119U) {
      double psnr = 0;
      gpu_pipeline_zcvid_compute_psnr(gpu, &psnr);
      printf("still psnr=%.2f  ", psnr);
    }
    if (fr + 1 == total) {
      D3D11_MAPPED_SUBRESOURCE mm = {0};
      if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)gpu->zcvid_enc_mvs_stg[(gpu->zcvid_stg_idx + ZCVID_STG_RING - 1U) % ZCVID_STG_RING], 0, D3D11_MAP_READ, 0, &mm))) {
        const int32_t *mv = (const int32_t *)mm.pData;
        uint32_t nt = gpu->zcvid_tiles_x * gpu->zcvid_tiles_y, exact = 0, zero = 0;
        for (uint32_t i = 0; i < nt; ++i) {
          if (mv[i*2] == vx && mv[i*2+1] == vy) exact++;
          if (mv[i*2] == 0 && mv[i*2+1] == 0) zero++;
        }
        printf("[mv exact=%u zero=%u of %u; first: ", exact, zero, nt);
        uint32_t s0 = (nt > 612U) ? 600U : 0U;
        uint32_t s1 = (nt > 612U) ? 612U : ((nt < 12U) ? nt : 12U);
        for (uint32_t i = s0; i < s1; ++i) printf("(%d,%d)", mv[i*2], mv[i*2+1]);
        puts("]");
        ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)gpu->zcvid_enc_mvs_stg[(gpu->zcvid_stg_idx + ZCVID_STG_RING - 1U) % ZCVID_STG_RING], 0);
      }
      if (gpu_pipeline_zcvid_ref_mismatch(gpu, &mm_total) != 0) dec_err++;
      if (dump) dump_bmp(gpu, dump);
    }
  }
  uint32_t denom = (n > 0U) ? n : 1U;
  printf("%dx%d@%uHz v=(%d,%d) %u Mbps: moving psnr avg=%.2f min=%.2f  %.1f kB/frame (%.1f Mbps)  mismatch=%llu\n",
         W, H, fps, vx, vy, mbps, psnr_sum / denom, n ? psnr_min : 0.0, (double)bytes / denom / 1000.0,
         (double)bytes / denom * 8.0 * (double)fps / 1e6, (unsigned long long)mm_total);
  printf("   enc avg=%.0f max=%.0f us, dec avg=%.0f us; ec frames %u/%u, roundtrip errors %u, decode errors %u\n",
         enc_sum / denom, enc_max, dec_sum / denom, ec_frames, total, rt_bad, dec_err);
  if (bd_frames) printf("   per frame: delta %.1f kB (%.0f records, %.2f B each), intra %.1f kB (%.0f px, %.2f bits/px)\n",
      bd_bytes[0] / bd_frames / 1000.0, (double)bd_delta_rec / bd_frames, bd_bytes[0] / (bd_delta_rec ? bd_delta_rec : 1),
      bd_bytes[1] / bd_frames / 1000.0, (double)bd_intra_px / bd_frames, bd_bytes[1] * 8.0 / (bd_intra_px ? bd_intra_px : 1));
  if (bd_frames && bd_tq_blk) printf("   transform: chroma %.0f%% of coefs, %.0f blocks/frame, %.1f coefs/block, %.1f kB/frame, %.2f bits/px" "\n",
      100.0 * bd_tq_chroma / (bd_tq_rec ? bd_tq_rec : 1), (double)bd_tq_blk / bd_frames, (double)bd_tq_rec / bd_tq_blk, bd_bytes[2] / bd_frames / 1000.0, bd_bytes[2] * 8.0 / (bd_tq_blk * 16.0));
  if (cpsnr_n) printf("   chroma psnr (Cb/Cr mean) avg=%.2f" "\n", cpsnr_sum / cpsnr_n);
  if (verify_cpudec) printf("   zcvid_gpu_cpu_dec bit-exact mismatch=%llu\n", (unsigned long long)cpu_mm_total);
  if (fuzz_runs) printf("   fuzzed decodes: %u\n", fuzz_runs);
  if (cpu_cmp_stg) ID3D11Buffer_Release(cpu_cmp_stg);
  if (verify_cpudec) zcvid_gpu_dec_destroy(&cpu_dec);
  free(cpu_mvs);
  if (nv_stg) ID3D11Texture2D_Release(nv_stg);
  for (int sc = 0; sc < 3; ++sc) free(cinema_scenes[sc]);
  gpu_pipeline_destroy(gpu);
  free(rt_w);
  free(rt_a);
  free(rt_b);
  free(img);
  free(frm);
  return (dec_err != 0U || rt_bad != 0U || mm_total != 0ULL || cpu_mm_total != 0ULL) ? 1 : 0;
}
