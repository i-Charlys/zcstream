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

static void run_sequence(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate_bps,
                         bool packed, bool entropy, bool transform, uint32_t nframes) {
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
              "FAIL (%ux%u packed=%d ec=%d tq=%d f=%u): ref_mismatch=%llu\n",
              w, h, packed, entropy, transform, f, (unsigned long long)mismatch);
      abort();
    }

    double psnr = 0.0;
    assert(gpu_pipeline_zcvid_compute_psnr(p, &psnr) == 0);
    if (psnr < min_psnr) min_psnr = psnr;
  }

  printf("  [OK] %ux%u | packed=%d ec=%d tq=%d | %u frames | ref_mismatch=0 | min_psnr=%.2f dB\n",
         w, h, packed ? 1 : 0, entropy ? 1 : 0, transform ? 1 : 0, nframes, min_psnr);
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

  /* CABAC v3 + Transform + Packed at 320x240 and 640x480 */
  run_sequence(320U, 240U, 60U, 12000000U, true,  true,  true,  16U);
  run_sequence(640U, 480U, 120U, 25000000U, true, true,  true,  12U);

  /* Plain-packed + Transform (no entropy) */
  run_sequence(320U, 240U, 60U, 15000000U, true,  false, true,  8U);

  /* Raw uncompressed records + Transform */
  run_sequence(320U, 240U, 60U, 20000000U, false, false, true,  8U);

  /* Fallback class-6 (transform off) with CABAC */
  run_sequence(320U, 240U, 60U, 20000000U, true,  true,  false, 6U);

  printf("All test_portable_pipeline checks passed!\n");
  return 0;
}
