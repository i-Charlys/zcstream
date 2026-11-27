/*
 * test_portable_pipeline.c - Verifies portable C11 Zcvid codec pipeline:
 *   - Multi-resolution encode + decode (320x240, 640x480)
 *   - Raw, plain-packed, CABAC v3, transform on/off, and keyframe resync
 *   - Closed-loop bit-exact parity (ref_mismatch == 0) & Y-PSNR >= 35.0 dB
 *   - Non-portable NVENC/Pyrowave calls cleanly fail with non-zero rc
 */

#include "gpu_pipeline_win32.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void run_sequence(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate_bps,
                         bool packed, bool entropy, bool transform, bool dct8,
                         uint32_t nframes) {
  gpu_pipeline_t *p = NULL;
  gpu_pipeline_info_t info;
  int rc = gpu_pipeline_create_zcvid(&p, w, h, fps, 6U, 4U, 256U, 128U, false, &info);
  assert(rc == 0 && p != NULL);
  assert(info.width == w && info.height == h);
  assert(gpu_pipeline_pump_events(p) == true);

  gpu_pipeline_set_bitrate(p, bitrate_bps);
  gpu_pipeline_force_synth(p, true);
  gpu_pipeline_zcvid_set_packed(p, packed);
  gpu_pipeline_zcvid_set_entropy(p, entropy);
  gpu_pipeline_zcvid_set_transform(p, transform);
  gpu_pipeline_zcvid_set_transform_inter(p, transform);
  gpu_pipeline_zcvid_set_dct8(p, dct8);
  gpu_pipeline_zcvid_set_qpel(p, true);

  double min_psnr = 999.0;
  for (uint32_t f = 0U; f < nframes; ++f) {
    if (f == nframes / 2U) {
      gpu_pipeline_set_bitrate(p, bitrate_bps / 2U);
      gpu_pipeline_zcvid_request_keyframe(p);
    }
    const uint8_t *bs = NULL;
    size_t len = 0U;
    bool kf = false;
    double cap_us = 0.0, enc_us = 0.0;
    rc = gpu_pipeline_capture_and_encode(p, f, &bs, &len, &kf, &cap_us, &enc_us);
    assert(rc == 0 && bs != NULL && len >= 8U);
    if (f == 0U || f == nframes / 2U) assert(kf == true);

    double dec_us = 0.0, prs_us = 0.0;
    rc = gpu_pipeline_decode_and_present(p, bs, len, &dec_us, &prs_us);
    assert(rc == 0);

    uint64_t mismatch = UINT64_MAX;
    assert(gpu_pipeline_zcvid_ref_mismatch(p, &mismatch) == 0);
    if (mismatch != 0ULL) {
      fprintf(stderr,
              "FAIL (%ux%u packed=%d ec=%d tq=%d dct8=%d f=%u): ref_mismatch=%llu\n",
              w, h, packed, entropy, transform, dct8, f, (unsigned long long)mismatch);
      abort();
    }

    double psnr = 0.0;
    assert(gpu_pipeline_zcvid_compute_psnr(p, &psnr) == 0);
    if (psnr < min_psnr) min_psnr = psnr;
  }

  printf("  [OK] %ux%u | packed=%d ec=%d tq=%d dct8=%d | %u frames | ref_mismatch=0 | min_psnr=%.2f dB\n",
         w, h, packed ? 1 : 0, entropy ? 1 : 0, transform ? 1 : 0, dct8 ? 1 : 0, nframes, min_psnr);
  fflush(stdout);
  if (min_psnr < 35.0) {
    fprintf(stderr, "FAIL: min_psnr=%.2f dB < 35.0 dB\n", min_psnr);
    abort();
  }

  uint64_t hf = 0U, hm = 0U, hmt = 0U, hst = 0U;
  gpu_pipeline_zcvid_hint_stats(p, &hf, &hm, &hmt, &hst);
  assert(hf == nframes);
  assert(gpu_pipeline_zcvid_entropy_budget(p) >= 2048U);
  gpu_pipeline_destroy(p);
}

