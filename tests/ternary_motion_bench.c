/* Ternary codec motion bench: moves a 1920x1080 picture through the ternary
 * encoder and decoder (same process, no network) and reports luma PSNR
 * against the source, bitrate, encode / decode time and the encoder /
 * decoder reference mismatch, which must be 0.
 *
 *   ternary_motion_bench <image.raw | -> <vx> <vy> <mbps> [frames] [dump.bmp]
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
 * Unity build over gpu_pipeline_win32.c, to reach the capture texture. */
#include "gpu_pipeline_win32.c"
#include <math.h>

#define W 1920
#define H 1080

static void dump_bmp(gpu_pipeline_t *p, const char *path) {
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
  td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.Format = p->tern_dec_use_copy ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  ID3D11Texture2D *stg = NULL;
  if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &stg))) return;
  ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)stg, (ID3D11Resource *)p->tern_dec_tex);
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
          row[x * 3 + 0] = s[x * 4 + (p->tern_dec_use_copy ? 0 : 2)];
          row[x * 3 + 1] = s[x * 4 + 1];
          row[x * 3 + 2] = s[x * 4 + (p->tern_dec_use_copy ? 2 : 0)];
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

  qpc_now_us();
  gpu_pipeline_t *gpu = NULL;
  gpu_pipeline_info_t info;
  if (gpu_pipeline_create_ternary(&gpu, W, H, 120, 6U, 4U, 256U, 128U, false, &info) != 0) return 1;
  gpu_pipeline_set_bitrate(gpu, mbps * 1000000U);
  gpu_pipeline_force_synth(gpu, true);
  if (getenv("PACK")) gpu_pipeline_ternary_set_packed(gpu, true);
  if (getenv("EC")) gpu_pipeline_ternary_set_entropy(gpu, true);
  if (getenv("TQ")) gpu_pipeline_ternary_set_transform(gpu, true);
  if (getenv("TQI")) gpu_pipeline_ternary_set_transform_inter(gpu, true);
  if (getenv("QPEL")) gpu_pipeline_ternary_set_qpel(gpu, true);
  if (getenv("ECLIMIT")) gpu_pipeline_ternary_set_entropy_limit(gpu, (uint32_t)atoi(getenv("ECLIMIT")));
  if (getenv("ECTHREADS")) gpu->tern_ec_threads = (uint32_t)atoi(getenv("ECTHREADS"));
  gpu->dupl_has_frame = true; /* keep the test pattern off: we upload frames */

  double psnr_sum = 0, psnr_min = 1e9, enc_sum = 0, dec_sum = 0, enc_max = 0;
  uint64_t bytes = 0, mm_total = 0;
  double bd_bytes[3] = {0, 0, 0}; uint64_t bd_tq_rec = 0, bd_tq_blk = 0, bd_tq_chroma = 0; uint64_t bd_intra_px = 0, bd_delta_rec = 0; uint32_t bd_frames = 0;
  double cpsnr_sum = 0; uint32_t cpsnr_n = 0;
  uint32_t n = 0, ec_frames = 0, rt_bad = 0, dec_err = 0, fuzz_runs = 0;
  uint32_t *rt_w = (uint32_t *)malloc(64u << 20);
  uint8_t *rt_a = (uint8_t *)malloc(64u << 20), *rt_b = (uint8_t *)malloc(64u << 20);
  for (uint32_t fr = 0; fr < total; ++fr) {
    /* still for the first 120 frames, then pan */
    int k = (fr < 120U) ? 0 : (int)(fr - 120U);
    int ox = ((k * vx) % W + W) % W, oy = ((k * vy) % H + H) % H;
    if (getenv("FPAN")) {
      /* Sub-pixel camera pan: FPAN="vx,vy" in pixels per frame (fractions
       * allowed), bilinear, wrapping around the picture. */
      double fvx = 0, fvy = 0;
      sscanf(getenv("FPAN"), "%lf,%lf", &fvx, &fvy);
      double offx = fmod(k * fvx, W), offy = fmod(k * fvy, H);
      if (offx < 0) offx += W;
      if (offy < 0) offy += H;
      int bx = (int)offx, by = (int)offy;
      double wx = offx - bx, wy = offy - by;
      /* Catmull-Rom (bicubic) weights: every frame stays about as sharp as the
       * original, as a rendered frame would; bilinear made every other frame
       * of a half-pel pan visibly blurrier than the next. */
      double cwx[4], cwy[4];
      for (int t = 0; t < 2; ++t) {
        double u = t ? wy : wx, *cw = t ? cwy : cwx;
        cw[0] = ((-u + 2) * u - 1) * u / 2;
        cw[1] = ((3 * u - 5) * u * u + 2) / 2;
        cw[2] = ((-3 * u + 4) * u + 1) * u / 2;
        cw[3] = (u - 1) * u * u / 2;
      }
      if (getenv("BILINEAR")) {
        cwx[0] = cwx[3] = cwy[0] = cwy[3] = 0;
        cwx[1] = 1 - wx; cwx[2] = wx; cwy[1] = 1 - wy; cwy[2] = wy;
      }
      for (int y = 0; y < H; ++y) {
        const uint8_t *rr[4];
        for (int j = 0; j < 4; ++j) rr[j] = img + (size_t)((y + by - 1 + j + H) % H) * W * 4;
        uint8_t *d = frm + (size_t)y * W * 4;
        for (int x = 0; x < W; ++x) {
          int xs[4];
          for (int i = 0; i < 4; ++i) xs[i] = (x + bx - 1 + i + W) % W;
          for (int c = 0; c < 4; ++c) {
            double v = 0;
            for (int j = 0; j < 4; ++j) {
              double h = 0;
              for (int i = 0; i < 4; ++i) h += cwx[i] * rr[j][xs[i] * 4 + c];
              v += cwy[j] * h;
            }
            v = v < 0 ? 0 : v > 255 ? 255 : v;
            d[x * 4 + c] = (uint8_t)(v + 0.5);
          }
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
          const uint8_t *a = img + ((size_t)y0 * W + x0) * 4, *b = a + W * 4;
          for (int c = 0; c < 4; ++c) {
            double v = (a[c] * (1 - wx) + a[4 + c] * wx) * (1 - wy) + (b[c] * (1 - wx) + b[4 + c] * wx) * wy;
            d[x * 4 + c] = (uint8_t)(v + 0.5);
          }
        }
      }
    } else
    for (int y = 0; y < H; ++y) {
      const uint8_t *s = img + (size_t)((y + oy) % H) * W * 4;
      uint8_t *d = frm + (size_t)y * W * 4;
      memcpy(d, s + (size_t)ox * 4, (size_t)(W - ox) * 4);
      memcpy(d + (size_t)(W - ox) * 4, s, (size_t)ox * 4);
    }
    /* Emulate a freshly acquired desktop frame: the pipeline swaps capture and
     * previous-source textures on entry and only keeps the new one when a
     * frame was acquired. Do that swap here, so the upload lands in the
     * texture the encoder reads and the previous one keeps frame N-1. */
    if (gpu->tern_has_prev_src) {
      ID3D11Texture2D *tt = gpu->cap_bgra_tex; gpu->cap_bgra_tex = gpu->tern_prev_src_tex; gpu->tern_prev_src_tex = tt;
      ID3D11RenderTargetView *tr = gpu->cap_rtv; gpu->cap_rtv = gpu->tern_prev_src_rtv; gpu->tern_prev_src_rtv = tr;
      ID3D11ShaderResourceView *ts = gpu->cap_bgra_srv; gpu->cap_bgra_srv = gpu->tern_prev_src_srv; gpu->tern_prev_src_srv = ts;
      gpu->tern_has_prev_src = false; /* no second swap, no copy-over inside */
    }
    ID3D11DeviceContext_UpdateSubresource(gpu->ctx, (ID3D11Resource *)gpu->cap_bgra_tex,
                                          0, NULL, frm, W * 4, 0);
    const uint8_t *bs = NULL;
    size_t len = 0;
    bool kf = false;
    double c = 0, e = 0, d = 0, p = 0;
    if (gpu_pipeline_capture_and_encode(gpu, fr, &bs, &len, &kf, &c, &e) != 0) continue;
    if (gpu_pipeline_decode_and_present(gpu, bs, len, &d, &p) != 0) dec_err++;
    {
      uint32_t hw = 0, mvb = 0;
      memcpy(&hw, bs, 4); memcpy(&mvb, bs + 4, 4);
      if (hw & TERN_EC_FLAG) {
        ec_frames++;
        uint32_t rw = hw & TERN_RECORD_WORDS_MASK;
        uint32_t got = tern_ec_unpack_records(gpu, bs + 8 + mvb, len - 8 - mvb, rt_w, rw, (hw & TERN_V2_FLAG) ? 9U : 8U);
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
            uint32_t g = tern_ec_unpack_records(gpu, fz, cut, rt_w, rw, (hw & TERN_V2_FLAG) ? 9U : 8U);
            if (g != UINT32_MAX && g > rw) rt_bad += 1000;
            fuzz_runs++;
          }
          free(fz);
        }
        else if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)gpu->tern_out_records_stg[(gpu->tern_stg_idx + TERN_STG_RING - 1U) % TERN_STG_RING], 0, D3D11_MAP_READ, 0, &m2))) {
          size_t la = tern_pack_records(gpu, (const uint32_t *)m2.pData, gpu->tern_last_raw_words / 2, rt_a);
          size_t lb = tern_pack_records(gpu, rt_w, rw / 2, rt_b);
          if (la != lb || memcmp(rt_a, rt_b, la) != 0) rt_bad++;
          ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)gpu->tern_out_records_stg[(gpu->tern_stg_idx + TERN_STG_RING - 1U) % TERN_STG_RING], 0);
        }
      }
    }
    if (fr >= 150U) {
      double psnr = 0;
      gpu_pipeline_ternary_compute_psnr(gpu, &psnr);
      psnr_sum += psnr;
      if (psnr < psnr_min) psnr_min = psnr;
      bytes += len;
      enc_sum += e; dec_sum += d; if (e > enc_max) enc_max = e;
      n++;
    }
    if (getenv("TRACE") && ((fr >= 118U && fr < 132U) || fr % 40U == 0U)) {
      D3D11_MAPPED_SUBRESOURCE mm = {0};
      uint32_t good = 0, zero = 0;
      if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)gpu->tern_enc_mvs_stg[(gpu->tern_stg_idx + TERN_STG_RING - 1U) % TERN_STG_RING], 0, D3D11_MAP_READ, 0, &mm))) {
        const int32_t *mv = (const int32_t *)mm.pData;
        uint32_t nt = gpu->tern_tiles_x * gpu->tern_tiles_y;
        for (uint32_t i = 0; i < nt; ++i) {
          if (mv[i*2] == -vx && mv[i*2+1] == -vy) good++;
          if (mv[i*2] == 0 && mv[i*2+1] == 0) zero++;
        }
        ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)gpu->tern_enc_mvs_stg[(gpu->tern_stg_idx + TERN_STG_RING - 1U) % TERN_STG_RING], 0);
      }
      double ps = 0; gpu_pipeline_ternary_compute_psnr(gpu, &ps);
      printf("f%3u len=%6zu good=%4u zero=%4u thr=%2u 2x=%d ierr=%3u static=%d psnr=%.1f enc=%.0fus\n", fr, len, good, zero,
             gpu->tern_rc_threshold, gpu->tern_rc_2x, gpu->tern_rc_intra_err_thr, gpu->tern_static_scene, ps, e); if (getenv("TRACE")) printf("      tq_qp=%u" "\n", gpu->tern_tq_qp); if (getenv("TRACE")) printf("      raw_words=%u ec_ratio=%u pack_ratio=%u kept=%u\n", gpu->tern_last_raw_words, gpu->tern_ec_ratio_q16, gpu->tern_pack_ratio_q16, gpu->tern_sort_kept);
    }
    if (getenv("BREAKDOWN") && fr >= 150U) {
      /* Split this frame's records by class and range-code each part alone. */
      D3D11_MAPPED_SUBRESOURCE m3 = {0};
      uint32_t hw = 0; memcpy(&hw, bs, 4);
      uint32_t rw = hw & TERN_RECORD_WORDS_MASK;
      ID3D11Resource *stg = (ID3D11Resource *)gpu->tern_out_records_stg[(gpu->tern_stg_idx + TERN_STG_RING - 1U) % TERN_STG_RING];
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
          size_t b = tern_ec_pack_records(gpu, part[k], n[k] / 2, n[k] / 2, rt_a, 64u << 20);
          bd_bytes[k] += b;
        }
        bd_frames++;
        ID3D11DeviceContext_Unmap(gpu->ctx, stg, 0);
      }
    }
    if (getenv("CHROMA") && fr >= 150U && fr % 10U == 0U) {
      static ID3D11Buffer *cst = NULL;
      if (!cst) {
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = W * H * 4; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
        ID3D11Device_CreateBuffer(gpu->dev, &bd, NULL, &cst);
      }
      ID3D11DeviceContext_CopyResource(gpu->ctx, (ID3D11Resource *)cst, (ID3D11Resource *)gpu->tern_enc_yuvd_buf[gpu->tern_enc_ping & 1U]);
      D3D11_MAPPED_SUBRESOURCE cm = {0};
      if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)cst, 0, D3D11_MAP_READ, 0, &cm))) {
        const uint32_t *ref = (const uint32_t *)cm.pData;
        double se_b = 0, se_r = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i) {
          double b = frm[i * 4], g = frm[i * 4 + 1], r = frm[i * 4 + 2];
          double cb = -0.169 * r - 0.331 * g + 0.5 * b + 128.0, cr = 0.5 * r - 0.419 * g - 0.081 * b + 128.0;
          int icb = (int)floor(cb + 0.5), icr = (int)floor(cr + 0.5);
          icb = icb < 0 ? 0 : icb > 255 ? 255 : icb; icr = icr < 0 ? 0 : icr > 255 ? 255 : icr;
          double db = (double)icb - (double)((ref[i] >> 8) & 0xFF), dr = (double)icr - (double)((ref[i] >> 16) & 0xFF);
          se_b += db * db; se_r += dr * dr;
        }
        ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)cst, 0);
        double npx = (double)W * H;
        cpsnr_sum += 0.5 * (10.0 * log10(255.0 * 255.0 / (se_b / npx + 1e-9)) + 10.0 * log10(255.0 * 255.0 / (se_r / npx + 1e-9)));
        cpsnr_n++;
      }
    }
    if (fr == 119U) {
      double psnr = 0;
      gpu_pipeline_ternary_compute_psnr(gpu, &psnr);
      printf("still psnr=%.2f  ", psnr);
    }
    if (fr + 1 == total) {
      D3D11_MAPPED_SUBRESOURCE mm = {0};
      if (SUCCEEDED(ID3D11DeviceContext_Map(gpu->ctx, (ID3D11Resource *)gpu->tern_enc_mvs_stg[(gpu->tern_stg_idx + TERN_STG_RING - 1U) % TERN_STG_RING], 0, D3D11_MAP_READ, 0, &mm))) {
        const int32_t *mv = (const int32_t *)mm.pData;
        uint32_t nt = gpu->tern_tiles_x * gpu->tern_tiles_y, exact = 0, zero = 0;
        for (uint32_t i = 0; i < nt; ++i) {
          if (mv[i*2] == vx && mv[i*2+1] == vy) exact++;
          if (mv[i*2] == 0 && mv[i*2+1] == 0) zero++;
        }
        printf("[mv exact=%u zero=%u of %u; first: ", exact, zero, nt);
        for (uint32_t i = 600; i < 612; ++i) printf("(%d,%d)", mv[i*2], mv[i*2+1]);
        puts("]");
        ID3D11DeviceContext_Unmap(gpu->ctx, (ID3D11Resource *)gpu->tern_enc_mvs_stg[(gpu->tern_stg_idx + TERN_STG_RING - 1U) % TERN_STG_RING], 0);
      }
      if (gpu_pipeline_ternary_ref_mismatch(gpu, &mm_total) != 0) dec_err++;
      if (dump) dump_bmp(gpu, dump);
    }
  }
  uint32_t denom = (n > 0U) ? n : 1U;
  printf("v=(%d,%d) %u Mbps: moving psnr avg=%.2f min=%.2f  %.1f kB/frame (%.1f Mbps)  mismatch=%llu\n",
         vx, vy, mbps, psnr_sum / denom, n ? psnr_min : 0.0, (double)bytes / denom / 1000.0,
         (double)bytes / denom * 8.0 * 120.0 / 1e6, (unsigned long long)mm_total);
  printf("   enc avg=%.0f max=%.0f us, dec avg=%.0f us; ec frames %u/%u, roundtrip errors %u, decode errors %u\n",
         enc_sum / denom, enc_max, dec_sum / denom, ec_frames, total, rt_bad, dec_err);
  if (bd_frames) printf("   per frame: delta %.1f kB (%.0f records, %.2f B each), intra %.1f kB (%.0f px, %.2f bits/px)\n",
      bd_bytes[0] / bd_frames / 1000.0, (double)bd_delta_rec / bd_frames, bd_bytes[0] / (bd_delta_rec ? bd_delta_rec : 1),
      bd_bytes[1] / bd_frames / 1000.0, (double)bd_intra_px / bd_frames, bd_bytes[1] * 8.0 / (bd_intra_px ? bd_intra_px : 1));
  if (bd_frames && bd_tq_blk) printf("   transform: chroma %.0f%% of coefs, %.0f blocks/frame, %.1f coefs/block, %.1f kB/frame, %.2f bits/px" "\n",
      100.0 * bd_tq_chroma / (bd_tq_rec ? bd_tq_rec : 1), (double)bd_tq_blk / bd_frames, (double)bd_tq_rec / bd_tq_blk, bd_bytes[2] / bd_frames / 1000.0, bd_bytes[2] * 8.0 / (bd_tq_blk * 16.0));
  if (cpsnr_n) printf("   chroma psnr (Cb/Cr mean) avg=%.2f" "\n", cpsnr_sum / cpsnr_n);
  if (fuzz_runs) printf("   fuzzed decodes: %u\n", fuzz_runs);
  gpu_pipeline_destroy(gpu);
  free(rt_w);
  free(rt_a);
  free(rt_b);
  free(img);
  free(frm);
  return (dec_err != 0U || rt_bad != 0U || mm_total != 0ULL) ? 1 : 0;
}