static void run_dct8_downgrade_and_mixed_intra(void) {
  /* 1. High-bitrate intra-only on non-multiple-of-8 324x244 forces mixed 8x8 DCT8
   *    intra (pred=4, low QP) + 4x4 intra (pred=0) in the same CABAC v4 stream,
   *    verifying separated 8x8 vs 4x4 intra DC DPCM state (no egk_max=8 overflow).
   * 2. Mid-stream dynamic ZCEC downgrade: DCT8 v4 -> legacy CABAC v3 + keyframe. */
  gpu_pipeline_t *p = NULL;
  gpu_pipeline_info_t info;
  int rc = gpu_pipeline_create_zcvid(&p, 324U, 244U, 60U, 6U, 4U, 256U, 128U, false, &info);
  assert(rc == 0 && p != NULL);
  gpu_pipeline_set_bitrate(p, 60000000U);
  gpu_pipeline_force_synth(p, true);
  gpu_pipeline_zcvid_set_packed(p, true);
  gpu_pipeline_zcvid_set_entropy(p, true);
  gpu_pipeline_zcvid_set_transform(p, true);
  gpu_pipeline_zcvid_set_transform_inter(p, false);
  gpu_pipeline_zcvid_set_dct8(p, true);
  gpu_pipeline_zcvid_set_qpel(p, true);

  double min_psnr = 999.0;
  for (uint32_t f = 0U; f < 12U; ++f) {
    if (f < 6U) {
      /* Force keyframes at low QP (60 Mbps) on 324x244 so every frame mixes
       * 8x8 DCT8 intra (pred=4) and 4x4 intra (pred=0) in CABAC v4. */
      gpu_pipeline_zcvid_request_keyframe(p);
    } else if (f == 6U) {
      /* Simulate ZCEC caps without bit 3 (legacy v3 peer) */
      gpu_pipeline_zcvid_set_dct8(p, false);
      gpu_pipeline_zcvid_set_transform_inter(p, true);
      gpu_pipeline_zcvid_request_keyframe(p);
    }
    const uint8_t *bs = NULL;
    size_t len = 0U;
    bool kf = false;
    double cap_us = 0.0, enc_us = 0.0, dec_us = 0.0, prs_us = 0.0;
    rc = gpu_pipeline_capture_and_encode(p, f, &bs, &len, &kf, &cap_us, &enc_us);
    assert(rc == 0 && bs != NULL && len >= 8U);

    uint32_t hdr_word = 0U;
    memcpy(&hdr_word, bs, sizeof(hdr_word));
    if (f < 6U) {
      assert((hdr_word & 0x01000000U) != 0U); /* ZCVID_TQ_V4_FLAG set */
    } else {
      assert((hdr_word & 0x01000000U) == 0U); /* ZCVID_TQ_V4_FLAG cleared for v3 */
    }

    rc = gpu_pipeline_decode_and_present(p, bs, len, &dec_us, &prs_us);
    assert(rc == 0);
    uint64_t mismatch = UINT64_MAX;
    assert(gpu_pipeline_zcvid_ref_mismatch(p, &mismatch) == 0);
    assert(mismatch == 0ULL);
    double psnr = 0.0;
    assert(gpu_pipeline_zcvid_compute_psnr(p, &psnr) == 0);
    if (psnr < min_psnr) min_psnr = psnr;
  }
  assert(min_psnr >= 35.0);
  printf("  [OK] 324x244 | mixed-intra low-QP + mid-stream v4->v3 downgrade | 12 frames | ref_mismatch=0 | min_psnr=%.2f dB\n",
         min_psnr);
  fflush(stdout);
  gpu_pipeline_destroy(p);
}

int main(void) {
  printf("Running test_portable_pipeline...\n");

  /* NVENC & Pyrowave must fail gracefully with NULL pipeline on portable backend */
  gpu_pipeline_t *nv = (gpu_pipeline_t *)(uintptr_t)0x1;
  gpu_pipeline_info_t info;
  assert(gpu_pipeline_create(&nv, 320, 240, 60, 10000000U, false, &info) != 0);
  assert(nv == NULL);
  gpu_pipeline_t *pw = (gpu_pipeline_t *)(uintptr_t)0x1;
  assert(gpu_pipeline_create_pyrowave(&pw, 320, 240, 60, 10000000U, false, &info) != 0);
  assert(pw == NULL);

  /* CABAC v4 (DCT8=1) + Transform + Packed at 320x240, 640x480, and non-multiple-of-8 324x244 */
  run_sequence(320U, 240U, 60U, 12000000U, true,  true,  true,  true,  16U);
  run_sequence(640U, 480U, 120U, 25000000U, true, true,  true,  true,  12U);
  run_sequence(324U, 244U, 60U, 12000000U, true,  true,  true,  true,  8U);

  /* Legacy CABAC v3 (DCT8=0) backward-compatible wire format */
  run_sequence(320U, 240U, 60U, 12000000U, true,  true,  true,  false, 12U);

  /* Plain-packed + Transform (no entropy), both DCT8 v4 and legacy v3 */
  run_sequence(320U, 240U, 60U, 15000000U, true,  false, true,  true,  8U);
  run_sequence(320U, 240U, 60U, 15000000U, true,  false, true,  false, 8U);

  /* Raw uncompressed records + Transform (DCT8 v4) */
  run_sequence(320U, 240U, 60U, 20000000U, false, false, true,  true,  8U);

  /* Fallback class-6 (transform off) with CABAC */
  run_sequence(320U, 240U, 60U, 20000000U, true,  true,  false, false, 6U);

  /* Mixed 8x8 DCT8 + 4x4 intra at low QP + dynamic mid-stream v4->v3 downgrade */
  run_dct8_downgrade_and_mixed_intra();

  printf("All test_portable_pipeline checks passed!\n");
  return 0;
}
