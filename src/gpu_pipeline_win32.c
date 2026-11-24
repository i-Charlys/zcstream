#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#ifdef ZC_HAVE_PYROWAVE
#include <d3d11_4.h>
#endif
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <math.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <initguid.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "gpu_pipeline_win32.h"
#include "protocol_zcvid.h"
#include "zcvid_entropy.h"
#ifdef ZC_HAVE_NVENC
#include "nvEncodeAPI.h"
#endif
#ifdef ZC_HAVE_PYROWAVE
#include "pyrowave_gpu_bridge.h"
#endif

static FILE *g_gpu_log_file = NULL;

void gpu_pipeline_set_log_file(void *stdio_file) {
  g_gpu_log_file = (FILE *)stdio_file;
}

static void gpu_log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
  if (g_gpu_log_file) {
    va_list ap2;
    va_start(ap2, fmt);
    vfprintf(g_gpu_log_file, fmt, ap2);
    va_end(ap2);
    fflush(g_gpu_log_file);
  }
}

/* Ensure key MF / CodecAPI GUIDs are defined even on older MinGW headers */
DEFINE_GUID(ZC_MF_MT_MAJOR_TYPE, 0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
DEFINE_GUID(ZC_MF_MT_SUBTYPE, 0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
DEFINE_GUID(ZC_MF_MT_FRAME_SIZE, 0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d);
DEFINE_GUID(ZC_MF_MT_FRAME_RATE, 0xc459a2e8, 0x3d2c, 0x4e44, 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0);
DEFINE_GUID(ZC_MF_MT_AVG_BITRATE, 0x20332624, 0xfb0d, 0x4d9e, 0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e);
DEFINE_GUID(ZC_MF_MT_INTERLACE_MODE, 0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4f, 0xdc);
DEFINE_GUID(ZC_MFMediaType_Video, 0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(ZC_MFVideoFormat_H264, 0x34363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(ZC_MFVideoFormat_NV12, 0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(ZC_CLSID_MSH264DecoderMFT, 0x62CE7E72, 0x4C71, 0x4d20, 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D);
DEFINE_GUID(ZC_CODECAPI_AVLowLatencyMode, 0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);
DEFINE_GUID(ZC_CODECAPI_AVEncMPVDefaultBPictureCount, 0x8d390aac, 0xdc5c, 0x4200, 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2);
DEFINE_GUID(ZC_CODECAPI_AVEncCommonMeanBitRate, 0xf7222374, 0x2144, 0x4815, 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52);
DEFINE_GUID(ZC_IID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

#ifndef DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 2048
#endif

static LARGE_INTEGER g_qpc_freq;

uint64_t qpc_now_us(void) {
  if (g_qpc_freq.QuadPart == 0) {
    QueryPerformanceFrequency(&g_qpc_freq);
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (uint64_t)((now.QuadPart * 1000000LL) / g_qpc_freq.QuadPart);
}

static double qpc_delta_us(LARGE_INTEGER t0, LARGE_INTEGER t1) {
  return ((double)(t1.QuadPart - t0.QuadPart) * 1000000.0) /
         (double)g_qpc_freq.QuadPart;
}

static HRESULT map_read_sync(ID3D11DeviceContext *ctx, ID3D11Resource *res,
                             D3D11_MAPPED_SUBRESOURCE *mr) {
  HRESULT hr = ID3D11DeviceContext_Map(ctx, res, 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, mr);
  if (hr != DXGI_ERROR_WAS_STILL_DRAWING) return hr;
  ID3D11DeviceContext_Flush(ctx);
  uint64_t t0 = qpc_now_us();
  for (;;) {
    hr = ID3D11DeviceContext_Map(ctx, res, 0, D3D11_MAP_READ,
                                 D3D11_MAP_FLAG_DO_NOT_WAIT, mr);
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING) return hr;
    uint64_t elapsed = qpc_now_us() - t0;
    if (elapsed >= 2000ULL) {
      return ID3D11DeviceContext_Map(ctx, res, 0, D3D11_MAP_READ, 0, mr);
    }
    if (elapsed >= 800ULL) {
      SwitchToThread();
    } else {
      YieldProcessor();
    }
  }
}

/* The range coder cuts a frame into at most this many chunks, work-stolen across threads. */
#define ZCVID_EC_MAX_CHUNKS 32U
#define ZCVID_STG_RING 3U

/* Display deblocking threshold (CSYCbCrToRGBA), 0 = off. */
#ifndef ZCVID_POST_THR
#define ZCVID_POST_THR 6U
#endif

/* 1: the rate control never halves the horizontal resolution of deltas. */
#ifndef ZCVID_RC_NO_2X
#define ZCVID_RC_NO_2X 0
#endif

/* Frames in a row under half the budget before the 2x deltas are dropped.
 * Leaving 2x after one cheap frame made the controller flip every frame on
 * still content (a 2x frame is cheap, the 1x frame after it pays for all the
 * detail 2x left), and where it settled depended on GPU scheduling. */
#ifndef ZCVID_RC_2X_CALM
#define ZCVID_RC_2X_CALM 3U
#endif

/* Transform intra qp = ZCVID_TQ_QP_BASE + 2 * threshold (clamped 12..51 in the shader). */
#ifndef ZCVID_TQ_QP_BASE
#define ZCVID_TQ_QP_BASE 16U
#endif
/* Transform inter takes a block whose mean |luma residual| after motion
 * compensation exceeds ZCVID_TQ_INTER_BASE + ZCVID_TQ_INTER_PER_THR * threshold,
 * and only if the coded residual brings it clearly closer to the source (the
 * shader checks). 0 + 0: every block where the transform pays, the hybrid
 * mode; the zcvid deltas refine the rest. */
#ifndef ZCVID_TQ_INTER_BASE
#define ZCVID_TQ_INTER_BASE 0U
#endif
#ifndef ZCVID_TQ_INTER_PER_THR
#define ZCVID_TQ_INTER_PER_THR 0U
#endif

#ifndef ZCVID_TQ_QP_PER_THR
#define ZCVID_TQ_QP_PER_THR 2U
#endif
/* 1: the transform qp follows its own loop (start / floor below). */
#ifndef ZCVID_TQ_ADAPTIVE
#define ZCVID_TQ_ADAPTIVE 1
#endif
/* The transform qp loop: coarser only when a frame asks for more than
 * twice the budget, finer as soon as it fits. The delta threshold absorbs
 * smaller overshoots; the transform, being the cheaper way to fix a block,
 * keeps a fine qp as long as it can. */
#ifndef ZCVID_TQ_UP_PCT
#define ZCVID_TQ_UP_PCT 200U
#endif
#ifndef ZCVID_TQ_DOWN_PCT
#define ZCVID_TQ_DOWN_PCT 100U
#endif
#ifndef ZCVID_TQ_QP_START
#define ZCVID_TQ_QP_START 24U
#endif
#ifndef ZCVID_TQ_QP_MIN
#define ZCVID_TQ_QP_MIN 6U
#endif

/* Chroma qp of the transform intra = luma qp + ZCVID_TQ_CQP. */
#ifndef ZCVID_TQ_CQP
#define ZCVID_TQ_CQP 3U
#endif

/* Lowest threshold / intra trigger the rate control goes to when the frames
 * stay well under budget (encoder-only: the decoder never sees either). */
#ifndef ZCVID_RC_MIN_THRESHOLD
#define ZCVID_RC_MIN_THRESHOLD 2U
#endif
#ifndef ZCVID_RC_MIN_INTRA_ERR
#define ZCVID_RC_MIN_INTRA_ERR 16U
#endif

struct zcvid_pool_s;

struct gpu_pipeline_s {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  uint32_t bitrate_bps;

  HWND hwnd;
  bool show_window;
  bool window_closed;
  gpu_input_cb_t input_cb;
  void *input_ud;
  bool mouse_relative;
  bool mouse_locked;
  uint32_t input_seq;
  bool cursor_hidden;
  bool tearing_supported;
  bool using_desktop_dup;
  bool dupl_has_frame; /* cap_bgra_tex holds a duplicated desktop frame */
  IDXGIOutput1 *dupl_out1; /* output to (re)duplicate; NULL = never retry */
  uint64_t dupl_retry_us;  /* last time duplication was attempted */
  ID3D11Texture2D *dupl_stage_tex; /* desktop frame of another size/format, before scaling */
  uint32_t dupl_stage_w, dupl_stage_h;
  DXGI_FORMAT dupl_stage_fmt;

  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  ID3D11DeviceContext1 *ctx1;
  bool fallback_bg_init;
  ID3D11VideoDevice *vdev;
  ID3D11VideoContext *vctx;
  ID3D11VideoProcessorEnumerator *vp_enum;
  ID3D11VideoProcessor *vp;
  IDXGISwapChain1 *swapchain;
  IDXGIOutputDuplication *dupl;

  ID3D11Texture2D *cap_bgra_tex;
  ID3D11RenderTargetView *cap_rtv;
  ID3D11Texture2D *enc_nv12_tex;

  IMFDXGIDeviceManager *dxgi_mgr;
  UINT dxgi_reset_token;
  IMFTransform *enc_mft;
  IMFMediaEventGenerator *enc_ev;
  IMFTransform *dec_mft;
  uint64_t dec_frame_idx;

  /* Direct NVENC (nvEncodeAPI64.dll) Zero-Copy ARGB state */
  bool use_direct_nvenc;
#ifdef ZC_HAVE_NVENC
  HMODULE nvenc_dll;
  NV_ENCODE_API_FUNCTION_LIST nvenc_fn;
  void *nvenc_session;
  NV_ENC_REGISTERED_PTR nvenc_reg_res;
  NV_ENC_OUTPUT_PTR nvenc_bitstream;
  NV_ENC_INITIALIZE_PARAMS nvenc_init_params;
  NV_ENC_CONFIG nvenc_config;
#endif

  uint8_t *enc_out_buf;
  size_t enc_out_cap;

  /* Zcvid HLSL Compute (cs_5_0) zero-copy VRAM state */
  bool use_zcvid;
  uint32_t zcvid_threshold;
  uint32_t zcvid_delta_min;
  uint32_t zcvid_delta_max;
  uint32_t zcvid_gamma_q8;
  uint32_t zcvid_neutral_y;
  uint32_t zcvid_total_blocks;
  uint32_t zcvid_tiles_x;
  uint32_t zcvid_tiles_y;
  uint32_t zcvid_max_record_words;
  uint32_t zcvid_enc_ping;
  uint32_t zcvid_dec_ping;
  uint32_t zcvid_rc_threshold;
  uint32_t zcvid_rc_intra_err_thr;
  uint32_t zcvid_kf_countdown;
  bool zcvid_packed;            /* emit the compact bitstream (see zcvid_pack_records) */
  uint32_t zcvid_pack_ratio_q16; /* packed bytes per raw record byte, running average */
  uint32_t zcvid_pack_mv_bytes;  /* packed vector bytes of the previous frame */
  uint64_t *zcvid_sort_a;        /* radix sort scratch, zcvid_sort_cap entries each */
  uint64_t *zcvid_sort_b;
  uint32_t zcvid_sort_cap;
  bool zcvid_ec;                 /* range-code the packed records (see zcvid_ec_pack_records) */
  bool zcvid_ec_frame;           /* rate control sized this frame for the range coder */
  uint32_t zcvid_ec_ratio_q16;   /* range-coded bytes per raw record byte, running average */
  uint32_t zcvid_ec_threads;     /* chunks coded side by side, 1..ZCVID_EC_MAX_CHUNKS */
  uint32_t zcvid_ec_enc_nsw_q8;  /* measured coding time, ns per record word (Q8) */
  uint32_t zcvid_ec_dec_nsw_q8;  /* measured decoding time, same unit */
  uint32_t zcvid_ec_peer_limit;  /* record words per frame the decoder asked for, 0 = any */
  struct zcvid_pool_s *zcvid_pool; /* their worker threads, created on first use */
  /* Per-chunk coder scratch: */
  uint8_t *zcvid_ec_line[ZCVID_EC_MAX_CHUNKS];  /* intra line buffers, 3 bytes per column */
  uint32_t *zcvid_ec_lrow[ZCVID_EC_MAX_CHUNKS]; /* gen + row of the sample of each column */
  uint32_t zcvid_ec_lrow_gen[ZCVID_EC_MAX_CHUNKS]; /* monotonic row-tag base (avoids per-frame memset) */
  void *zcvid_ec_model[ZCVID_EC_MAX_CHUNKS];    /* zcrudp_tec_model_t */
  uint8_t *zcvid_ec_scratch;     /* chunk streams before they are laid end to end */
  size_t zcvid_ec_scratch_cap;
  bool zcvid_rc_2x;
  uint32_t zcvid_rc_2x_calm;     /* consecutive frames under half the budget in 2x */
  bool zcvid_intra8;             /* rolling refresh in 8-bit 2-px intra (budget to spare) */
  bool zcvid_tq;                 /* transform intra on 4x4 blocks (CSIntraT / CSITrans) */
  bool zcvid_tq_inter;           /* ... and transform-coded motion residual */
  bool zcvid_qpel;               /* tile vectors in quarter pels (wide +-60 px range) */
  bool zcvid_dither;             /* ordered dither on the 6-bit rolling intra */
  uint32_t zcvid_tq_qp;          /* transform qp, its own rate loop (ZCVID_TQ_ADAPTIVE) */
  uint32_t zcvid_tq_inter_thr;   /* adaptive mean |luma residual| gate for transform blocks */
  uint32_t zcvid_dec_post_qp;    /* smoothed transform QP for display deblocking */
  uint32_t zcvid_cut_burst;      /* remaining scene-cut VBV burst frames (0 = normal) */
  uint32_t zcvid_cut_cooldown;   /* remaining frames before another GPU scene-cut burst is allowed */
  uint32_t zcvid_ncls;           /* record classes in the packed layouts being written */
  uint32_t zcvid_sort_kept;      /* records the last sort kept (unused slots dropped) */
  uint32_t zcvid_last_raw_words; /* record words read back for the last frame, before packing */
  uint32_t zcvid_tq_blocks;      /* 4x4 blocks in a frame */
  ID3D11ComputeShader *zcvid_cs_motion_pred;   /* single-pass MC + decay into pred_yuvd & cur_yuvd */
  ID3D11Buffer *zcvid_enc_pred_buf;
  ID3D11UnorderedAccessView *zcvid_enc_pred_uav;
  ID3D11ShaderResourceView *zcvid_enc_pred_srv;
  ID3D11ComputeShader *zcvid_cs_intra_t;
  ID3D11ComputeShader *zcvid_cs_intra_t_count; /* what each 4x4 block wants */
  ID3D11ComputeShader *zcvid_cs_tq_scan_a;     /* budget: group bases of the demand */
  ID3D11ComputeShader *zcvid_cs_tq_mid;        /* budget: first rule, accepted words */
  ID3D11ComputeShader *zcvid_cs_tq_clear;      /* budget: zero 24 tail hist/misc words */
  ID3D11ComputeShader *zcvid_cs_tq_final;      /* budget: starts the record counter */
  ID3D11Buffer *zcvid_tq_scratch_buf;          /* see g_tq_scratch */
  ID3D11UnorderedAccessView *zcvid_tq_scratch_uav;
  ID3D11Buffer *zcvid_tq_stage_buf;            /* transform records before the budget */
  ID3D11UnorderedAccessView *zcvid_tq_stage_uav;
  ID3D11ComputeShader *zcvid_cs_itrans;
  ID3D11Buffer *zcvid_enc_tflag_buf;
  ID3D11UnorderedAccessView *zcvid_enc_tflag_uav;
  ID3D11ShaderResourceView *zcvid_enc_tflag_srv;
  ID3D11Buffer *zcvid_dec_coef_buf;
  ID3D11UnorderedAccessView *zcvid_dec_coef_uav;
  ID3D11Buffer *zcvid_dec_tflag_buf;
  ID3D11UnorderedAccessView *zcvid_dec_tflag_uav;
  bool zcvid_static_scene;
  uint32_t zcvid_static_sweep;        /* remaining frames of paced 8-bit static polish */
  uint32_t zcvid_static_sweep_cycles; /* cycle count sized to fit target_words */
  bool zcvid_dec_use_copy;
  ID3D11ComputeShader *zcvid_cs_tile_mv;
  ID3D11ComputeShader *zcvid_cs_mv_smooth;
  ID3D11ComputeShader *zcvid_cs_encode;
  ID3D11ComputeShader *zcvid_cs_motion_decay;
  ID3D11ComputeShader *zcvid_cs_apply;
  ID3D11ComputeShader *zcvid_cs_ycbcr_to_rgba;
  ID3D11ShaderResourceView *cap_bgra_srv;
  ID3D11Texture2D *zcvid_prev_src_tex; /* previous captured frame (motion search) */
  ID3D11RenderTargetView *zcvid_prev_src_rtv;
  ID3D11ShaderResourceView *zcvid_prev_src_srv;
  bool zcvid_has_prev_src;
  ID3D11Texture2D *zcvid_half_tex[2]; /* half-res luma, current / previous frame */
  ID3D11ShaderResourceView *zcvid_half_srv[2];
  ID3D11UnorderedAccessView *zcvid_half_uav[2];
  ID3D11Texture2D *zcvid_quarter_tex[2]; /* quarter-res luma, same ping-pong */
  ID3D11ShaderResourceView *zcvid_quarter_srv[2];
  ID3D11UnorderedAccessView *zcvid_quarter_uav[2];
  ID3D11ComputeShader *zcvid_cs_luma_quarter;
  uint32_t zcvid_half_ping;
  ID3D11ComputeShader *zcvid_cs_luma_half;
  ID3D11Buffer *zcvid_enc_yuvd_buf[2];
  ID3D11ShaderResourceView *zcvid_enc_yuvd_srv[2];
  ID3D11UnorderedAccessView *zcvid_enc_yuvd_uav[2];
  ID3D11Buffer *zcvid_enc_mvs_buf[2];
  ID3D11Buffer *zcvid_hint_buf;        /* capture hints per tile, see build_capture_hints */
  ID3D11ShaderResourceView *zcvid_hint_srv;
  int32_t *zcvid_hint_cpu;             /* 4 ints per tile */
  uint8_t *dupl_meta;                 /* move / dirty rects of the last acquired frame */
  UINT dupl_meta_cap;
  /* DXGI Desktop Duplication hardware cursor state */
  uint8_t *cursor_shape;
  UINT cursor_shape_cap;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO cursor_info;
  bool cursor_has_shape;
  bool cursor_visible;
  int32_t cursor_x;
  int32_t cursor_y;
  int32_t cursor_prev_x;
  int32_t cursor_prev_y;
  bool cursor_prev_visible;
  ID3D11Texture2D *cursor_stg;
  uint32_t cursor_stg_w;
  uint32_t cursor_stg_h;
  bool zcvid_hints_static;             /* no new desktop frame: every tile unchanged */
  bool zcvid_hints_set;                /* zcvid_hint_cpu holds this frame's hints */
  int zcvid_hint_gpu_state;            /* -1 unknown/custom, 0 all-zero, 2 all-static */
  uint64_t hint_frames, hint_moved_tiles, hint_static_tiles, hint_move_rects; /* counters */
  ID3D11ShaderResourceView *zcvid_enc_mvs_srv[2];
  ID3D11UnorderedAccessView *zcvid_enc_mvs_uav[2];
  uint32_t zcvid_mv_ping;
  ID3D11Buffer *zcvid_enc_mvs_stg[ZCVID_STG_RING];
  ID3D11Buffer *zcvid_out_records_buf;
  ID3D11UnorderedAccessView *zcvid_out_records_uav;
  ID3D11Buffer *zcvid_out_records_stg[ZCVID_STG_RING];
  ID3D11Buffer *zcvid_out_counter_buf;
  ID3D11UnorderedAccessView *zcvid_out_counter_uav;
  ID3D11Buffer *zcvid_out_counter_stg[ZCVID_STG_RING];
  bool zcvid_prof_en;
  ID3D11Query *zcvid_prof_disj;
  ID3D11Query *zcvid_prof_ts[8];
  uint32_t zcvid_prof_count;
  double zcvid_prof_acc[11]; /* 0:luma 1:mv 2:pred 3:tq 4:enc 5:dma 6:wait_cpu 7:mv_cpu 8:ec_cpu 9:total 10:max */
  uint32_t zcvid_dec_prof_count;
  double zcvid_dec_prof_acc[6]; /* 0:mv_dec 1:mv_up 2:ec_dec 3:rec_up 4:dispatch 5:total */
  double zcvid_ec_prof_acc[8];  /* 0:sort 1:enc_pool 2:enc_max_c 3:dec_max_c 4:dec_min_c 5:c05 6:c67 7:c8 */
  uint32_t zcvid_ec_prof_enc_n;
  uint32_t zcvid_ec_prof_dec_n;
  uint32_t zcvid_ec_prof_chunks;
  uint32_t zcvid_stg_idx;
  ID3D11Buffer *zcvid_dec_yuvd_buf[2];
  ID3D11ShaderResourceView *zcvid_dec_yuvd_srv[2];
  ID3D11UnorderedAccessView *zcvid_dec_yuvd_uav[2];
  ID3D11Buffer *zcvid_dec_mvs_buf;
  ID3D11ShaderResourceView *zcvid_dec_mvs_srv;
  ID3D11Buffer *zcvid_in_records_buf;
  ID3D11ShaderResourceView *zcvid_in_records_srv;
  uint32_t *zcvid_dec_unpack_buf;
  size_t zcvid_dec_unpack_cap;
  int32_t *zcvid_dec_mv_scratch;
  uint32_t zcvid_dec_mv_cap;
  bool zcvid_dec_mvs_zero;
  bool zcvid_dec_has_tq;
  uint32_t zcvid_dma_spec_words;
  ID3D11Texture2D *zcvid_dec_tex;
  ID3D11UnorderedAccessView *zcvid_dec_uav;
  ID3D11ShaderResourceView *zcvid_dec_srv;
  ID3D11ComputeShader *zcvid_cs_psnr;
  ID3D11Buffer *zcvid_psnr_cb;
  ID3D11Buffer *zcvid_psnr_buf;
  ID3D11UnorderedAccessView *zcvid_psnr_uav;
  ID3D11Buffer *zcvid_psnr_stg;
  ID3D11Buffer *zcvid_enc_cb;
  ID3D11Buffer *zcvid_dec_cb;

  /* Pyrowave (Vulkan Wavelet GPU) zero-copy D3D11<->Vulkan pipeline state */
  bool use_pyrowave;
#ifdef ZC_HAVE_PYROWAVE
  zc_pyro_ctx_t *pyro;
  ID3D11Texture2D *pyro_shared_bgra;
  HANDLE pyro_shared_tex_handle;
  ID3D11Texture2D *pyro_dec_shared_tex[3];
  ID3D11ShaderResourceView *pyro_dec_shared_srv[3];
  HANDLE pyro_dec_shared_handle[3];
  ID3D11Device5 *pyro_dev5;
  ID3D11DeviceContext4 *pyro_ctx4;
  ID3D11Fence *pyro_fence;
  HANDLE pyro_fence_handle;
  uint64_t pyro_fence_val;
  ID3D11Texture2D *pyro_nv12_stg;
  uint8_t *pyro_dec_y;
  uint8_t *pyro_dec_cb;
  uint8_t *pyro_dec_cr;
  ID3D11Texture2D *pyro_y_tex;
  ID3D11Texture2D *pyro_cb_tex;
  ID3D11Texture2D *pyro_cr_tex;
  ID3D11ShaderResourceView *pyro_y_srv;
  ID3D11ShaderResourceView *pyro_cb_srv;
  ID3D11ShaderResourceView *pyro_cr_srv;
  ID3D11Texture2D *pyro_dec_tex;
  ID3D11UnorderedAccessView *pyro_dec_uav;
  ID3D11Buffer *pyro_dec_cbuf;
  ID3D11ComputeShader *pyro_cs_yuv2rgba;
  bool pyro_dec_use_copy;
#endif
};

static uint32_t g_display_idx = 0U;

void gpu_pipeline_set_display_index(uint32_t display_idx) {
  g_display_idx = display_idx;
}

static void win_set_mouse_lock(gpu_pipeline_t *p, bool lock) {
  if (!p || !p->hwnd) return;
  if (lock) {
    RECT rc;
    if (GetClientRect(p->hwnd, &rc)) {
      POINT tl = {rc.left, rc.top};
      POINT br = {rc.right, rc.bottom};
      ClientToScreen(p->hwnd, &tl);
      ClientToScreen(p->hwnd, &br);
      RECT scr = {tl.x, tl.y, br.x, br.y};
      ClipCursor(&scr);
    }
    if (!p->cursor_hidden) {
      ShowCursor(FALSE);
      p->cursor_hidden = true;
    }
    p->mouse_locked = true;
    SetWindowTextA(p->hwnd, "zcstream — Live Receiver [Souris FPS verrouillée | Ctrl+Alt+Shift+M pour libérer]");
  } else {
    ClipCursor(NULL);
    if (p->cursor_hidden) {
      ShowCursor(TRUE);
      p->cursor_hidden = false;
    }
    p->mouse_locked = false;
    SetWindowTextA(p->hwnd, "zcrudp + D3D11 Zero-Copy Video Pipeline");
  }
}

void gpu_pipeline_set_input_callback(gpu_pipeline_t *pipe, gpu_input_cb_t cb, void *userdata) {
  if (!pipe) return;
  pipe->input_cb = cb;
  pipe->input_ud = userdata;
}

void gpu_pipeline_set_mouse_relative(gpu_pipeline_t *pipe, bool relative) {
  if (!pipe) return;
  pipe->mouse_relative = relative;
  if (!relative && pipe->mouse_locked) {
    win_set_mouse_lock(pipe, false);
  }
}

bool gpu_pipeline_get_mouse_relative(gpu_pipeline_t *pipe) {
  return pipe ? pipe->mouse_relative : false;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  gpu_pipeline_t *p = (gpu_pipeline_t *)(LONG_PTR)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
  if (msg == WM_DESTROY || msg == WM_CLOSE) {
    if (p) {
      if (p->mouse_locked) win_set_mouse_lock(p, false);
      p->window_closed = true;
    }
    PostQuitMessage(0);
    return 0;
  }
  if (msg == WM_KILLFOCUS) {
    if (p && p->mouse_locked) win_set_mouse_lock(p, false);
    return 0;
  }
  if (!p || !p->input_cb) {
    return DefWindowProcA(hwnd, msg, wp, lp);
  }

  switch (msg) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
      bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
      bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (down && ctrl && alt && shift) {
        if (wp == 'M') {
          p->mouse_relative = !p->mouse_relative;
          win_set_mouse_lock(p, p->mouse_relative);
          return 0;
        }
        if (wp == 'Q') {
          if (p->mouse_locked) win_set_mouse_lock(p, false);
          p->window_closed = true;
          PostQuitMessage(0);
          return 0;
        }
      }
      zc_input_event_t ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind = ZC_INPUT_KEY;
      ev.flags = (uint8_t)(down ? ZC_INPUT_FLAG_DOWN : 0U);
      if (((lp >> 24) & 1L) != 0L) ev.flags |= ZC_INPUT_FLAG_EXTENDED;
      ev.code = (uint16_t)((lp >> 16) & 0xFFL);
      ev.seq = ++p->input_seq;
      if (ev.code != 0U) p->input_cb(&ev, p->input_ud);
      if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) return 0;
      break;
    }
    case WM_MOUSEMOVE: {
      if (!p->mouse_locked) {
        RECT rc;
        if (GetClientRect(hwnd, &rc) && rc.right > 1 && rc.bottom > 1) {
          int mx = (int)(short)(lp & 0xFFFF);
          int my = (int)(short)((lp >> 16) & 0xFFFF);
          if (mx < 0) mx = 0;
          if (my < 0) my = 0;
          if (mx >= rc.right) mx = rc.right - 1;
          if (my >= rc.bottom) my = rc.bottom - 1;
          zc_input_event_t ev;
          memset(&ev, 0, sizeof(ev));
          ev.kind = ZC_INPUT_MOUSE_ABS;
          ev.x = (int32_t)(((int64_t)mx * 65535) / (rc.right - 1));
          ev.y = (int32_t)(((int64_t)my * 65535) / (rc.bottom - 1));
          ev.seq = ++p->input_seq;
          p->input_cb(&ev, p->input_ud);
        }
      }
      break;
    }
    case WM_INPUT: {
      if (p->mouse_locked) {
        RAWINPUT raw;
        UINT sz = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &sz,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            raw.header.dwType == RIM_TYPEMOUSE &&
            !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
            (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0)) {
          zc_input_event_t ev;
          memset(&ev, 0, sizeof(ev));
          ev.kind = ZC_INPUT_MOUSE_REL;
          ev.x = (int32_t)raw.data.mouse.lLastX;
          ev.y = (int32_t)raw.data.mouse.lLastY;
          ev.seq = ++p->input_seq;
          p->input_cb(&ev, p->input_ud);
        }
      }
      break;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
      bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                   msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN);
      if (down && p->mouse_relative && !p->mouse_locked) {
        win_set_mouse_lock(p, true);
      }
      uint16_t btn = ZC_MOUSE_BTN_LEFT;
      if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) btn = ZC_MOUSE_BTN_RIGHT;
      else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) btn = ZC_MOUSE_BTN_MIDDLE;
      else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP) {
        btn = (HIWORD(wp) == XBUTTON1) ? ZC_MOUSE_BTN_X1 : ZC_MOUSE_BTN_X2;
      }
      zc_input_event_t ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind = ZC_INPUT_MOUSE_BTN;
      ev.flags = (uint8_t)(down ? ZC_INPUT_FLAG_DOWN : 0U);
      ev.code = btn;
      ev.seq = ++p->input_seq;
      p->input_cb(&ev, p->input_ud);
      if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP) return TRUE;
      return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      int delta = (int)(short)HIWORD(wp);
      zc_input_event_t ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind = ZC_INPUT_MOUSE_WHEEL;
      if (msg == WM_MOUSEHWHEEL) {
        ev.flags = ZC_INPUT_FLAG_HORIZ;
        ev.x = delta;
      } else {
        ev.y = delta;
      }
      ev.seq = ++p->input_seq;
      p->input_cb(&ev, p->input_ud);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Starts duplicating p->dupl_out1, whatever mode the desktop is in. A frame
 * of the stream's own size and format is copied 1:1 into cap_bgra_tex; any
 * other one (a game switched the display mode, HDR desktop...) is scaled and
 * converted on the way, see capture_desktop_frame. */
static bool try_start_duplication(gpu_pipeline_t *p) {
  if (!p->dupl_out1 || p->dupl) return p->dupl != NULL;
  IDXGIOutputDuplication *d = NULL;
  if (FAILED(IDXGIOutput1_DuplicateOutput(p->dupl_out1, (IUnknown *)p->dev, &d)) || !d) {
    return false;
  }
  p->dupl = d;
  p->using_desktop_dup = true;
  return true;
}

static int create_d3d11_and_swapchain(gpu_pipeline_t *p, bool show_window,
                                      gpu_pipeline_info_t *info) {
  IDXGIFactory2 *fac2 = NULL;
  HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&fac2);
  if (FAILED(hr)) return -1;

  /* Prefer NVIDIA adapter (0x10DE) if present */
  IDXGIAdapter1 *chosen = NULL;
  IDXGIAdapter1 *ad = NULL;
  for (UINT i = 0; IDXGIFactory2_EnumAdapters1(fac2, i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    IDXGIAdapter1_GetDesc1(ad, &desc);
    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      if (!chosen || desc.VendorId == 0x10DE) {
        if (chosen) IDXGIAdapter1_Release(chosen);
        chosen = ad;
        if (desc.VendorId == 0x10DE) break;
      } else {
        IDXGIAdapter1_Release(ad);
      }
    } else {
      IDXGIAdapter1_Release(ad);
    }
  }

  if (chosen) {
    DXGI_ADAPTER_DESC1 desc;
    IDXGIAdapter1_GetDesc1(chosen, &desc);
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, info->adapter_name,
                        sizeof(info->adapter_name), NULL, NULL);
  } else {
    snprintf(info->adapter_name, sizeof(info->adapter_name), "Default Adapter");
  }

  D3D_FEATURE_LEVEL fl_in = D3D_FEATURE_LEVEL_11_1;
  D3D_FEATURE_LEVEL fl_out = D3D_FEATURE_LEVEL_11_0;
  UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  hr = D3D11CreateDevice((IDXGIAdapter *)chosen,
                         chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                         NULL, flags, &fl_in, 1, D3D11_SDK_VERSION, &p->dev,
                         &fl_out, &p->ctx);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice((IDXGIAdapter *)chosen,
                           chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           NULL, flags, NULL, 0, D3D11_SDK_VERSION, &p->dev,
                           &fl_out, &p->ctx);
  }
  if (FAILED(hr)) {
    if (chosen) IDXGIAdapter1_Release(chosen);
    IDXGIFactory2_Release(fac2);
    return -2;
  }

  ID3D10Multithread *mt = NULL;
  if (SUCCEEDED(ID3D11Device_QueryInterface(p->dev, &IID_ID3D10Multithread, (void **)&mt))) {
    ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
    ID3D10Multithread_Release(mt);
  }

  /* Check tearing support via IDXGIFactory5 */
  IDXGIFactory5 *fac5 = NULL;
  BOOL allow_tear = FALSE;
  if (SUCCEEDED(IDXGIFactory2_QueryInterface(fac2, &IID_IDXGIFactory5, (void **)&fac5))) {
    IDXGIFactory5_CheckFeatureSupport(fac5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                      &allow_tear, sizeof(allow_tear));
    IDXGIFactory5_Release(fac5);
  }
  p->tearing_supported = (allow_tear == TRUE);
  info->tearing_supported = p->tearing_supported;

  /* Try Desktop Duplication on chosen adapter's selected output */
  if (chosen) {
    IDXGIOutput *out = NULL;
    if (FAILED(IDXGIAdapter1_EnumOutputs(chosen, g_display_idx, &out)) && g_display_idx != 0U) {
      IDXGIAdapter1_EnumOutputs(chosen, 0, &out);
    }
    if (out) {
      /* The output is kept: duplication is lost on every mode switch or
       * secure-desktop prompt and has to be started again. */
      if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1,
                                               (void **)&p->dupl_out1))) {
        try_start_duplication(p);
      }
      IDXGIOutput_Release(out);
    }
    IDXGIAdapter1_Release(chosen);
  }
  info->using_desktop_dup = p->using_desktop_dup;

  /* Create Win32 window for FLIP_DISCARD SwapChain */
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "ZCRUDPStreamWnd";
  RegisterClassA(&wc);

  p->show_window = show_window;
  DWORD style = show_window ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE) : WS_POPUP;
  p->hwnd = CreateWindowExA(0, wc.lpszClassName,
                            "zcrudp + D3D11 Zero-Copy Video Pipeline", style,
                            100, 100, (int)p->width / 2, (int)p->height / 2,
                            NULL, NULL, wc.hInstance, NULL);
  if (p->hwnd) {
    SetWindowLongPtrA(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);
    if (show_window) {
      RAWINPUTDEVICE rid;
      rid.usUsagePage = 0x01;
      rid.usUsage = 0x02; /* Mouse */
      rid.dwFlags = 0;
      rid.hwndTarget = p->hwnd;
      RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
  }

  DXGI_SWAP_CHAIN_DESC1 scd = {0};
  scd.Width = p->width;
  scd.Height = p->height;
  scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 3;
  scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  scd.Flags = p->tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

  hr = IDXGIFactory2_CreateSwapChainForHwnd(fac2, (IUnknown *)p->dev, p->hwnd,
                                            &scd, NULL, NULL, &p->swapchain);
  IDXGIFactory2_Release(fac2);
  if (FAILED(hr)) return -3;

  {
    IDXGIDevice1 *dxgi_dev1 = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(p->dev, &IID_IDXGIDevice1, (void **)&dxgi_dev1))) {
      IDXGIDevice1_SetMaximumFrameLatency(dxgi_dev1, 1);
      if (FAILED(IDXGIDevice1_SetGPUThreadPriority(dxgi_dev1, 7))) {
        if (FAILED(IDXGIDevice1_SetGPUThreadPriority(dxgi_dev1, 5))) {
          (void)IDXGIDevice1_SetGPUThreadPriority(dxgi_dev1, 1);
        }
      }
      IDXGIDevice1_Release(dxgi_dev1);
    }
  }

  /* Create capture BGRA render target & NV12 encoder input texture in VRAM */
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = p->width;
  td.Height = p->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  hr = ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->cap_bgra_tex);
  if (FAILED(hr)) return -4;
  hr = ID3D11Device_CreateRenderTargetView(p->dev, (ID3D11Resource *)p->cap_bgra_tex,
                                           NULL, &p->cap_rtv);
  if (FAILED(hr)) return -4;

  if (!p->use_zcvid) {
    td.Format = DXGI_FORMAT_NV12;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->enc_nv12_tex);
    if (FAILED(hr)) return -5;
  }

  /* Setup D3D11 VideoProcessor for zero-copy VRAM BGRA <-> NV12 conversion */
  (void)ID3D11DeviceContext_QueryInterface(p->ctx, &IID_ID3D11DeviceContext1, (void **)&p->ctx1);
  hr = ID3D11Device_QueryInterface(p->dev, &IID_ID3D11VideoDevice, (void **)&p->vdev);
  if (FAILED(hr)) return -6;
  hr = ID3D11DeviceContext_QueryInterface(p->ctx, &IID_ID3D11VideoContext, (void **)&p->vctx);
  if (FAILED(hr)) return -6;

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC vp_desc = {0};
  vp_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  vp_desc.InputWidth = p->width;
  vp_desc.InputHeight = p->height;
  vp_desc.OutputWidth = p->width;
  vp_desc.OutputHeight = p->height;
  vp_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(p->vdev, &vp_desc, &p->vp_enum);
  if (FAILED(hr)) return -7;
  hr = ID3D11VideoDevice_CreateVideoProcessor(p->vdev, p->vp_enum, 0, &p->vp);
  if (FAILED(hr)) return -7;

  return 0;
}

static int setup_direct_nvenc(gpu_pipeline_t *p, gpu_pipeline_info_t *info) {
#ifndef ZC_HAVE_NVENC
  (void)p;
  (void)info;
  return -1;
#else
  HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
  if (!dll) return -1;

  typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST *);
  PFN_NvEncodeAPICreateInstance create_inst =
      (PFN_NvEncodeAPICreateInstance)GetProcAddress(dll, "NvEncodeAPICreateInstance");
  if (!create_inst) {
    FreeLibrary(dll);
    return -2;
  }

  memset(&p->nvenc_fn, 0, sizeof(p->nvenc_fn));
  p->nvenc_fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (create_inst(&p->nvenc_fn) != NV_ENC_SUCCESS) {
    FreeLibrary(dll);
    return -3;
  }

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sess_params = {0};
  sess_params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  sess_params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
  sess_params.device = p->dev;
  sess_params.apiVersion = NVENCAPI_VERSION;
  NVENCSTATUS st = p->nvenc_fn.nvEncOpenEncodeSessionEx(&sess_params, &p->nvenc_session);
  if (st != NV_ENC_SUCCESS) {
    FreeLibrary(dll);
    return -4;
  }

  NV_ENC_PRESET_CONFIG preset_cfg = {0};
  preset_cfg.version = NV_ENC_PRESET_CONFIG_VER;
  preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
  st = p->nvenc_fn.nvEncGetEncodePresetConfigEx(
      p->nvenc_session, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
      NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_cfg);
  if (st != NV_ENC_SUCCESS) {
    p->nvenc_fn.nvEncDestroyEncoder(p->nvenc_session);
    FreeLibrary(dll);
    return -5;
  }

  p->nvenc_config = preset_cfg.presetCfg;
  p->nvenc_config.version = NV_ENC_CONFIG_VER;
  p->nvenc_config.frameIntervalP = 1; /* 0 B-frames */
  p->nvenc_config.gopLength = NVENC_INFINITE_GOPLENGTH;
  p->nvenc_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  p->nvenc_config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
  p->nvenc_config.rcParams.averageBitRate = p->bitrate_bps;
  p->nvenc_config.rcParams.maxBitRate = p->bitrate_bps;
  p->nvenc_config.rcParams.vbvBufferSize = p->bitrate_bps / p->fps;
  p->nvenc_config.rcParams.vbvInitialDelay = p->nvenc_config.rcParams.vbvBufferSize;
  p->nvenc_config.rcParams.zeroReorderDelay = 1;
  p->nvenc_config.rcParams.lowDelayKeyFrameScale = 1;
  p->nvenc_config.encodeCodecConfig.h264Config.idrPeriod = p->fps * 2;
  p->nvenc_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
  p->nvenc_config.encodeCodecConfig.h264Config.sliceMode = 0;
  p->nvenc_config.encodeCodecConfig.h264Config.sliceModeData = 0;
  p->nvenc_config.encodeCodecConfig.h264Config.enableIntraRefresh = 0;
  p->nvenc_config.encodeCodecConfig.h264Config.disableDeblockingFilterIDC = 1;
  p->nvenc_config.encodeCodecConfig.h264Config.outputAUD = 1;

  memset(&p->nvenc_init_params, 0, sizeof(p->nvenc_init_params));
  p->nvenc_init_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
  p->nvenc_init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
  p->nvenc_init_params.presetGUID = NV_ENC_PRESET_P1_GUID;
  p->nvenc_init_params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  p->nvenc_init_params.encodeWidth = p->width;
  p->nvenc_init_params.encodeHeight = p->height;
  p->nvenc_init_params.darWidth = p->width;
  p->nvenc_init_params.darHeight = p->height;
  p->nvenc_init_params.frameRateNum = p->fps;
  p->nvenc_init_params.frameRateDen = 1;
  p->nvenc_init_params.enablePTD = 1;
  p->nvenc_init_params.encodeConfig = &p->nvenc_config;

  st = p->nvenc_fn.nvEncInitializeEncoder(p->nvenc_session, &p->nvenc_init_params);
  if (st != NV_ENC_SUCCESS) {
    p->nvenc_fn.nvEncDestroyEncoder(p->nvenc_session);
    FreeLibrary(dll);
    return -6;
  }

  NV_ENC_REGISTER_RESOURCE reg = {0};
  reg.version = NV_ENC_REGISTER_RESOURCE_VER;
  reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
  reg.width = p->width;
  reg.height = p->height;
  reg.pitch = 0;
  reg.resourceToRegister = p->cap_bgra_tex;
  reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
  reg.bufferUsage = NV_ENC_INPUT_IMAGE;
  st = p->nvenc_fn.nvEncRegisterResource(p->nvenc_session, &reg);
  if (st != NV_ENC_SUCCESS) {
    p->nvenc_fn.nvEncDestroyEncoder(p->nvenc_session);
    FreeLibrary(dll);
    return -7;
  }
  p->nvenc_reg_res = reg.registeredResource;

  NV_ENC_CREATE_BITSTREAM_BUFFER bs_buf = {0};
  bs_buf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  st = p->nvenc_fn.nvEncCreateBitstreamBuffer(p->nvenc_session, &bs_buf);
  if (st != NV_ENC_SUCCESS) {
    p->nvenc_fn.nvEncUnregisterResource(p->nvenc_session, p->nvenc_reg_res);
    p->nvenc_fn.nvEncDestroyEncoder(p->nvenc_session);
    FreeLibrary(dll);
    return -8;
  }
  p->nvenc_bitstream = bs_buf.bitstreamBuffer;
  p->nvenc_dll = dll;
  p->use_direct_nvenc = true;
  snprintf(info->encoder_name, sizeof(info->encoder_name),
           "Direct NVENC (P1 ULL, ARGB Zero-Copy, 1-Slice, DeblockOff)");
  return 0;
#endif
}

static int setup_mf_h264_encoder(gpu_pipeline_t *p, gpu_pipeline_info_t *info) {
  MFT_REGISTER_TYPE_INFO in_t = {ZC_MFMediaType_Video, ZC_MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO out_t = {ZC_MFMediaType_Video, ZC_MFVideoFormat_H264};
  IMFActivate **acts = NULL;
  UINT32 count = 0;

  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         &in_t, &out_t, &acts, &count);
  if (FAILED(hr) || count == 0) {
    /* Fallback to any synchronous/async H264 encoder MFT */
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
                   &in_t, &out_t, &acts, &count);
  }
  if (FAILED(hr) || count == 0) return -10;

  bool activated = false;
  for (UINT32 idx = 0; idx < count; ++idx) {
    WCHAR name_w[128] = L"Hardware H.264 MFT";
    UINT32 name_len = 0;
    IMFActivate_GetString(acts[idx], &MFT_FRIENDLY_NAME_Attribute, name_w, 128, &name_len);
    WideCharToMultiByte(CP_UTF8, 0, name_w, -1, info->encoder_name,
                        sizeof(info->encoder_name), NULL, NULL);

    hr = IMFActivate_ActivateObject(acts[idx], &IID_IMFTransform, (void **)&p->enc_mft);
    if (SUCCEEDED(hr) && p->enc_mft) {
      activated = true;
      break;
    }
    fprintf(stderr, "Encoder MFT[%u] '%s' activate failed: 0x%08lx\n",
            idx, info->encoder_name, (unsigned long)hr);
  }
  for (UINT32 i = 0; i < count; ++i) IMFActivate_Release(acts[i]);
  CoTaskMemFree(acts);
  if (!activated) return -11;

  /* Unlock async MFT if needed */
  IMFAttributes *mft_attr = NULL;
  if (SUCCEEDED(IMFTransform_GetAttributes(p->enc_mft, &mft_attr))) {
    UINT32 is_async = 0;
    if (SUCCEEDED(IMFAttributes_GetUINT32(mft_attr, &MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
      IMFAttributes_SetUINT32(mft_attr, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      IMFTransform_QueryInterface(p->enc_mft, &IID_IMFMediaEventGenerator, (void **)&p->enc_ev);
    }
    IMFAttributes_Release(mft_attr);
  }

  /* Bind D3D11 device manager for zero-copy VRAM input */
  IMFTransform_ProcessMessage(p->enc_mft, MFT_MESSAGE_SET_D3D_MANAGER,
                              (ULONG_PTR)p->dxgi_mgr);

  /* Configure ultra-low-latency codec properties */
  ICodecAPI *codec = NULL;
  if (SUCCEEDED(IMFTransform_QueryInterface(p->enc_mft, &ZC_IID_ICodecAPI, (void **)&codec))) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    ICodecAPI_SetValue(codec, &ZC_CODECAPI_AVLowLatencyMode, &v);

    v.vt = VT_UI4;
    v.ulVal = 0;
    ICodecAPI_SetValue(codec, &ZC_CODECAPI_AVEncMPVDefaultBPictureCount, &v);

    v.vt = VT_UI4;
    v.ulVal = p->bitrate_bps;
    ICodecAPI_SetValue(codec, &ZC_CODECAPI_AVEncCommonMeanBitRate, &v);
    ICodecAPI_Release(codec);
  }

  /* Output type first for H.264 encoders */
  IMFMediaType *mt_out = NULL;
  MFCreateMediaType(&mt_out);
  IMFMediaType_SetGUID(mt_out, &ZC_MF_MT_MAJOR_TYPE, &ZC_MFMediaType_Video);
  IMFMediaType_SetGUID(mt_out, &ZC_MF_MT_SUBTYPE, &ZC_MFVideoFormat_H264);
  IMFMediaType_SetUINT32(mt_out, &ZC_MF_MT_AVG_BITRATE, p->bitrate_bps);
  IMFMediaType_SetUINT64(mt_out, &ZC_MF_MT_FRAME_SIZE,
                         ((UINT64)p->width << 32) | p->height);
  IMFMediaType_SetUINT64(mt_out, &ZC_MF_MT_FRAME_RATE,
                         ((UINT64)p->fps << 32) | 1ULL);
  IMFMediaType_SetUINT32(mt_out, &ZC_MF_MT_INTERLACE_MODE, 2); /* Progressive */
  hr = IMFTransform_SetOutputType(p->enc_mft, 0, mt_out, 0);
  IMFMediaType_Release(mt_out);
  if (FAILED(hr)) return -12;

  IMFMediaType *mt_in = NULL;
  MFCreateMediaType(&mt_in);
  IMFMediaType_SetGUID(mt_in, &ZC_MF_MT_MAJOR_TYPE, &ZC_MFMediaType_Video);
  IMFMediaType_SetGUID(mt_in, &ZC_MF_MT_SUBTYPE, &ZC_MFVideoFormat_NV12);
  IMFMediaType_SetUINT64(mt_in, &ZC_MF_MT_FRAME_SIZE,
                         ((UINT64)p->width << 32) | p->height);
  IMFMediaType_SetUINT64(mt_in, &ZC_MF_MT_FRAME_RATE,
                         ((UINT64)p->fps << 32) | 1ULL);
  IMFMediaType_SetUINT32(mt_in, &ZC_MF_MT_INTERLACE_MODE, 2);
  hr = IMFTransform_SetInputType(p->enc_mft, 0, mt_in, 0);
  IMFMediaType_Release(mt_in);
  if (FAILED(hr)) return -13;

  IMFTransform_ProcessMessage(p->enc_mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  IMFTransform_ProcessMessage(p->enc_mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return 0;
}

static int setup_mf_h264_decoder(gpu_pipeline_t *p, gpu_pipeline_info_t *info) {
  HRESULT hr = CoCreateInstance(&ZC_CLSID_MSH264DecoderMFT, NULL,
                                CLSCTX_INPROC_SERVER, &IID_IMFTransform,
                                (void **)&p->dec_mft);
  if (FAILED(hr)) return -20;

  snprintf(info->decoder_name, sizeof(info->decoder_name),
           "MS H.264 D3D11VA Hardware Decoder MFT");

  IMFTransform_ProcessMessage(p->dec_mft, MFT_MESSAGE_SET_D3D_MANAGER,
                              (ULONG_PTR)p->dxgi_mgr);

  IMFAttributes *dec_attr = NULL;
  if (SUCCEEDED(IMFTransform_GetAttributes(p->dec_mft, &dec_attr))) {
    IMFAttributes_SetUINT32(dec_attr, &ZC_CODECAPI_AVLowLatencyMode, TRUE);
    IMFAttributes_Release(dec_attr);
  }

  ICodecAPI *codec = NULL;
  if (SUCCEEDED(IMFTransform_QueryInterface(p->dec_mft, &ZC_IID_ICodecAPI, (void **)&codec))) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    ICodecAPI_SetValue(codec, &ZC_CODECAPI_AVLowLatencyMode, &v);
    ICodecAPI_Release(codec);
  }

  IMFMediaType *mt_in = NULL;
  MFCreateMediaType(&mt_in);
  IMFMediaType_SetGUID(mt_in, &ZC_MF_MT_MAJOR_TYPE, &ZC_MFMediaType_Video);
  IMFMediaType_SetGUID(mt_in, &ZC_MF_MT_SUBTYPE, &ZC_MFVideoFormat_H264);
  IMFMediaType_SetUINT64(mt_in, &ZC_MF_MT_FRAME_SIZE,
                         ((UINT64)p->width << 32) | p->height);
  IMFMediaType_SetUINT64(mt_in, &ZC_MF_MT_FRAME_RATE,
                         ((UINT64)p->fps << 32) | 1ULL);
  IMFMediaType_SetUINT32(mt_in, &ZC_MF_MT_INTERLACE_MODE, 2);
  hr = IMFTransform_SetInputType(p->dec_mft, 0, mt_in, 0);
  IMFMediaType_Release(mt_in);
  if (FAILED(hr)) return -21;

  for (DWORD idx = 0; ; ++idx) {
    IMFMediaType *mt_avail = NULL;
    hr = IMFTransform_GetOutputAvailableType(p->dec_mft, 0, idx, &mt_avail);
    if (FAILED(hr)) break;
    GUID sub = {0};
    IMFMediaType_GetGUID(mt_avail, &ZC_MF_MT_SUBTYPE, &sub);
    if (IsEqualGUID(&sub, &ZC_MFVideoFormat_NV12)) {
      hr = IMFTransform_SetOutputType(p->dec_mft, 0, mt_avail, 0);
      IMFMediaType_Release(mt_avail);
      if (SUCCEEDED(hr)) break;
    } else {
      IMFMediaType_Release(mt_avail);
    }
  }

  IMFTransform_ProcessMessage(p->dec_mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  IMFTransform_ProcessMessage(p->dec_mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return 0;
}

int gpu_pipeline_create(gpu_pipeline_t **out_pipe, uint32_t width,
                        uint32_t height, uint32_t fps, uint32_t bitrate_bps,
                        bool show_window, gpu_pipeline_info_t *out_info) {
  if (!out_pipe) return -1;
  QueryPerformanceFrequency(&g_qpc_freq);
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION, MFSTARTUP_FULL);

  gpu_pipeline_t *p = (gpu_pipeline_t *)calloc(1, sizeof(*p));
  if (!p) return -1;
  p->width = width;
  p->height = height;
  p->fps = fps;
  p->bitrate_bps = bitrate_bps;
  p->enc_out_cap = 2 * 1024 * 1024;
  p->enc_out_buf = (uint8_t *)malloc(p->enc_out_cap);

  gpu_pipeline_info_t info;
  memset(&info, 0, sizeof(info));
  info.width = width;
  info.height = height;

  int rc = create_d3d11_and_swapchain(p, show_window, &info);
  if (rc != 0) {
    gpu_pipeline_destroy(p);
    return rc;
  }

  HRESULT hr = MFCreateDXGIDeviceManager(&p->dxgi_reset_token, &p->dxgi_mgr);
  if (FAILED(hr)) {
    gpu_pipeline_destroy(p);
    return -8;
  }
  IMFDXGIDeviceManager_ResetDevice(p->dxgi_mgr, (IUnknown *)p->dev, p->dxgi_reset_token);

  if (setup_direct_nvenc(p, &info) != 0) {
    rc = setup_mf_h264_encoder(p, &info);
    if (rc != 0) {
      gpu_pipeline_destroy(p);
      return rc;
    }
  }
  rc = setup_mf_h264_decoder(p, &info);
  if (rc != 0) {
    gpu_pipeline_destroy(p);
    return rc;
  }

  if (out_info) *out_info = info;
  *out_pipe = p;
  return 0;
}

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t threshold;
  uint32_t delta_min;
  uint32_t delta_max;
  uint32_t gamma_q8;
  uint32_t neutral_y;
  uint32_t total_blocks;
  uint32_t frame_id;
  uint32_t intra_cycles;
  uint32_t intra_err_thr;
  uint32_t tiles_x;
  uint32_t tiles_y;
  uint32_t search_r;
  uint32_t max_record_words;
  uint32_t mode_flags;
  uint32_t tq_qp;
  uint32_t tq_pad[3];
} zcvid_enc_cb_t;

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t delta_min;
  uint32_t delta_max;
  uint32_t gamma_q8;
  uint32_t neutral_y;
  uint32_t record_words;
  uint32_t tiles_x;
  uint32_t mv_qpel;
  uint32_t dpad[3];
} zcvid_dec_cb_t;

/* ---------------------------------------------------------------------------
 * Compact zcvid bitstream (lossless repacking of what the shaders produce).
 *
 * The encode shader emits 8-byte records in whatever order its threads won
 * the output counter: [position word][payload word]. Most of those 64 bits
 * are redundant: a delta payload is a 16-bit number, and once records are
 * sorted the position is a small gap to the previous one. Packed layout:
 *
 *   u32  0x80000000 | record_words      (top bit marks the packed format;
 *                                        bit 30: records left as raw words)
 *   u32  packed vector bytes
 *   vectors: runs of (count - 1 : u8, dx : i8, dy : i8) over the tile grid;
 *            nothing at all when every vector is zero
 *   8 classes in order: delta (plane * 2 + is_2x) x 6, intra 2-px, intra 4-px
 *     varint  record count
 *     per record, ascending position: varint gap, then the payload in
 *     2 bytes (delta), 4 bytes (intra 2-px) or 5 bytes (intra 4-px)
 *
 * The decoder applies records in any order, so sorting changes nothing.
 * ------------------------------------------------------------------------- */
#define ZCVID_PACKED_FLAG 0x80000000U
#define ZCVID_RAW_RECORDS_FLAG 0x40000000U
#define ZCVID_WIDE_MV_FLAG 0x08000000U /* MVs use int16 (wide +-63.75 px quarter-pel) */
#define ZCVID_EC_MV_FLAG 0x04000000U   /* MVs are median-DPCM + range coded */
#define ZCVID_TQ_V3_FLAG 0x02000000U   /* Class-8 CABAC superblock + intra DC DPCM */
#define ZCVID_RECORD_WORDS_MASK 0x01FFFFFFU
/* Packed layouts carry 9 record classes (class 8: transform intra
 * coefficients) instead of 8. Only set when that class is in use, so a
 * stream without it stays readable by decoders that predate it. */
#define ZCVID_V2_FLAG 0x10000000U
#define ZCVID_CLASSES_MAX 9U

static uint8_t *zcvid_put_varint(uint8_t *o, uint32_t v) {
  while (v >= 0x80U) {
    *o++ = (uint8_t)(v | 0x80U);
    v >>= 7;
  }
  *o++ = (uint8_t)v;
  return o;
}

static const uint8_t *zcvid_get_varint(const uint8_t *i, const uint8_t *end, uint32_t *v) {
  uint32_t r = 0;
  for (unsigned sh = 0; sh < 35U; sh += 7U) {
    if (i >= end) return NULL;
    uint8_t b = *i++;
    r |= (uint32_t)(b & 0x7FU) << sh;
    if (!(b & 0x80U)) {
      *v = r;
      return i;
    }
  }
  return NULL;
}

static uint32_t zcvid_record_class(uint32_t w0, uint32_t *id) {
  uint32_t tag = w0 >> 30;
  uint32_t bit29 = (w0 >> 29) & 1U;
  if (tag == 3U) {
    if (bit29) {
      if ((w0 >> 28) & 1U) { /* transform intra coefficient */
        *id = w0 & 0x0FFFFFFFU;
        return 8U;
      }
      *id = w0 & 0x3FFFFFU;
      return 7U;
    }
    *id = w0 & 0x1FFFFFFFU;
    return 6U;
  }
  *id = w0 & 0x1FFFFFFFU;
  return tag * 2U + bit29;
}

/* Worst case: 5 count bytes per class, 4 gap bytes (28-bit ids) + 5 payload bytes per record (10U bound is conservative). */
static size_t zcvid_pack_bound(uint32_t num_records) {
  return (size_t)ZCVID_CLASSES_MAX * 5U + (size_t)num_records * 10U;
}

/* Sorts records by (class, position). Returns the sorted keys, each
 * (class << 60 | position << 32 | source index), or NULL without memory. */
/* Host buffers are sized at creation (zcvid_prealloc_host); growing one
 * later means an allocation, and page faults, in the frame loop. Kept as a
 * safety net, and said once so that a too small bound gets noticed. */
static void zcvid_note_late_alloc(const char *what, size_t bytes) {
  static volatile LONG said = 0;
  if (InterlockedExchange(&said, 1) == 0) {
    fprintf(stderr, "zcvid: %s grew to %zu bytes after start-up (pre-allocation too small)\n",
            what, bytes);
  }
}

/* A record word pair the encoder left unused (refused transform block). */
#define ZCVID_NO_RECORD 0xFFFFFFFFU

static inline uint32_t zcvid_tq_id_to_sb_key(uint32_t id, uint32_t bw, uint32_t sbw,
                                            uint64_t bw_recip) {
  uint32_t b = (uint32_t)(((uint64_t)id * 0xAAAAAAABULL) >> 37);
  uint32_t rem = id - b * 48U;
  uint32_t by = (uint32_t)(((uint64_t)b * bw_recip) >> 32);
  if (b - by * bw >= bw) by++;
  uint32_t bx = b - by * bw;
  uint32_t sb = (by >> 1) * sbw + (bx >> 1);
  uint32_t quad = ((by & 1U) << 1) | (bx & 1U);
  return ((sb << 2) | quad) * 48U + rem;
}

typedef void (*zcvid_job_fn)(void *ctx, uint32_t index);
static void zcvid_pool_run(struct zcvid_pool_s *pool, zcvid_job_fn fn, void *ctx,
                          uint32_t num_jobs);

#define ZCVID_SORT_CLS8_BKTS 64U
#define ZCVID_SORT_TOTAL_BKTS (8U + ZCVID_SORT_CLS8_BKTS)
#define ZCVID_SORT_MAX_SLICES 32U

typedef struct {
  bool valid;
  uint32_t bkt_beg[ZCVID_SORT_CLS8_BKTS];
  uint32_t bkt_cnt[ZCVID_SORT_CLS8_BKTS];
  uint64_t bkt_w[ZCVID_SORT_CLS8_BKTS];
} zcvid_tq_bkt_info_t;

/* L1-cache-resident 8-bit (256-bin) LSD radix sort on top 32 bits of keys.
 * Output is always placed in a[0..n). */
static void zcvid_radix_sort_256(uint64_t *a, uint64_t *b, uint32_t n, uint32_t diff) {
  if (n <= 1U || diff == 0U) return;
  if (n <= 64U) {
    for (uint32_t i = 1U; i < n; ++i) {
      uint64_t kv = a[i];
      uint32_t j = i;
      while (j > 0U && (a[j - 1U] >> 32) > (kv >> 32)) {
        a[j] = a[j - 1U];
        j--;
      }
      a[j] = kv;
    }
    return;
  }
  uint32_t hist[4][256];
  memset(hist, 0, sizeof(hist));
  uint32_t kor = 0U, kand = 0xFFFFFFFFU;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t k = (uint32_t)(a[i] >> 32);
    kor |= k;
    kand &= k;
    hist[0][k & 0xFFU]++;
    hist[1][(k >> 8) & 0xFFU]++;
    hist[2][(k >> 16) & 0xFFU]++;
    hist[3][k >> 24]++;
  }
  diff &= (kor ^ kand);
  if (diff == 0U) return;
  uint64_t *src = a, *dst = b;
  for (unsigned pass = 0; pass < 4U; ++pass) {
    if (((diff >> (pass * 8U)) & 0xFFU) == 0U) continue;
    uint32_t *h = hist[pass];
    uint32_t sum = 0;
    for (unsigned j = 0; j < 256U; ++j) {
      uint32_t c = h[j];
      h[j] = sum;
      sum += c;
    }
    unsigned shift = 32U + pass * 8U;
    for (uint32_t i = 0; i < n; ++i) dst[h[(src[i] >> shift) & 0xFFU]++] = src[i];
    uint64_t *t = src;
    src = dst;
    dst = t;
  }
  if (src != a) memcpy(a, src, (size_t)n * sizeof(uint64_t));
}

static inline uint32_t zcvid_sort_bucket_of_k32(uint32_t k32, bool sb_tq, uint32_t shift) {
  uint32_t cls = k32 >> 28;
  if (cls < 8U) return cls;
  uint32_t id = k32 & 0x0FFFFFFFU;
  uint32_t v = sb_tq ? (uint32_t)(((uint64_t)id * 0xAAAAAAABULL) >> 39) : id;
  uint32_t b = v >> shift;
  return 8U + ((b < (ZCVID_SORT_CLS8_BKTS - 1U)) ? b : (ZCVID_SORT_CLS8_BKTS - 1U));
}

typedef struct {
  const uint32_t *rec;
  uint64_t *a;
  uint64_t *b;
  uint32_t num_records;
  uint32_t num_slices;
  uint32_t bw;
  uint32_t sbw;
  uint64_t bw_recip;
  uint32_t bkt_shift;
  bool sb_tq;
  uint32_t slice_beg[ZCVID_SORT_MAX_SLICES];
  uint32_t slice_end[ZCVID_SORT_MAX_SLICES];
  uint32_t slice_kept[ZCVID_SORT_MAX_SLICES];
  uint32_t local_cls[ZCVID_SORT_MAX_SLICES][ZCVID_CLASSES_MAX];
  uint32_t local_hist[ZCVID_SORT_MAX_SLICES][ZCVID_SORT_TOTAL_BKTS];
  uint32_t scatter_off[ZCVID_SORT_MAX_SLICES][ZCVID_SORT_TOTAL_BKTS];
  uint32_t bkt_beg[ZCVID_SORT_TOTAL_BKTS];
  uint32_t bkt_cnt[ZCVID_SORT_TOTAL_BKTS];
  uint32_t job_bkt[ZCVID_SORT_TOTAL_BKTS];
  uint64_t bkt_tq_w[ZCVID_SORT_CLS8_BKTS];
} zcvid_psort_ctx_t;

static void zcvid_psort_build_job(void *vctx, uint32_t t) {
  zcvid_psort_ctx_t *c = (zcvid_psort_ctx_t *)vctx;
  const uint32_t beg = c->slice_beg[t];
  const uint32_t end = c->slice_end[t];
  const uint32_t *rec = c->rec;
  uint64_t *dst = c->a + beg;
  uint32_t cls_cnt[ZCVID_CLASSES_MAX];
  uint32_t hist[ZCVID_SORT_TOTAL_BKTS];
  memset(cls_cnt, 0, sizeof(cls_cnt));
  memset(hist, 0, sizeof(hist));
  const uint32_t bw = c->bw, sbw = c->sbw, shift = c->bkt_shift;
  const uint64_t bw_recip = c->bw_recip;
  const bool sb_tq = c->sb_tq;
  uint32_t kept = 0;
  for (uint32_t i = beg; i < end; ++i) {
    uint32_t w0 = rec[i * 2U];
    if (w0 == ZCVID_NO_RECORD) continue;
    uint32_t w1 = rec[i * 2U + 1U];
    uint32_t id = 0;
    uint32_t cls = zcvid_record_class(w0, &id);
    cls_cnt[cls]++;
    uint32_t bkt = cls;
    if (cls == 8U) {
      if (sb_tq) {
        uint32_t blk = (uint32_t)(((uint64_t)id * 0xAAAAAAABULL) >> 37);
        uint32_t rem = id - blk * 48U;
        uint32_t by = (uint32_t)(((uint64_t)blk * bw_recip) >> 32);
        if (blk - by * bw >= bw) by++;
        uint32_t bx = blk - by * bw;
        uint32_t sb = (by >> 1) * sbw + (bx >> 1);
        uint32_t quad = ((by & 1U) << 1) | (bx & 1U);
        id = ((sb << 2) | quad) * 48U + rem;
        uint32_t sb_b = sb >> shift;
        bkt = 8U + ((sb_b < (ZCVID_SORT_CLS8_BKTS - 1U)) ? sb_b : (ZCVID_SORT_CLS8_BKTS - 1U));
      } else {
        uint32_t id_b = id >> shift;
        bkt = 8U + ((id_b < (ZCVID_SORT_CLS8_BKTS - 1U)) ? id_b : (ZCVID_SORT_CLS8_BKTS - 1U));
      }
    }
    hist[bkt]++;
    uint32_t k32 = (cls << 28) | (id & 0x0FFFFFFFU);
    /* Inline w1 into low 32 bits for classes 0..6 and 8 (eliminates random reads
     * from D3D11 staging memory after sort); class 7 needs w0's cr bits so keeps i. */
    uint32_t lo = (cls == 7U) ? i : w1;
    dst[kept++] = ((uint64_t)k32 << 32) | (uint64_t)lo;
  }
  memcpy(c->local_cls[t], cls_cnt, sizeof(cls_cnt));
  memcpy(c->local_hist[t], hist, sizeof(hist));
  c->slice_kept[t] = kept;
}

static void zcvid_psort_scatter_job(void *vctx, uint32_t t) {
  zcvid_psort_ctx_t *c = (zcvid_psort_ctx_t *)vctx;
  const uint64_t *src = c->a + c->slice_beg[t];
  const uint32_t n = c->slice_kept[t];
  uint64_t *dst = c->b;
  uint32_t off[ZCVID_SORT_TOTAL_BKTS];
  memcpy(off, c->scatter_off[t], sizeof(off));
  const bool sb_tq = c->sb_tq;
  const uint32_t shift = c->bkt_shift;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t kv = src[i];
    uint32_t bkt = zcvid_sort_bucket_of_k32((uint32_t)(kv >> 32), sb_tq, shift);
    dst[off[bkt]++] = kv;
  }
}

static inline uint32_t zcvid_tq_rec_weight(uint32_t id, uint32_t *sb_end, uint32_t *blk_end,
                                          uint32_t *pl_end, uint32_t *prev_k) {
  uint32_t w = 3U; /* coef: sign + gt1 + gt2 */
  uint32_t k = id & 15U;
  if (id >= *sb_end) {
    uint32_t sb = (uint32_t)(((uint64_t)id * 0xAAAAAAABULL) >> 39);
    uint32_t blk = (uint32_t)(((uint64_t)id * 0xAAAAAAABULL) >> 37);
    *sb_end = (sb + 1U) * 192U;
    *blk_end = (blk + 1U) * 48U;
    *pl_end = (id & ~15U) + 16U;
    *prev_k = k;
    w += 18U + (k ? (4U + k) : 0U); /* new SB (8) + blk (6) + plane (4) + last_k/sig */
  } else if (id >= *blk_end) {
    uint32_t blk = (uint32_t)(((uint64_t)id * 0xAAAAAAABULL) >> 37);
    *blk_end = (blk + 1U) * 48U;
    *pl_end = (id & ~15U) + 16U;
    *prev_k = k;
    w += 10U + (k ? (4U + k) : 0U); /* new blk (6) + plane (4) + last_k/sig */
  } else if (id >= *pl_end) {
    *pl_end = (id & ~15U) + 16U;
    *prev_k = k;
    w += 4U + (k ? (4U + k) : 0U); /* new plane (4) + last_k/sig */
  } else {
    w += (*prev_k == 0U) ? (4U + k) : (k - *prev_k);
    *prev_k = k;
  }
  return w;
}

static void zcvid_psort_bucket_job(void *vctx, uint32_t idx) {
  zcvid_psort_ctx_t *c = (zcvid_psort_ctx_t *)vctx;
  uint32_t bk = c->job_bkt[idx];
  uint32_t beg = c->bkt_beg[bk];
  uint32_t cnt = c->bkt_cnt[bk];
  uint64_t *keys = c->b + beg;
  zcvid_radix_sort_256(keys, c->a + beg, cnt, 0xFFFFFFFFU);
  if (c->sb_tq && bk >= 8U) {
    uint32_t sb_end = 0U, blk_end = 0U, pl_end = 0U, prev_k = 0U;
    uint64_t w = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
      uint32_t id = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
      w += zcvid_tq_rec_weight(id, &sb_end, &blk_end, &pl_end, &prev_k);
    }
    c->bkt_tq_w[bk - 8U] = w;
  }
}

static const uint64_t *zcvid_sort_records_ex(gpu_pipeline_t *p, const uint32_t *rec,
                                            uint32_t *num_io, uint32_t counts[ZCVID_CLASSES_MAX],
                                            bool sb_tq, zcvid_tq_bkt_info_t *bkt_info) {
  if (bkt_info) bkt_info->valid = false;
  uint32_t num_records = *num_io;
  if (num_records > p->zcvid_sort_cap) {
    uint32_t cap = num_records + num_records / 2U + 1024U;
    if (p->zcvid_sort_cap != 0U) zcvid_note_late_alloc("record sort", (size_t)cap * 16U);
    uint64_t *a = (uint64_t *)realloc(p->zcvid_sort_a, (size_t)cap * sizeof(uint64_t));
    if (a) p->zcvid_sort_a = a;
    uint64_t *b = (uint64_t *)realloc(p->zcvid_sort_b, (size_t)cap * sizeof(uint64_t));
    if (b) p->zcvid_sort_b = b;
    if (!a || !b) return NULL;
    p->zcvid_sort_cap = cap;
  }
  const uint32_t bw = (p->width + 3U) >> 2;
  const uint32_t bh = (p->height + 3U) >> 2;
  const uint32_t sbw = (bw + 1U) >> 1;
  const uint32_t sbh = (bh + 1U) >> 1;
  const uint64_t bw_recip = (bw > 0U) ? ((1ULL << 32) / (uint64_t)bw) : 0ULL;
  uint64_t *a = p->zcvid_sort_a, *b = p->zcvid_sort_b;
  memset(counts, 0, ZCVID_CLASSES_MAX * sizeof(uint32_t));

  /* Parallel L1-cache-resident 72-bucket MSD+LSD radix sort for large frames. */
  if (num_records >= 4096U && p->zcvid_pool && p->zcvid_ec_threads > 1U) {
    uint32_t max_v = sb_tq ? (sbw * sbh) : (bw * bh * 48U);
    uint32_t shift = 0U;
    while (max_v > ZCVID_SORT_CLS8_BKTS && ((max_v - 1U) >> shift) >= ZCVID_SORT_CLS8_BKTS) {
      shift++;
    }
    uint32_t slices = p->zcvid_ec_threads;
    if (slices > ZCVID_SORT_MAX_SLICES) slices = ZCVID_SORT_MAX_SLICES;
    if (num_records / slices < 512U) slices = num_records / 512U;
    if (slices < 2U) slices = 2U;

    zcvid_psort_ctx_t sc;
    sc.rec = rec;
    sc.a = a;
    sc.b = b;
    sc.num_records = num_records;
    sc.num_slices = slices;
    sc.bw = bw;
    sc.sbw = sbw;
    sc.bw_recip = bw_recip;
    sc.bkt_shift = shift;
    sc.sb_tq = sb_tq;
    for (uint32_t t = 0; t < slices; ++t) {
      sc.slice_beg[t] = (uint32_t)(((uint64_t)num_records * t) / slices);
      sc.slice_end[t] = (uint32_t)(((uint64_t)num_records * (t + 1U)) / slices);
    }
    zcvid_pool_run(p->zcvid_pool, zcvid_psort_build_job, &sc, slices);

    uint32_t kept = 0;
    for (uint32_t t = 0; t < slices; ++t) {
      kept += sc.slice_kept[t];
      for (uint32_t cls = 0; cls < ZCVID_CLASSES_MAX; ++cls) counts[cls] += sc.local_cls[t][cls];
    }
    *num_io = kept;
    p->zcvid_sort_kept = kept;
    if (kept <= 1U) {
      if (kept == 1U) {
        for (uint32_t t = 0; t < slices; ++t) {
          if (sc.slice_kept[t] != 0U) { b[0] = a[sc.slice_beg[t]]; break; }
        }
      }
      return b;
    }
    uint32_t run = 0;
    uint32_t njobs = 0;
    for (uint32_t bk = 0; bk < ZCVID_SORT_TOTAL_BKTS; ++bk) {
      sc.bkt_beg[bk] = run;
      uint32_t btot = 0;
      for (uint32_t t = 0; t < slices; ++t) {
        sc.scatter_off[t][bk] = run + btot;
        btot += sc.local_hist[t][bk];
      }
      sc.bkt_cnt[bk] = btot;
      run += btot;
      if (bk >= 8U) sc.bkt_tq_w[bk - 8U] = 0;
      if (btot > 0U) sc.job_bkt[njobs++] = bk;
    }
    zcvid_pool_run(p->zcvid_pool, zcvid_psort_scatter_job, &sc, slices);

    /* Schedule largest buckets first so no big bucket stalls the tail. */
    for (uint32_t i = 1U; i < njobs; ++i) {
      uint32_t bk = sc.job_bkt[i];
      uint32_t cnt = sc.bkt_cnt[bk];
      uint32_t j = i;
      while (j > 0U && sc.bkt_cnt[sc.job_bkt[j - 1U]] < cnt) {
        sc.job_bkt[j] = sc.job_bkt[j - 1U];
        j--;
      }
      sc.job_bkt[j] = bk;
    }
    zcvid_pool_run(p->zcvid_pool, zcvid_psort_bucket_job, &sc, njobs);
    if (bkt_info && sb_tq) {
      bkt_info->valid = true;
      for (uint32_t k = 0; k < ZCVID_SORT_CLS8_BKTS; ++k) {
        bkt_info->bkt_beg[k] = sc.bkt_beg[8U + k];
        bkt_info->bkt_cnt[k] = sc.bkt_cnt[8U + k];
        bkt_info->bkt_w[k] = sc.bkt_tq_w[k];
      }
    }
    return b;
  }

  /* Small frame (< 4096 records): L1-resident single-threaded 256-bin radix sort. */
  uint32_t kept = 0;
  uint32_t key_or = 0U, key_and = 0xFFFFFFFFU;
  for (uint32_t i = 0; i < num_records; ++i) {
    uint32_t w0 = rec[i * 2U];
    if (w0 == ZCVID_NO_RECORD) continue;
    uint32_t w1 = rec[i * 2U + 1U];
    uint32_t id = 0;
    uint32_t cls = zcvid_record_class(w0, &id);
    counts[cls]++;
    if (sb_tq && cls == 8U) {
      id = zcvid_tq_id_to_sb_key(id, bw, sbw, bw_recip);
    }
    uint32_t k32 = (cls << 28) | (id & 0x0FFFFFFFU);
    key_or |= k32;
    key_and &= k32;
    uint32_t lo = (cls == 7U) ? i : w1;
    a[kept++] = ((uint64_t)k32 << 32) | (uint64_t)lo;
  }
  *num_io = kept;
  p->zcvid_sort_kept = kept;
  if (kept <= 1U) return a;
  zcvid_radix_sort_256(a, b, kept, key_or ^ key_and);
  return a;
}

static const uint64_t *zcvid_sort_records(gpu_pipeline_t *p, const uint32_t *rec,
                                         uint32_t *num_io, uint32_t counts[ZCVID_CLASSES_MAX],
                                         bool sb_tq) {
  return zcvid_sort_records_ex(p, rec, num_io, counts, sb_tq, NULL);
}

/* Packs the records of sorted keys a[0 .. num). */
static size_t zcvid_pack_sorted(const uint32_t *rec, const uint64_t *a, uint32_t num,
                               uint32_t ncls, uint8_t *out) {
  uint32_t counts[ZCVID_CLASSES_MAX] = {0};
  for (uint32_t n = 0; n < num; ++n) counts[a[n] >> 60]++;

  uint8_t *o = out;
  uint32_t i = 0;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    o = zcvid_put_varint(o, counts[cls]);
    uint32_t prev = 0;
    for (uint32_t n = 0; n < counts[cls]; ++n, ++i) {
      uint32_t lo = (uint32_t)a[i];
      uint32_t id = (uint32_t)(a[i] >> 32) & 0x0FFFFFFFU;
      uint32_t w0 = (cls == 7U) ? rec[lo * 2U] : 0U;
      uint32_t w1 = (cls == 7U) ? rec[lo * 2U + 1U] : lo;
      o = zcvid_put_varint(o, id - prev);
      prev = id;
      if (cls < 6U) {
        *o++ = (uint8_t)w1;
        *o++ = (uint8_t)(w1 >> 8);
      } else if (cls == 6U) {
        memcpy(o, &w1, 4);
        o += 4;
      } else if (cls == 7U) {
        uint64_t v = ((uint64_t)(w1 & 0x3FFFFFFFU) << 6) | ((w0 >> 22) & 0x3FU);
        for (unsigned k = 0; k < 5U; ++k) *o++ = (uint8_t)(v >> (8U * k));
      } else { /* level (16 bits), qp (6 bits) and pred (2 bits: 0..3, 4x4/8x8 intra/inter) */
        *o++ = (uint8_t)w1;
        *o++ = (uint8_t)(w1 >> 8);
        *o++ = (uint8_t)(((w1 >> 16) & 0x3FU) | (((w1 >> 24) & 3U) << 6));
      }
    }
  }
  return (size_t)(o - out);
}

static size_t zcvid_pack_records(gpu_pipeline_t *p, const uint32_t *rec,
                                uint32_t num_records, uint8_t *out) {
  uint32_t counts[ZCVID_CLASSES_MAX];
  const uint64_t *a = zcvid_sort_records(p, rec, &num_records, counts, false);
  if (!a) return 0;
  return zcvid_pack_sorted(rec, a, num_records, p->zcvid_ncls, out);
}

/* Expands a packed record stream back to 2 words per record. Returns the word
 * count, or UINT32_MAX on a malformed stream. */
static uint32_t zcvid_unpack_records(const uint8_t *in, size_t in_len, uint32_t *out,
                                    uint32_t max_words, uint32_t ncls) {
  const uint8_t *i = in, *end = in + in_len;
  uint32_t words = 0;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    uint32_t count = 0;
    i = zcvid_get_varint(i, end, &count);
    if (!i) return UINT32_MAX;
    uint32_t id = 0;
    for (uint32_t n = 0; n < count; ++n) {
      uint32_t gap = 0;
      i = zcvid_get_varint(i, end, &gap);
      if (!i || words + 2U > max_words) return UINT32_MAX;
      id += gap;
      uint32_t w0, w1;
      if (cls < 6U) {
        if (end - i < 2) return UINT32_MAX;
        w0 = ((cls >> 1) << 30) | ((cls & 1U) << 29) | (id & 0x1FFFFFFFU);
        w1 = (uint32_t)i[0] | ((uint32_t)i[1] << 8);
        i += 2;
      } else if (cls == 6U) {
        if (end - i < 4) return UINT32_MAX;
        w0 = 0xC0000000U | (id & 0x1FFFFFFFU);
        memcpy(&w1, i, 4);
        i += 4;
      } else if (cls == 7U) {
        if (end - i < 5) return UINT32_MAX;
        uint64_t v = 0;
        for (unsigned k = 0; k < 5U; ++k) v |= (uint64_t)i[k] << (8U * k);
        i += 5;
        w0 = 0xE0000000U | ((uint32_t)(v & 0x3FU) << 22) | (id & 0x3FFFFFU);
        w1 = (uint32_t)(v >> 6) & 0x3FFFFFFFU;
      } else {
        if (end - i < 3) return UINT32_MAX;
        w0 = 0xF0000000U | (id & 0x0FFFFFFFU);
        w1 = (uint32_t)i[0] | ((uint32_t)i[1] << 8) | ((uint32_t)(i[2] & 0x3FU) << 16) |
             ((uint32_t)(i[2] >> 6) << 24);
        i += 3;
      }
      out[words++] = w0;
      out[words++] = w1;
    }
  }
  return words;
}

/* ---------------------------------------------------------------------------
 * Range-coded records (ZCVID_EC_FLAG). Same sorted classes as the packed
 * layout, but every field goes through the adaptive binary range coder of
 * zcvid_entropy.h, one stream for the whole frame:
 *
 *   per class: record count (24 raw bits), then per record in ascending
 *   position the distance to the end of the previous record (adaptive
 *   Exp-Golomb, 0 = adjacent) and its payload:
 *
 *   delta   10 trits: zero flag and sign, each under a context made of the
 *           trits to the left and on the row above (blocks that were not
 *           sent count as zeros). A record costs what its trits are worth
 *           instead of a flat 16 bits.
 *   intra   every sample is predicted from its left / upper / upper-left
 *           neighbours (median edge detector) when the frame carries them,
 *           and only the prediction error is coded. Rolling-refresh rows are
 *           contiguous, so almost every sample has its neighbours.
 *
 * Lossless: the unpacked records are the packed ones, bit for bit. Nothing
 * depends on earlier frames, so a lost frame cannot desynchronise the coder.
 * The decoder accepts arbitrary bytes (bounded output, no out-of-range read).
 * ------------------------------------------------------------------------- */
#define ZCVID_EC_FLAG 0x20000000U
#define ZCVID_EC_COUNT_BITS 24U

/* Coding time grows with the record count, so each side measures what a
 * record word costs it and the coder takes ZCVID_EC_TIME_PCT percent of the
 * frame period at most. Records beyond that budget are not given up on: the
 * frame carries them in the plain packed layout after the coded ones. */
#ifndef ZCVID_EC_TIME_PCT
#define ZCVID_EC_TIME_PCT 30U
#endif

typedef struct {
  zcrudp_iec_model_t luma;
  zcrudp_iec_model_t chroma;
  uint8_t *line;       /* [y, cb, cr] per column: the sample last coded there */
  uint32_t *lrow;      /* row_base + row of the sample last coded in this column */
  uint32_t row_base;   /* monotonic tag base for this reset (avoids per-frame memset) */
  uint32_t width;
  uint64_t width_recip;
  uint32_t last_idx;   /* pixel index of the last sample stored */
  uint8_t last_up[3];  /* what its column held before, when that was the row above */
  bool last_up_ok;
  uint32_t act_y;      /* magnitude of the last luma / chroma residual */
  uint32_t act_c;
} zcvid_ec_intra_t;

static void zcvid_ec_intra_reset(zcvid_ec_intra_t *s, uint8_t *line, uint32_t *lrow,
                                uint32_t *gen_io, uint32_t width, uint32_t height) {
  zcrudp_iec_model_reset(&s->luma);
  zcrudp_iec_model_reset(&s->chroma);
  s->line = line;
  s->lrow = lrow;
  uint32_t step = height + 2U;
  if (*gen_io == 0U || *gen_io > UINT32_MAX - step - 2U) {
    memset(lrow, 0, (size_t)width * sizeof(uint32_t));
    *gen_io = 1U;
  } else {
    *gen_io += step;
  }
  s->row_base = *gen_io;
  s->width = width;
  s->width_recip = (width > 0U) ? ((1ULL << 32) / (uint64_t)width) : 0ULL;
  s->last_idx = UINT32_MAX;
  s->last_up_ok = false;
  s->act_y = 0;
  s->act_c = 0;
}

static inline void zcvid_ec_idx_to_xy(const zcvid_ec_intra_t *s, uint32_t idx,
                                     uint32_t *out_row, uint32_t *out_x) {
  uint32_t w = s->width;
  uint32_t row = (uint32_t)(((uint64_t)idx * s->width_recip) >> 32);
  uint32_t rem = idx - row * w;
  if (rem >= w) {
    row++;
    rem -= w;
  }
  *out_row = row;
  *out_x = rem;
}

/* Neighbours of pixel (row, x) among the samples of this frame coded so far. */
typedef struct {
  bool has_a, has_b, has_c;
  uint8_t a[3], b[3], c[3]; /* left, above, above-left */
} zcvid_ec_nb_t;

static inline void zcvid_ec_neighbours_xy(const zcvid_ec_intra_t *s, uint32_t idx,
                                         uint32_t row, uint32_t x, zcvid_ec_nb_t *nb) {
  uint32_t cur_tag = s->row_base + row;
  nb->has_a = (x > 0U) && s->lrow[x - 1U] == cur_tag;
  nb->has_b = (row > 0U) && s->lrow[x] == cur_tag - 1U;
  /* The column to the left now holds this row: its former content was kept
   * aside when the previous sample was stored. */
  nb->has_c = nb->has_a && s->last_up_ok && s->last_idx + 1U == idx;
  if (nb->has_a) {
    const uint8_t *pa = s->line + (size_t)(x - 1U) * 3U;
    nb->a[0] = pa[0]; nb->a[1] = pa[1]; nb->a[2] = pa[2];
  }
  if (nb->has_b) {
    const uint8_t *pb = s->line + (size_t)x * 3U;
    nb->b[0] = pb[0]; nb->b[1] = pb[1]; nb->b[2] = pb[2];
  }
  if (nb->has_c) {
    nb->c[0] = s->last_up[0]; nb->c[1] = s->last_up[1]; nb->c[2] = s->last_up[2];
  }
}

static inline uint32_t zcvid_ec_predict(const zcvid_ec_nb_t *nb, unsigned ch, uint32_t *grad) {
  if (nb->has_a && nb->has_b && nb->has_c) {
    int a = nb->a[ch], b = nb->b[ch], c = nb->c[ch];
    *grad = (uint32_t)(abs(a - c) + abs(b - c));
    return zcrudp_iec_med((uint8_t)a, (uint8_t)b, (uint8_t)c);
  }
  *grad = UINT32_MAX; /* unknown: the caller falls back on the last residual */
  if (nb->has_a) return nb->a[ch];
  if (nb->has_b) return nb->b[ch];
  return 128U;
}

static inline void zcvid_ec_store_xy(zcvid_ec_intra_t *s, uint32_t idx, uint32_t row, uint32_t x,
                                    uint8_t y, uint8_t cb, uint8_t cr) {
  uint8_t *l = s->line + (size_t)x * 3U;
  uint32_t cur_tag = s->row_base + row;
  s->last_up_ok = (row > 0U) && s->lrow[x] == cur_tag - 1U;
  s->last_up[0] = l[0]; s->last_up[1] = l[1]; s->last_up[2] = l[2];
  s->last_idx = idx;
  l[0] = y;
  l[1] = cb;
  l[2] = cr;
  s->lrow[x] = cur_tag;
}

/* Samples travel as 8-bit values (shift 0) or 6-bit ones (shift 2); the
 * predictors always work on 8 bits. */
static inline void zcvid_ec_put_sample(zcrudp_rc_enc_t *rc, zcrudp_iec_model_t *m, uint32_t *act,
                                      uint32_t pred8, uint32_t grad, unsigned shift, uint32_t v) {
  uint32_t a = (grad != UINT32_MAX) ? (grad >> shift) : (*act * 2U);
  int32_t res = (int32_t)v - (int32_t)(pred8 >> shift);
  zcrudp_iec_enc_residual(rc, m, zcrudp_iec_bucket(a), 0U, res);
  *act = (uint32_t)abs(res);
}

static inline uint32_t zcvid_ec_get_sample(zcrudp_rc_dec_t *rc, zcrudp_iec_model_t *m, uint32_t *act,
                                          uint32_t pred8, uint32_t grad, unsigned shift) {
  uint32_t a = (grad != UINT32_MAX) ? (grad >> shift) : (*act * 2U);
  int32_t res = zcrudp_iec_dec_residual(rc, m, zcrudp_iec_bucket(a), 0U);
  *act = (uint32_t)abs(res);
  return (uint32_t)((int32_t)(pred8 >> shift) + res) & (0xFFU >> shift);
}

/* A 6-bit sample as the 8-bit value the shaders rebuild from it. */
static inline uint8_t zcvid_ec_expand6(uint32_t q6) { return (uint8_t)((q6 << 2) | (q6 >> 4)); }

/* Trits are indexed block * 10 + j; returns the distance to the trit above
 * (0 when rows do not line up). In 2x mode a trit covers two pixels. */
static size_t zcvid_ec_delta_stride(uint32_t width, uint32_t cls) {
  if ((cls & 1U) == 0U) return width;
  return (width & 1U) ? 0U : width / 2U;
}

/* Positions a record of this class may take; anything else is corrupt. */
static uint32_t zcvid_ec_id_limit(uint32_t num_px, uint32_t cls) {
  if (cls < 6U) return num_px / 10U + 1U;
  return (cls == 6U) ? num_px : (num_px + 1U) / 2U;
}

static inline uint32_t zcvid_ec_trit_ctx(const zcrudp_tec_model_t *m, size_t idx, uint32_t *sctx) {
  const uint8_t *hist = m->hist;
  const size_t stride = m->stride;
  uint32_t l1 = hist[(idx - 1U) & ZCRUDP_TEC_HIST_MASK];
  uint32_t l2 = hist[(idx - 2U) & ZCRUDP_TEC_HIST_MASK];
  uint32_t u = 0, ul = 0, ur = 0;
  if (stride != 0U) {
    u = hist[(idx - stride) & ZCRUDP_TEC_HIST_MASK];
    ul = hist[(idx - stride - 1U) & ZCRUDP_TEC_HIST_MASK];
    ur = hist[(idx - stride + 1U) & ZCRUDP_TEC_HIST_MASK];
  }
  *sctx = l1 * 3U + (u ? u : (ul ? ul : ur));
  return (uint32_t)(l1 != 0U) | ((uint32_t)(l2 != 0U) << 1) | ((uint32_t)(u != 0U) << 2) |
         (((uint32_t)(ul != 0U) + (uint32_t)(ur != 0U)) << 3);
}

static inline void zcvid_ec_put_trits(zcrudp_rc_enc_t *rc, zcrudp_tec_model_t *m, size_t base,
                                     uint32_t w1) {
  zcrudp_trit_t trits[10];
  zcrudp_unpack_10trits((uint16_t)w1, trits);
  for (size_t j = 0; j < 10U; ++j) {
    size_t idx = base + j;
    int t = trits[j];
    uint32_t sctx = 0;
    uint32_t zctx = zcvid_ec_trit_ctx(m, idx, &sctx);
    uint8_t h = 0;
    zcrudp_rc_enc_bit(rc, &m->zero[zctx], (t != 0) ? 1U : 0U);
    if (t != 0) {
      zcrudp_rc_enc_bit(rc, &m->sign[sctx], (t < 0) ? 1U : 0U);
      h = (t < 0) ? 2U : 1U;
    }
    m->hist[idx & ZCRUDP_TEC_HIST_MASK] = h;
  }
}

static inline uint32_t zcvid_ec_get_trits(zcrudp_rc_dec_t *rc, zcrudp_tec_model_t *m, size_t base) {
  static const uint32_t pow3[10] = {1, 3, 9, 27, 81, 243, 729, 2187, 6561, 19683};
  uint32_t w1 = 0;
  for (size_t j = 0; j < 10U; ++j) {
    size_t idx = base + j;
    uint32_t sctx = 0;
    uint32_t zctx = zcvid_ec_trit_ctx(m, idx, &sctx);
    uint32_t code = 1U;
    uint8_t h = 0;
    if (zcrudp_rc_dec_bit(rc, &m->zero[zctx]) != 0U) {
      bool neg = zcrudp_rc_dec_bit(rc, &m->sign[sctx]) != 0U;
      code = neg ? 0U : 2U;
      h = neg ? 2U : 1U;
    }
    m->hist[idx & ZCRUDP_TEC_HIST_MASK] = h;
    w1 += code * pow3[j];
  }
  return w1;
}

/* ---------------------------------------------------------------------------
 * Worker pool: the range coder is serial inside a stream, so a frame is cut
 * into chunks coded side by side. The calling thread takes its share.
 * Cache-line isolated per-worker slots + acquire loads + single-semaphore
 * batched wakeup eliminate multi-core bus contention and SetEvent storms.
 * ------------------------------------------------------------------------- */
static inline LONG zcvid_atomic_load_acq(const volatile LONG *ptr) {
#if defined(__GNUC__) || defined(__clang__)
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#else
  LONG v = *ptr;
  _ReadBarrier();
  return v;
#endif
}

static inline uint32_t zcvid_ctz32(uint32_t mask) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctz(mask);
#elif defined(_MSC_VER)
  unsigned long idx = 0;
  _BitScanForward(&idx, mask);
  return (uint32_t)idx;
#else
  uint32_t n = 0;
  while ((mask & 1U) == 0U) { mask >>= 1; n++; }
  return n;
#endif
}

static inline uint32_t zcvid_popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_popcount(v);
#else
  v = v - ((v >> 1) & 0x55555555U);
  v = (v & 0x33333333U) + ((v >> 2) & 0x33333333U);
  return (((v + (v >> 4)) & 0x0F0F0F0FU) * 0x01010101U) >> 24;
#endif
}

#define ZCVID_POOL_RING 64U

typedef struct {
  _Alignas(64) volatile LONG next;
  _Alignas(64) volatile LONG done;
  _Alignas(64) volatile LONG active;
  _Alignas(64) volatile LONG batch_gen;
  LONG num_jobs;
  zcvid_job_fn fn;
  void *ctx;
} zcvid_pool_batch_t;

struct zcvid_pool_s;
typedef struct {
  HANDLE thread;
  struct zcvid_pool_s *pool;
  uint32_t worker_idx;
} zcvid_pool_slot_t;

struct zcvid_pool_s {
  _Alignas(64) volatile LONG gen;
  _Alignas(64) volatile LONG quit;
  _Alignas(64) volatile LONG sleeping;
  HANDLE sem;
  uint32_t num_workers;
  zcvid_pool_batch_t batches[ZCVID_POOL_RING];
  zcvid_pool_slot_t slots[ZCVID_EC_MAX_CHUNKS];
};

static void zcvid_pool_drain_batch(zcvid_pool_batch_t *b) {
  LONG nj = b->num_jobs;
  for (;;) {
    LONG i = InterlockedIncrement(&b->next) - 1;
    if (i >= nj) break;
    b->fn(b->ctx, (uint32_t)i);
    InterlockedIncrement(&b->done);
  }
}

static DWORD WINAPI zcvid_pool_worker(LPVOID arg) {
  zcvid_pool_slot_t *sl = (zcvid_pool_slot_t *)arg;
  struct zcvid_pool_s *pool = sl->pool;
  LONG seen_gen = 0;
  for (;;) {
    LONG g = zcvid_atomic_load_acq(&pool->gen);
    if (g == seen_gen) {
      uint64_t t0 = qpc_now_us();
      uint32_t spin = 0U;
      while ((g = zcvid_atomic_load_acq(&pool->gen)) == seen_gen) {
        if (zcvid_atomic_load_acq(&pool->quit) != 0) return 0;
        if ((++spin & 63U) == 0U && qpc_now_us() - t0 >= 300ULL) break;
        YieldProcessor();
      }
      if (g == seen_gen) {
        InterlockedIncrement(&pool->sleeping);
        if ((g = zcvid_atomic_load_acq(&pool->gen)) == seen_gen &&
            zcvid_atomic_load_acq(&pool->quit) == 0) {
          WaitForSingleObject(pool->sem, INFINITE);
        }
        InterlockedDecrement(&pool->sleeping);
        continue;
      }
    }
    if (zcvid_atomic_load_acq(&pool->quit) != 0) break;
    seen_gen = g;
    zcvid_pool_batch_t *b = &pool->batches[(uint32_t)g & (ZCVID_POOL_RING - 1U)];
    InterlockedIncrement(&b->active);
    if (zcvid_atomic_load_acq(&b->batch_gen) == g) {
      zcvid_pool_drain_batch(b);
    }
    InterlockedDecrement(&b->active);
  }
  return 0;
}

static struct zcvid_pool_s *zcvid_pool_create(uint32_t num_workers) {
  struct zcvid_pool_s *pool = (struct zcvid_pool_s *)_aligned_malloc(sizeof(*pool), 64U);
  if (!pool) return NULL;
  memset(pool, 0, sizeof(*pool));
  if (num_workers > ZCVID_EC_MAX_CHUNKS) num_workers = ZCVID_EC_MAX_CHUNKS;
  pool->sem = CreateSemaphoreA(NULL, 0, (LONG)(ZCVID_EC_MAX_CHUNKS * 8U), NULL);
  if (!pool->sem) {
    _aligned_free(pool);
    return NULL;
  }
  for (uint32_t i = 0; i < num_workers; ++i) {
    zcvid_pool_slot_t *sl = &pool->slots[i];
    sl->pool = pool;
    sl->worker_idx = i;
    HANDLE h = CreateThread(NULL, 0, zcvid_pool_worker, sl, 0, NULL);
    if (!h) break;
    sl->thread = h;
    pool->num_workers++;
  }
  return pool; /* with 0 workers every job runs on the caller */
}

static void zcvid_pool_destroy(struct zcvid_pool_s *pool) {
  if (!pool) return;
  InterlockedExchange(&pool->quit, 1);
  if (pool->num_workers > 0U) {
    HANDLE threads[ZCVID_EC_MAX_CHUNKS];
    for (uint32_t i = 0; i < pool->num_workers; ++i) {
      threads[i] = pool->slots[i].thread;
    }
    ReleaseSemaphore(pool->sem, (LONG)pool->num_workers, NULL);
    WaitForMultipleObjects(pool->num_workers, threads, TRUE, INFINITE);
  }
  for (uint32_t i = 0; i < pool->num_workers; ++i) {
    CloseHandle(pool->slots[i].thread);
  }
  if (pool->sem) CloseHandle(pool->sem);
  _aligned_free(pool);
}

/* Lock-free generation-counter batch dispatch: zero skipped workers on
 * back-to-back batches (build -> scatter -> bucket -> ec_put -> ec_get),
 * and returns as soon as all jobs complete without waiting for slow wakeups. */
static void zcvid_pool_run(struct zcvid_pool_s *pool, zcvid_job_fn fn, void *ctx,
                          uint32_t num_jobs) {
  if (!pool || pool->num_workers == 0U || num_jobs <= 1U) {
    for (uint32_t i = 0; i < num_jobs; ++i) fn(ctx, i);
    return;
  }
  LONG g = pool->gen + 1;
  if (g == 0) g = 1; /* 0 is reserved for invalidated batch_gen */
  zcvid_pool_batch_t *b = &pool->batches[(uint32_t)g & (ZCVID_POOL_RING - 1U)];
  while (zcvid_atomic_load_acq(&b->active) != 0) {
    YieldProcessor();
  }
  b->fn = fn;
  b->ctx = ctx;
  b->num_jobs = (LONG)num_jobs;
  b->next = 0;
  b->done = 0;
  InterlockedExchange(&b->batch_gen, g);
  InterlockedExchange(&pool->gen, g);
  LONG slp = zcvid_atomic_load_acq(&pool->sleeping);
  if (slp > 0) {
    LONG wake = (slp < (LONG)(num_jobs - 1U)) ? slp : (LONG)(num_jobs - 1U);
    if (wake > 0) ReleaseSemaphore(pool->sem, wake, NULL);
  }
  zcvid_pool_drain_batch(b);
  while (zcvid_atomic_load_acq(&b->done) < (LONG)num_jobs) {
    YieldProcessor();
  }
  InterlockedExchange(&b->batch_gen, 0);
}

/* Scratch of one chunk coder: trit model and intra line buffers. */
static bool zcvid_ec_alloc(gpu_pipeline_t *p, uint32_t slot) {
  if (!p->zcvid_ec_line[slot]) p->zcvid_ec_line[slot] = (uint8_t *)malloc((size_t)p->width * 3U);
  if (!p->zcvid_ec_lrow[slot]) {
    p->zcvid_ec_lrow[slot] = (uint32_t *)calloc((size_t)p->width, sizeof(uint32_t));
    p->zcvid_ec_lrow_gen[slot] = 0U;
  }
  if (!p->zcvid_ec_model[slot]) p->zcvid_ec_model[slot] = malloc(sizeof(zcrudp_tec_model_t));
  return p->zcvid_ec_line[slot] && p->zcvid_ec_lrow[slot] && p->zcvid_ec_model[slot];
}

/* Chunks a frame of cost-weighted work total_w is cut into: unlocks full
 * multi-core parallelism on 1080p24/4K60 cinema decode while preserving strong
 * CABAC adaptation per chunk. */
static uint32_t zcvid_ec_chunks(const gpu_pipeline_t *p, uint64_t total_w) {
  uint64_t k = total_w / 4096ULL;
  uint32_t max_c = p->zcvid_tq ? ZCVID_EC_MAX_CHUNKS : 16U;
  uint32_t cap_c = p->zcvid_ec_threads;
  if (cap_c > max_c) cap_c = max_c;
  if (k > cap_c) k = cap_c;
  return (k < 1ULL) ? 1U : (uint32_t)k;
}

/* Worst-case size of a coded chunk the encoder is willing to produce. Real
 * streams are far smaller; one that is not is sent in the plain layout. */
static size_t zcvid_ec_chunk_bound(uint32_t num_records) {
  return 64U + (size_t)num_records * 12U;
}

/* Range-codes sorted records keys[0 .. num) into out. Returns the stream
 * size, or 0 when they cannot be coded (output full, or a record the layout
 * does not cover). */

/* ---------------------------------------------------------------------------
 * CABAC-grade Transform Entropy Coder v3 (class 8):
 *   - 8x8 Superblock addressing: one superblock gap + is_8x8 + 4-bit quadrant CBP
 *     cuts block-address overhead 3-4x vs raster 4x4 block gaps.
 *   - Per-plane 4-bit binary-tree last_sig_pos + binary significance map.
 *   - Context-adaptive gt1/gt2 + Exp-Golomb tail + fast bypass AC signs.
 *   - Unpacks to the exact GPU (w0, w1) record words (zero shader/CPU-dec changes).
 * ------------------------------------------------------------------------- */
typedef struct {
  zcrudp_rc_prob_t is_8x8;
  zcrudp_rc_prob_t qmask[9];           /* 0:b0, 1..2:b1|b0, 3..5:b2|(b0+b1), 6..8:b3|(b0+b1+b2-1) */
  zcrudp_rc_prob_t pred[2];
  zcrudp_rc_prob_t present[4];         /* 0:Y, 1:Cb, 2:Cr|!Cb, 3:Cr|Cb */
  zcrudp_rc_prob_t qp_same[2];
  zcrudp_rc_prob_t qp_is_alt[2];       /* 2-slot QP MRU cache for low-cost AQ switching */
  zcrudp_rc_prob_t has_ac[2][2];       /* [is_inter][chroma] */
  zcrudp_rc_prob_t last_k[2][2][14];   /* [is_inter][chroma][tree_node-1] */
  zcrudp_rc_prob_t sig[2][2][15];      /* [is_inter][chroma][k=0..14] */
  zcrudp_rc_prob_t intra_dc_nz[2];     /* [chroma]: intra DC DPCM residual != 0 */
  zcrudp_rc_prob_t dc_sign[2][2];      /* [is_inter][chroma] */
  zcrudp_rc_prob_t gt1[2][2][4];       /* [is_inter][chroma][kb=0..3] */
  zcrudp_rc_prob_t gt2[2][2][4];       /* [is_inter][chroma][kb=0..3] */
  zcrudp_rc_prob_t mag_rem[2][4][7];   /* [chroma][kb=0..3][egk_ctx] */
} zcvid_tq_model_t;

typedef struct {
  uint32_t last_qp[2];
  uint32_t alt_qp[2];
  int32_t  prev_intra_dc[3];
  uint32_t prev_intra_qp[3];
} zcvid_tq_state_t;

static void zcvid_tq_state_reset(zcvid_tq_state_t *st) {
  st->last_qp[0] = st->last_qp[1] = 64U;
  st->alt_qp[0]  = st->alt_qp[1]  = 64U;
  st->prev_intra_dc[0] = st->prev_intra_dc[1] = st->prev_intra_dc[2] = 0;
  st->prev_intra_qp[0] = st->prev_intra_qp[1] = st->prev_intra_qp[2] = 64U;
}

static zcvid_tq_model_t g_tq_model_tpl;
static volatile LONG g_tq_model_tpl_state = 0;

static void zcvid_tq_model_reset(zcvid_tq_model_t *m) {
  if (zcvid_atomic_load_acq(&g_tq_model_tpl_state) != 2) {
    if (InterlockedCompareExchange(&g_tq_model_tpl_state, 1, 0) == 0) {
      zcvid_tq_model_t *t = &g_tq_model_tpl;
      zcrudp_rc_prob_init(&t->is_8x8, 22000U);
      for (int i = 0; i < 9; ++i) zcrudp_rc_prob_init(&t->qmask[i], 16384U);
      zcrudp_rc_prob_init(&t->qmask[6], 26000U); /* sparse 1-of-3 prior -> b3 rarely set */
      zcrudp_rc_prob_init(&t->qmask[8], 8000U);  /* dense 3-of-3 prior -> b3 usually set */
      zcrudp_rc_prob_init(&t->pred[0], 6000U);
      zcrudp_rc_prob_init(&t->pred[1], 8000U);
      zcrudp_rc_prob_init(&t->present[0], 2500U);
      zcrudp_rc_prob_init(&t->present[1], 24000U);
      zcrudp_rc_prob_init(&t->present[2], 26000U);
      zcrudp_rc_prob_init(&t->present[3], 12000U);
      for (int i = 0; i < 2; ++i) {
        zcrudp_rc_prob_init(&t->qp_same[i], 30000U);
        zcrudp_rc_prob_init(&t->qp_is_alt[i], 16384U);
        zcrudp_rc_prob_init(&t->intra_dc_nz[i], 16384U);
        for (int j = 0; j < 2; ++j) {
          zcrudp_rc_prob_init(&t->has_ac[i][j], (j == 0) ? 12000U : 22000U);
          zcrudp_rc_prob_init(&t->dc_sign[i][j], 16384U);
          for (int c = 0; c < 14; ++c) zcrudp_rc_prob_init(&t->last_k[i][j][c], 16384U);
          for (int k = 0; k < 15; ++k) {
            uint16_t p0 = (k == 0) ? 9000U : (k < 4) ? 14000U : 20000U;
            zcrudp_rc_prob_init(&t->sig[i][j][k], p0);
          }
          for (int kb = 0; kb < 4; ++kb) {
            uint16_t p_gt1 = (kb == 0) ? ((i == 0) ? 20000U : 18000U) : (kb == 1) ? 23000U : 27000U;
            uint16_t p_gt2 = (kb == 0) ? ((i == 0) ? 22000U : 20000U) : 25000U;
            zcrudp_rc_prob_init(&t->gt1[i][j][kb], p_gt1);
            zcrudp_rc_prob_init(&t->gt2[i][j][kb], p_gt2);
          }
        }
        for (int kb = 0; kb < 4; ++kb) {
          for (int c = 0; c < 7; ++c) {
            int32_t p0 = 14000 + kb * 3000 + c * 2000;
            if (p0 > 28000) p0 = 28000;
            zcrudp_rc_prob_init(&t->mag_rem[i][kb][c], (uint16_t)p0);
          }
        }
      }
      InterlockedExchange(&g_tq_model_tpl_state, 2);
    } else {
      while (zcvid_atomic_load_acq(&g_tq_model_tpl_state) != 2) YieldProcessor();
    }
  }
  memcpy(m, &g_tq_model_tpl, sizeof(*m));
}

static inline uint32_t zcvid_tq_kb(uint32_t k) {
  static const uint8_t lut[16] = {0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3};
  return lut[k & 15U];
}

static inline bool zcvid_tq_put_block(zcrudp_rc_enc_t * __restrict rc,
                                     zcvid_tq_model_t * __restrict m,
                                     zcvid_tq_state_t * __restrict st,
                                     const uint64_t * __restrict keys,
                                     uint32_t i, uint32_t e,
                                     uint32_t blk_base, uint32_t is_8x8) {
  uint32_t pred = ((uint32_t)keys[i] >> 24) & 0xFFU;
  if (pred > 3U || ((pred >> 1) & 1U) != is_8x8) return false;
  uint32_t is_inter = pred & 1U;
  zcrudp_rc_enc_t lrc = *rc;
  zcrudp_rc_enc_bit(&lrc, &m->pred[is_8x8], is_inter);

  uint32_t p_beg[3], p_end[3];
  uint32_t j = i;
  for (uint32_t pl = 0; pl < 3U; ++pl) {
    uint32_t pl_hi = blk_base + (pl + 1U) * 16U;
    p_beg[pl] = j;
    while (j < e && (((uint32_t)(keys[j] >> 32) & 0x0FFFFFFFU) < pl_hi)) j++;
    p_end[pl] = j;
  }
  if (j != e) { *rc = lrc; return false; }

  uint32_t py = (p_end[0] > p_beg[0]) ? 1U : 0U;
  uint32_t pb = (p_end[1] > p_beg[1]) ? 1U : 0U;
  uint32_t pr = (p_end[2] > p_beg[2]) ? 1U : 0U;
  if ((py | pb | pr) == 0U) { *rc = lrc; return false; }
  zcrudp_rc_enc_bit(&lrc, &m->present[0], py);
  zcrudp_rc_enc_bit(&lrc, &m->present[1], pb);
  if ((py | pb) != 0U) zcrudp_rc_enc_bit(&lrc, &m->present[2U + pb], pr);

  for (uint32_t pl = 0; pl < 3U; ++pl) {
    if (p_end[pl] == p_beg[pl]) continue;
    uint32_t chroma = (pl > 0U) ? 1U : 0U;
    uint32_t qp = ((uint32_t)keys[p_beg[pl]] >> 16) & 0xFFU;
    if (qp > 51U) { *rc = lrc; return false; }
    if (qp == st->last_qp[chroma]) {
      zcrudp_rc_enc_bit(&lrc, &m->qp_same[chroma], 0U);
    } else {
      zcrudp_rc_enc_bit(&lrc, &m->qp_same[chroma], 1U);
      if (qp == st->alt_qp[chroma]) {
        zcrudp_rc_enc_bit(&lrc, &m->qp_is_alt[chroma], 1U);
        st->alt_qp[chroma] = st->last_qp[chroma];
        st->last_qp[chroma] = qp;
      } else {
        zcrudp_rc_enc_bit(&lrc, &m->qp_is_alt[chroma], 0U);
        zcrudp_rc_enc_direct(&lrc, qp, 6U);
        st->alt_qp[chroma] = st->last_qp[chroma];
        st->last_qp[chroma] = qp;
      }
    }

    int16_t lv[16];
    uint32_t sig_mask = 0U;
    int32_t prev_k = -1;
    uint32_t last_k = 0U;
    for (uint32_t t = p_beg[pl]; t < p_end[pl]; ++t) {
      uint64_t kv = keys[t];
      uint32_t w1 = (uint32_t)kv;
      uint32_t id = (uint32_t)(kv >> 32) & 0x0FFFFFFFU;
      uint32_t k = id & 15U;
      int32_t level = (int32_t)(int16_t)(w1 & 0xFFFFU);
      if (((w1 >> 16) & 0xFFU) != qp || ((w1 >> 24) & 0xFFU) != pred) { *rc = lrc; return false; }
      if ((int32_t)k <= prev_k || level < -255 || level > 255) { *rc = lrc; return false; }
      if ((k > 0U || is_inter != 0U) && level == 0) { *rc = lrc; return false; }
      lv[k] = (int16_t)level;
      sig_mask |= (1U << k);
      last_k = k;
      prev_k = (int32_t)k;
    }

    uint32_t has_ac = (last_k > 0U) ? 1U : 0U;
    zcrudp_rc_enc_bit(&lrc, &m->has_ac[is_inter][chroma], has_ac);
    if (has_ac) {
      uint32_t v = last_k - 1U;
      zcrudp_rc_prob_t *lk = m->last_k[is_inter][chroma];
      uint32_t b3 = (v >> 3) & 1U; zcrudp_rc_enc_bit(&lrc, &lk[0], b3);
      uint32_t n2 = 2U | b3;
      uint32_t b2 = (v >> 2) & 1U; zcrudp_rc_enc_bit(&lrc, &lk[n2 - 1U], b2);
      uint32_t n1 = (n2 << 1) | b2;
      uint32_t b1 = (v >> 1) & 1U; zcrudp_rc_enc_bit(&lrc, &lk[n1 - 1U], b1);
      uint32_t n0 = (n1 << 1) | b1;
      if (n0 != 15U) zcrudp_rc_enc_bit(&lrc, &lk[n0 - 1U], v & 1U);
      zcrudp_rc_prob_t *sp = m->sig[is_inter][chroma];
      for (uint32_t k = 0; k < last_k; ++k) {
        zcrudp_rc_enc_bit(&lrc, &sp[k], (sig_mask >> k) & 1U);
      }
    }

    if (is_inter == 0U && (sig_mask & 1U) == 0U) {
      st->prev_intra_dc[pl] = 0;
      st->prev_intra_qp[pl] = qp;
    }

    uint32_t sm = sig_mask;
    while (sm != 0U) {
      uint32_t k = zcvid_ctz32(sm);
      sm &= sm - 1U;
      int32_t val = lv[k];
      uint32_t egk_max = 7U;
      if (k == 0U && is_inter == 0U) {
        int32_t pred_dc = (qp == st->prev_intra_qp[pl]) ? st->prev_intra_dc[pl] : 0;
        st->prev_intra_dc[pl] = val;
        st->prev_intra_qp[pl] = qp;
        val -= pred_dc;
        uint32_t nz = (val != 0) ? 1U : 0U;
        zcrudp_rc_enc_bit(&lrc, &m->intra_dc_nz[chroma], nz);
        if (!nz) continue;
        egk_max = 8U;
      }
      uint32_t neg = (val < 0) ? 1U : 0U;
      if (k == 0U) {
        zcrudp_rc_enc_bit(&lrc, &m->dc_sign[is_inter][chroma], neg);
      } else {
        zcrudp_rc_enc_bit_bypass(&lrc, neg);
      }
      uint32_t mag = (uint32_t)(neg ? -val : val);
      uint32_t kb = zcvid_tq_kb(k);
      uint32_t gt1 = (mag > 1U) ? 1U : 0U;
      zcrudp_rc_enc_bit(&lrc, &m->gt1[is_inter][chroma][kb], gt1);
      if (gt1) {
        uint32_t gt2 = (mag > 2U) ? 1U : 0U;
        zcrudp_rc_enc_bit(&lrc, &m->gt2[is_inter][chroma][kb], gt2);
        if (gt2) {
          zcrudp_rc_enc_egk(&lrc, m->mag_rem[chroma][kb], 7U, egk_max, (uint64_t)(mag - 3U));
        }
      }
    }
  }
  *rc = lrc;
  return !lrc.overflow;
}

/* Codes keys[0 .. num) (all class 8, sorted in 8x8 superblock order).
 * Returns false when the records break the layout (caller falls back to plain). */
static bool zcvid_tq_put(zcrudp_rc_enc_t * __restrict rc, zcrudp_tec_model_t * __restrict tm,
                        const uint64_t * __restrict keys,
                        uint32_t num, uint32_t bw, uint32_t bh) {
  zcvid_tq_model_t m;
  zcvid_tq_model_reset(&m);
  zcvid_tq_state_t st;
  zcvid_tq_state_reset(&st);
  const uint32_t sbw = (bw + 1U) >> 1;
  const uint32_t sbh = (bh + 1U) >> 1;
  const uint32_t nsb = sbw * sbh;
  uint32_t prev_sb = UINT32_MAX;
  uint32_t i = 0;
  while (i < num) {
    uint32_t key0 = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
    uint32_t sb = (uint32_t)(((uint64_t)key0 * 0xAAAAAAABULL) >> 39);
    if (sb >= nsb || sb >= (1U << 20) || (prev_sb != UINT32_MAX && sb <= prev_sb)) return false;
    if (prev_sb == UINT32_MAX) {
      zcrudp_rc_enc_direct(rc, sb, 20U);
    } else {
      zcrudp_tec_enc_run(rc, tm, (uint64_t)(sb - prev_sb - 1U));
    }
    prev_sb = sb;

    uint32_t sb_lo = sb * 192U;
    uint32_t sb_hi = sb_lo + 192U;
    uint32_t se = i + 1U;
    while (se < num && (((uint32_t)(keys[se] >> 32) & 0x0FFFFFFFU) < sb_hi)) se++;

    uint32_t pred0 = ((uint32_t)keys[i] >> 24) & 0xFFU;
    if (pred0 > 3U) return false;
    uint32_t is_8x8 = (pred0 >> 1) & 1U;
    zcrudp_rc_enc_bit(rc, &m.is_8x8, is_8x8);

    if (is_8x8) {
      /* Verify all records in this 8x8 superblock are anchored at quadrant 0. */
      uint32_t q0_last = ((uint32_t)(keys[se - 1U] >> 32) & 0x0FFFFFFFU) - sb_lo;
      if (q0_last >= 48U) return false;
      if (!zcvid_tq_put_block(rc, &m, &st, keys, i, se, sb_lo, 1U)) return false;
    } else {
      uint32_t q_beg[4], q_end[4];
      uint32_t j = i;
      for (uint32_t q = 0; q < 4U; ++q) {
        uint32_t q_hi = sb_lo + (q + 1U) * 48U;
        q_beg[q] = j;
        while (j < se && (((uint32_t)(keys[j] >> 32) & 0x0FFFFFFFU) < q_hi)) j++;
        q_end[q] = j;
      }
      if (j != se) return false;
      uint32_t b0 = (q_end[0] > q_beg[0]) ? 1U : 0U;
      uint32_t b1 = (q_end[1] > q_beg[1]) ? 1U : 0U;
      uint32_t b2 = (q_end[2] > q_beg[2]) ? 1U : 0U;
      uint32_t b3 = (q_end[3] > q_beg[3]) ? 1U : 0U;
      if ((b0 | b1 | b2 | b3) == 0U) return false;
      zcrudp_rc_enc_bit(rc, &m.qmask[0], b0);
      zcrudp_rc_enc_bit(rc, &m.qmask[1U + b0], b1);
      zcrudp_rc_enc_bit(rc, &m.qmask[3U + b0 + b1], b2);
      if ((b0 | b1 | b2) != 0U) zcrudp_rc_enc_bit(rc, &m.qmask[5U + b0 + b1 + b2], b3);

      for (uint32_t q = 0; q < 4U; ++q) {
        if (q_end[q] == q_beg[q]) continue;
        if (!zcvid_tq_put_block(rc, &m, &st, keys, q_beg[q], q_end[q], sb_lo + q * 48U, 0U)) {
          return false;
        }
      }
    }
    i = se;
  }
  return true;
}

static inline bool zcvid_tq_get_block(zcrudp_rc_dec_t * __restrict rc,
                                     zcvid_tq_model_t * __restrict m,
                                     zcvid_tq_state_t * __restrict st,
                                     uint32_t b, uint32_t is_8x8, uint32_t count,
                                     uint32_t * __restrict got_io,
                                     uint32_t * __restrict out) {
  zcrudp_rc_dec_t lrc = *rc;
  uint32_t is_inter = zcrudp_rc_dec_bit(&lrc, &m->pred[is_8x8]);
  uint32_t pred = is_inter | (is_8x8 << 1);
  uint32_t py = zcrudp_rc_dec_bit(&lrc, &m->present[0]);
  uint32_t pb = zcrudp_rc_dec_bit(&lrc, &m->present[1]);
  uint32_t pr = ((py | pb) != 0U) ? zcrudp_rc_dec_bit(&lrc, &m->present[2U + pb]) : 1U;
  uint32_t pmask = py | (pb << 1) | (pr << 2);
  uint32_t got = *got_io;

  for (uint32_t pl = 0; pl < 3U; ++pl) {
    if (((pmask >> pl) & 1U) == 0U) continue;
    uint32_t chroma = (pl > 0U) ? 1U : 0U;
    if (zcrudp_rc_dec_bit(&lrc, &m->qp_same[chroma]) != 0U) {
      if (zcrudp_rc_dec_bit(&lrc, &m->qp_is_alt[chroma]) != 0U) {
        uint32_t tmp = st->last_qp[chroma];
        st->last_qp[chroma] = st->alt_qp[chroma];
        st->alt_qp[chroma] = tmp;
      } else {
        uint32_t nqp = (uint32_t)zcrudp_rc_dec_direct(&lrc, 6U);
        st->alt_qp[chroma] = st->last_qp[chroma];
        st->last_qp[chroma] = nqp;
      }
    }
    uint32_t qp = st->last_qp[chroma];
    if (qp > 51U) { *rc = lrc; return false; }

    uint32_t sig_mask = 1U;
    if (zcrudp_rc_dec_bit(&lrc, &m->has_ac[is_inter][chroma]) != 0U) {
      zcrudp_rc_prob_t *lk = m->last_k[is_inter][chroma];
      uint32_t node = 2U | zcrudp_rc_dec_bit(&lrc, &lk[0]);
      node = (node << 1) | zcrudp_rc_dec_bit(&lrc, &lk[node - 1U]);
      node = (node << 1) | zcrudp_rc_dec_bit(&lrc, &lk[node - 1U]);
      node = (node == 15U) ? 30U : ((node << 1) | zcrudp_rc_dec_bit(&lrc, &lk[node - 1U]));
      uint32_t last_k = node - 15U;
      sig_mask = 1U << last_k;
      zcrudp_rc_prob_t *sp = m->sig[is_inter][chroma];
      for (uint32_t k = 0; k < last_k; ++k) {
        sig_mask |= zcrudp_rc_dec_bit(&lrc, &sp[k]) << k;
      }
    }
    if (zcvid_popcount32(sig_mask) > count - got) { *rc = lrc; return false; }

    if (is_inter == 0U && (sig_mask & 1U) == 0U) {
      st->prev_intra_dc[pl] = 0;
      st->prev_intra_qp[pl] = qp;
    }

    uint32_t w0_base = 0xF0000000U | (b * 48U + pl * 16U);
    uint32_t w1_hi = (qp << 16) | (pred << 24);
    uint32_t sm = sig_mask;
    while (sm != 0U) {
      uint32_t k = zcvid_ctz32(sm);
      sm &= sm - 1U;
      int32_t level = 0;
      if (k == 0U && is_inter == 0U) {
        int32_t pred_dc = (qp == st->prev_intra_qp[pl]) ? st->prev_intra_dc[pl] : 0;
        int32_t val = 0;
        if (zcrudp_rc_dec_bit(&lrc, &m->intra_dc_nz[chroma]) != 0U) {
          uint32_t neg = zcrudp_rc_dec_bit(&lrc, &m->dc_sign[0][chroma]);
          int32_t mag = 1;
          if (zcrudp_rc_dec_bit(&lrc, &m->gt1[0][chroma][0]) != 0U) {
            mag = 2;
            if (zcrudp_rc_dec_bit(&lrc, &m->gt2[0][chroma][0]) != 0U) {
              mag = (int32_t)zcrudp_rc_dec_egk(&lrc, m->mag_rem[chroma][0], 7U, 8U) + 3;
              if (mag > 510) { *rc = lrc; return false; }
            }
          }
          val = neg ? -mag : mag;
        }
        level = pred_dc + val;
        if (level < -255 || level > 255) { *rc = lrc; return false; }
        st->prev_intra_dc[pl] = level;
        st->prev_intra_qp[pl] = qp;
      } else {
        uint32_t neg = (k == 0U)
                           ? zcrudp_rc_dec_bit(&lrc, &m->dc_sign[is_inter][chroma])
                           : zcrudp_rc_dec_bit_bypass(&lrc);
        uint32_t kb = zcvid_tq_kb(k);
        int32_t mag = 1;
        if (zcrudp_rc_dec_bit(&lrc, &m->gt1[is_inter][chroma][kb]) != 0U) {
          mag = 2;
          if (zcrudp_rc_dec_bit(&lrc, &m->gt2[is_inter][chroma][kb]) != 0U) {
            mag = (int32_t)zcrudp_rc_dec_egk(&lrc, m->mag_rem[chroma][kb], 7U, 7U) + 3;
            if (mag > 255) { *rc = lrc; return false; }
          }
        }
        level = neg ? -mag : mag;
      }
      out[got * 2U] = w0_base + k;
      out[got * 2U + 1U] = ((uint32_t)level & 0xFFFFU) | w1_hi;
      got++;
    }
  }
  *rc = lrc;
  *got_io = got;
  return true;
}

/* Decodes count records into out (2 words each). Returns false on a
 * malformed stream. */
static bool zcvid_tq_get(zcrudp_rc_dec_t * __restrict rc, zcrudp_tec_model_t * __restrict tm,
                        uint32_t count, uint32_t * __restrict out, uint32_t bw, uint32_t bh) {
  zcvid_tq_model_t m;
  zcvid_tq_model_reset(&m);
  zcvid_tq_state_t st;
  zcvid_tq_state_reset(&st);
  zcrudp_rc_dec_t lrc = *rc;
  const uint32_t sbw = (bw + 1U) >> 1;
  const uint32_t sbh = (bh + 1U) >> 1;
  const uint32_t nsb = sbw * sbh;
  uint32_t prev_sb = UINT32_MAX;
  uint32_t sbx = 0U, sby = 0U;
  uint32_t got = 0;
  while (got < count) {
    uint32_t sb;
    if (prev_sb == UINT32_MAX) {
      sb = (uint32_t)zcrudp_rc_dec_direct(&lrc, 20U);
      if (sb >= nsb) { *rc = lrc; return false; }
      sby = sb / sbw;
      sbx = sb - sby * sbw;
    } else {
      uint64_t gap = zcrudp_tec_dec_run(&lrc, tm);
      if (gap >= (uint64_t)(nsb - prev_sb - 1U)) { *rc = lrc; return false; }
      uint32_t step = 1U + (uint32_t)gap;
      sb = prev_sb + step;
      sbx += step;
      if (sbx >= sbw) {
        if (sbx < sbw * 2U) {
          sbx -= sbw;
          sby++;
        } else {
          sby = sb / sbw;
          sbx = sb - sby * sbw;
        }
      }
    }
    prev_sb = sb;

    uint32_t is_8x8 = zcrudp_rc_dec_bit(&lrc, &m.is_8x8);
    if (is_8x8) {
      uint32_t bx = sbx * 2U, by = sby * 2U;
      if (bx >= bw || by >= bh) { *rc = lrc; return false; }
      if (!zcvid_tq_get_block(&lrc, &m, &st, by * bw + bx, 1U, count, &got, out)) { *rc = lrc; return false; }
    } else {
      uint32_t b0 = zcrudp_rc_dec_bit(&lrc, &m.qmask[0]);
      uint32_t b1 = zcrudp_rc_dec_bit(&lrc, &m.qmask[1U + b0]);
      uint32_t b2 = zcrudp_rc_dec_bit(&lrc, &m.qmask[3U + b0 + b1]);
      uint32_t b3 = ((b0 | b1 | b2) != 0U) ? zcrudp_rc_dec_bit(&lrc, &m.qmask[5U + b0 + b1 + b2]) : 1U;
      uint32_t qmask = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3);
      for (uint32_t q = 0; q < 4U; ++q) {
        if (((qmask >> q) & 1U) == 0U) continue;
        uint32_t bx = sbx * 2U + (q & 1U);
        uint32_t by = sby * 2U + (q >> 1);
        if (bx >= bw || by >= bh) { *rc = lrc; return false; }
        if (!zcvid_tq_get_block(&lrc, &m, &st, by * bw + bx, 0U, count, &got, out)) { *rc = lrc; return false; }
      }
    }
  }
  *rc = lrc;
  return got == count;
}

static size_t zcvid_ec_put_chunk(gpu_pipeline_t * __restrict p, uint32_t slot,
                                const uint32_t * __restrict rec,
                                const uint64_t * __restrict keys, uint32_t num, uint32_t ncls,
                                uint8_t * __restrict out, size_t out_cap) {
  zcrudp_tec_model_t *tm = (zcrudp_tec_model_t *)p->zcvid_ec_model[slot];
  const uint32_t num_px = p->width * p->height;
  uint32_t counts[ZCVID_CLASSES_MAX] = {0};
  for (uint32_t n = 0; n < num; ++n) counts[keys[n] >> 60]++;

  zcrudp_rc_enc_t rc;
  zcrudp_rc_enc_init(&rc, out, out_cap);
  zcvid_ec_intra_t im;
  uint32_t i = 0;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    if (counts[cls] >> ZCVID_EC_COUNT_BITS) return 0;
    zcrudp_rc_enc_direct(&rc, counts[cls], ZCVID_EC_COUNT_BITS);
    if (counts[cls] == 0U) continue;
    if (cls < 6U) {
      zcrudp_tec_model_reset(tm, zcvid_ec_delta_stride(p->width, cls), 1U);
    } else if (cls == 8U) {
      zcrudp_tec_run_reset(tm);
      if (!zcvid_tq_put(&rc, tm, keys + i, counts[cls],
                       (p->width + 3U) >> 2, (p->height + 3U) >> 2)) return 0;
      i += counts[cls];
      continue;
    } else {
      zcrudp_tec_run_reset(tm);
      zcvid_ec_intra_reset(&im, p->zcvid_ec_line[slot], p->zcvid_ec_lrow[slot],
                          &p->zcvid_ec_lrow_gen[slot], p->width, p->height);
    }
    /* Positions count blocks (delta), pixels (2-px intra) or pixel pairs
     * (4-px intra). span is a record's extent in those units, px in pixels. */
    const uint32_t unit = (cls == 7U) ? 2U : 1U;
    const uint32_t px = (cls < 6U) ? 0U : (cls == 6U) ? 2U : 4U;
    const uint32_t span = (cls < 6U) ? 1U : px / unit;
    const uint32_t id_limit = zcvid_ec_id_limit(num_px, cls);
    uint32_t next = 0; /* first position the next record may take */
    uint32_t cur_px = 0U, cur_row = 0U, cur_x = 0U;
    for (uint32_t n = 0; n < counts[cls]; ++n, ++i) {
      uint32_t lo = (uint32_t)keys[i];
      uint32_t id = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
      uint32_t w0 = (cls == 7U) ? rec[lo * 2U] : 0U;
      uint32_t w1 = (cls == 7U) ? rec[lo * 2U + 1U] : lo;
      /* Duplicated, overlapping or out-of-frame record: not representable. */
      if (id < next || id >= id_limit) return 0;
      zcrudp_tec_enc_run(&rc, tm, id - next);
      if (cls < 6U) {
        if (w1 >= 59049U) return 0;
        zcrudp_tec_hist_clear(tm, (size_t)next * 10U, (size_t)id * 10U);
        zcvid_ec_put_trits(&rc, tm, (size_t)id * 10U, w1);
      } else {
        const unsigned shift = (cls == 6U) ? 0U : 2U;
        uint32_t y[4] = {0, 0, 0, 0}, cb, cr;
        if (cls == 6U) {
          y[0] = w1 & 0xFFU;
          cb = (w1 >> 8) & 0xFFU;
          y[1] = (w1 >> 16) & 0xFFU;
          cr = w1 >> 24;
        } else {
          if ((w1 >> 30) != 0U || ((w0 >> 28) & 1U) != 0U) return 0;
          for (unsigned k = 0; k < 4U; ++k) y[k] = (w1 >> (6U * k)) & 0x3FU;
          cb = (w1 >> 24) & 0x3FU;
          cr = (w0 >> 22) & 0x3FU;
        }
        uint8_t cb8 = (cls == 6U) ? (uint8_t)cb : (uint8_t)(cb << 2);
        uint8_t cr8 = (cls == 6U) ? (uint8_t)cr : (uint8_t)(cr << 2);
        uint32_t pidx = id * unit;
        if (pidx != cur_px) {
          zcvid_ec_idx_to_xy(&im, pidx, &cur_row, &cur_x);
          cur_px = pidx;
        }
        zcvid_ec_nb_t nb;
        uint32_t grad = 0, pred;
        zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
        pred = zcvid_ec_predict(&nb, 1U, &grad);
        zcvid_ec_put_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift, cb);
        pred = zcvid_ec_predict(&nb, 2U, &grad);
        zcvid_ec_put_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift, cr);
        for (unsigned k = 0; k < px; ++k) {
          if (k > 0U) zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
          pred = zcvid_ec_predict(&nb, 0U, &grad);
          zcvid_ec_put_sample(&rc, &im.luma, &im.act_y, pred, grad, shift, y[k]);
          zcvid_ec_store_xy(&im, cur_px, cur_row, cur_x,
                           (cls == 6U) ? (uint8_t)y[k] : zcvid_ec_expand6(y[k]), cb8, cr8);
          cur_px++;
          if (++cur_x == im.width) { cur_x = 0U; cur_row++; }
        }
      }
      next = id + span;
      if (rc.overflow) return 0;
    }
  }
  size_t len = zcrudp_rc_enc_finish(&rc);
  if (rc.overflow) return 0;
  return (len > 0U) ? len : 1U; /* 0 means "not coded" to the caller */
}

/* Expands one chunk back to 2 words per record. Returns the word count, or
 * UINT32_MAX on a malformed stream. */
static uint32_t zcvid_ec_get_chunk(gpu_pipeline_t * __restrict p, uint32_t slot,
                                  const uint8_t * __restrict in,
                                  size_t in_len, uint32_t * __restrict out, uint32_t max_words,
                                  uint32_t ncls, bool * __restrict out_has_tq) {
  zcrudp_tec_model_t *tm = (zcrudp_tec_model_t *)p->zcvid_ec_model[slot];
  const uint32_t num_px = p->width * p->height;
  zcrudp_rc_dec_t rc;
  zcrudp_rc_dec_init(&rc, in, in_len);
  zcvid_ec_intra_t im;
  uint32_t words = 0;
  bool has_tq = false;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    uint32_t count = (uint32_t)zcrudp_rc_dec_direct(&rc, ZCVID_EC_COUNT_BITS);
    if (count == 0U) continue;
    if (count > (max_words - words) / 2U) return UINT32_MAX;
    if (cls < 6U) {
      zcrudp_tec_model_reset(tm, zcvid_ec_delta_stride(p->width, cls), 1U);
    } else if (cls == 8U) {
      zcrudp_tec_run_reset(tm);
      if (!zcvid_tq_get(&rc, tm, count, out + words,
                       (p->width + 3U) >> 2, (p->height + 3U) >> 2)) return UINT32_MAX;
      has_tq = true;
      words += count * 2U;
      continue;
    } else {
      zcrudp_tec_run_reset(tm);
      zcvid_ec_intra_reset(&im, p->zcvid_ec_line[slot], p->zcvid_ec_lrow[slot],
                          &p->zcvid_ec_lrow_gen[slot], p->width, p->height);
    }
    /* Positions count blocks (delta), pixels (2-px intra) or pixel pairs
     * (4-px intra). span is a record's extent in those units, px in pixels. */
    const uint32_t unit = (cls == 7U) ? 2U : 1U;
    const uint32_t px = (cls < 6U) ? 0U : (cls == 6U) ? 2U : 4U;
    const uint32_t span = (cls < 6U) ? 1U : px / unit;
    const uint32_t id_limit = zcvid_ec_id_limit(num_px, cls);
    uint32_t next = 0;
    uint32_t cur_px = 0U, cur_row = 0U, cur_x = 0U;
    for (uint32_t n = 0; n < count; ++n) {
      uint64_t run = zcrudp_tec_dec_run(&rc, tm);
      if (run >= id_limit || next + (uint32_t)run >= id_limit) return UINT32_MAX;
      uint32_t id = next + (uint32_t)run;
      uint32_t w0, w1;
      if (cls < 6U) {
        zcrudp_tec_hist_clear(tm, (size_t)next * 10U, (size_t)id * 10U);
        w1 = zcvid_ec_get_trits(&rc, tm, (size_t)id * 10U);
        w0 = ((cls >> 1) << 30) | ((cls & 1U) << 29) | id;
      } else {
        const unsigned shift = (cls == 6U) ? 0U : 2U;
        uint32_t y[4] = {0, 0, 0, 0}, cb, cr;
        uint32_t pidx = id * unit;
        if (pidx != cur_px) {
          zcvid_ec_idx_to_xy(&im, pidx, &cur_row, &cur_x);
          cur_px = pidx;
        }
        zcvid_ec_nb_t nb;
        uint32_t grad = 0, pred;
        zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
        pred = zcvid_ec_predict(&nb, 1U, &grad);
        cb = zcvid_ec_get_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift);
        pred = zcvid_ec_predict(&nb, 2U, &grad);
        cr = zcvid_ec_get_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift);
        uint8_t cb8 = (cls == 6U) ? (uint8_t)cb : (uint8_t)(cb << 2);
        uint8_t cr8 = (cls == 6U) ? (uint8_t)cr : (uint8_t)(cr << 2);
        for (unsigned k = 0; k < px; ++k) {
          if (k > 0U) zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
          pred = zcvid_ec_predict(&nb, 0U, &grad);
          y[k] = zcvid_ec_get_sample(&rc, &im.luma, &im.act_y, pred, grad, shift);
          zcvid_ec_store_xy(&im, cur_px, cur_row, cur_x,
                           (cls == 6U) ? (uint8_t)y[k] : zcvid_ec_expand6(y[k]), cb8, cr8);
          cur_px++;
          if (++cur_x == im.width) { cur_x = 0U; cur_row++; }
        }
        if (cls == 6U) {
          w0 = 0xC0000000U | id;
          w1 = y[0] | (cb << 8) | (y[1] << 16) | (cr << 24);
        } else {
          w0 = 0xE0000000U | (cr << 22) | (id & 0x3FFFFFU);
          w1 = y[0] | (y[1] << 6) | (y[2] << 12) | (y[3] << 18) | (cb << 24);
        }
      }
      out[words++] = w0;
      out[words++] = w1;
      next = id + span;
    }
  }
  if (out_has_tq) *out_has_tq = has_tq;
  return words;
}

static bool zcvid_ec_unpack_mvs(const uint8_t *in, uint32_t in_len, int32_t *dst,
                               uint32_t tiles_x, uint32_t tiles_y, bool *out_qpel);

typedef struct {
  const uint8_t *in;
  uint32_t len;
  int32_t *dst;
  uint32_t tiles_x;
  uint32_t tiles_y;
  bool qpel;
  bool ok;
} zcvid_mv_dec_job_t;

typedef struct {
  gpu_pipeline_t *p;
  const uint32_t *rec;      /* encoder: raw records */
  const uint64_t *keys;     /* encoder: sorted keys */
  uint32_t first[ZCVID_EC_MAX_CHUNKS + 1U]; /* encoder: first record of each chunk */
  uint8_t *buf[ZCVID_EC_MAX_CHUNKS];        /* coded stream of each chunk */
  size_t cap[ZCVID_EC_MAX_CHUNKS];          /* encoder: room; decoder: stream size */
  size_t len[ZCVID_EC_MAX_CHUNKS];          /* encoder: coded size, 0 = failed */
  uint32_t *out[ZCVID_EC_MAX_CHUNKS];       /* decoder: where the chunk's words go */
  uint32_t words[ZCVID_EC_MAX_CHUNKS];      /* decoder: words announced */
  bool ok[ZCVID_EC_MAX_CHUNKS];             /* decoder: chunk decoded as announced */
  bool has_tq[ZCVID_EC_MAX_CHUNKS];         /* decoder: chunk contained class-8 transform records */
  double chunk_us[ZCVID_EC_MAX_CHUNKS];     /* ZCVID_PROFILE per-chunk wall time */
  zcvid_mv_dec_job_t *mv_job;               /* optional overlapped MV decode (job 0) */
  uint32_t ncls;                           /* record classes in the layout */
} zcvid_ec_job_t;

static void zcvid_ec_put_job(void *ctx, uint32_t i) {
  zcvid_ec_job_t *j = (zcvid_ec_job_t *)ctx;
  LARGE_INTEGER t0, t1;
  bool prof = j->p->zcvid_prof_en;
  if (prof) QueryPerformanceCounter(&t0);
  j->len[i] = zcvid_ec_put_chunk(j->p, i, j->rec, j->keys + j->first[i],
                                j->first[i + 1U] - j->first[i], j->ncls, j->buf[i], j->cap[i]);
  if (prof) {
    QueryPerformanceCounter(&t1);
    j->chunk_us[i] = qpc_delta_us(t0, t1);
  }
}

static void zcvid_ec_get_job(void *ctx, uint32_t idx) {
  zcvid_ec_job_t *j = (zcvid_ec_job_t *)ctx;
  uint32_t i = idx;
  if (j->mv_job) {
    if (idx == 0U) {
      zcvid_mv_dec_job_t *mv = j->mv_job;
      mv->ok = zcvid_ec_unpack_mvs(mv->in, mv->len, mv->dst, mv->tiles_x, mv->tiles_y, &mv->qpel);
      return;
    }
    i = idx - 1U;
  }
  LARGE_INTEGER t0, t1;
  bool prof = j->p->zcvid_prof_en;
  if (prof) QueryPerformanceCounter(&t0);
  j->has_tq[i] = false;
  j->ok[i] = zcvid_ec_get_chunk(j->p, i, j->buf[i], j->cap[i], j->out[i], j->words[i],
                               j->ncls, &j->has_tq[i]) == j->words[i];
  if (prof) {
    QueryPerformanceCounter(&t1);
    j->chunk_us[i] = qpc_delta_us(t0, t1);
  }
}

/* Range-coded payload: [u8 chunks][per chunk: u32 record words, u32 bytes]
 * [chunk streams][plain packed records, when the chunks do not hold them all].
 * At most ec_records are range-coded, taken from the end of the sorted order
 * (intra first: that is where a coded record saves the most); the others
 * follow in the plain packed layout. Returns the payload size, or 0 when the
 * records cannot be range-coded (no memory, output full, or a record the
 * layout does not cover): the caller then falls back on zcvid_pack_records. */
static size_t zcvid_ec_pack_records(gpu_pipeline_t *p, const uint32_t *rec,
                                   uint32_t num_records, uint32_t ec_records,
                                   uint8_t *out, size_t out_cap) {
  LARGE_INTEGER ps0, ps1, pp0, pp1;
  if (p->zcvid_prof_en) QueryPerformanceCounter(&ps0);
  uint32_t counts[ZCVID_CLASSES_MAX];
  zcvid_tq_bkt_info_t bkt_info;
  const uint64_t *all_keys = zcvid_sort_records_ex(p, rec, &num_records, counts, true, &bkt_info);
  if (p->zcvid_prof_en) QueryPerformanceCounter(&ps1);
  if (!all_keys) return 0;
  if (ec_records > num_records) ec_records = num_records;
  if (ec_records == 0U) return 0;
  uint32_t plain_records = num_records - ec_records;
  const uint32_t non_tq = num_records - counts[8];
  if (plain_records > non_tq && plain_records < num_records) {
    uint32_t sb = ((uint32_t)(all_keys[plain_records - 1U] >> 32) & 0x0FFFFFFFU) / 192U;
    while (plain_records < num_records && (all_keys[plain_records] >> 60) == 8U &&
           (((uint32_t)(all_keys[plain_records] >> 32) & 0x0FFFFFFFU) / 192U) == sb) {
      plain_records++;
    }
    ec_records = num_records - plain_records;
    if (ec_records == 0U) return 0;
  }
  const uint64_t *keys = all_keys + plain_records;
  num_records = ec_records;

  zcvid_ec_job_t job;
  job.p = p;
  job.rec = rec;
  job.keys = keys;
  job.mv_job = NULL;
  job.ncls = p->zcvid_ncls;
  /* Measured RC-decision cost weights per record class:
   *   delta (0..5): 20; intra2 (6): 24; intra4 (7): 34;
   *   class-8 tq: +4 new SB, +3 new 4x4 block, +2 extra coef. */
  static const uint32_t cls_w[ZCVID_CLASSES_MAX] = {20U, 20U, 20U, 20U, 20U, 20U, 24U, 34U, 1U};
  uint32_t ec_cnt[ZCVID_CLASSES_MAX] = {0};
  uint32_t rem_skip = plain_records;
  uint64_t non_tq_w = 0;
  for (uint32_t k = 0; k < ZCVID_CLASSES_MAX; ++k) {
    uint32_t ck = counts[k];
    if (rem_skip >= ck) {
      rem_skip -= ck;
    } else {
      ec_cnt[k] = ck - rem_skip;
      rem_skip = 0U;
      if (k < 8U) non_tq_w += (uint64_t)ec_cnt[k] * cls_w[k];
    }
  }
  const uint32_t tq_start = num_records - ec_cnt[8];
  const bool use_bkt = bkt_info.valid && (plain_records <= non_tq) && (ec_cnt[8] > 0U);
  uint64_t tq_w = 0;
  if (use_bkt) {
    for (uint32_t bk = 0; bk < ZCVID_SORT_CLS8_BKTS; ++bk) tq_w += bkt_info.bkt_w[bk];
  } else if (ec_cnt[8] > 0U) {
    uint32_t sb_end = 0U, blk_end = 0U, pl_end = 0U, prev_k = 0U;
    for (uint32_t i = tq_start; i < num_records; ++i) {
      uint32_t id = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
      tq_w += zcvid_tq_rec_weight(id, &sb_end, &blk_end, &pl_end, &prev_k);
    }
  }
  uint64_t total_w = non_tq_w + tq_w;
  uint32_t want_chunks = zcvid_ec_chunks(p, total_w);
  uint32_t raw_first[ZCVID_EC_MAX_CHUNKS + 1U];
  raw_first[0] = 0U;
  uint32_t tq_scan_pos = tq_start;
  uint64_t tq_scan_acc = non_tq_w;
  uint32_t tq_scan_sb_end = 0U, tq_scan_blk_end = 0U, tq_scan_pl_end = 0U, tq_scan_prev_k = 0U;
  uint32_t bkt_idx = 0U;
  for (uint32_t c = 1; c < want_chunks; ++c) {
    uint64_t target_w = (total_w * c) / want_chunks;
    uint32_t pos = 0U;
    if (target_w <= non_tq_w || ec_cnt[8] == 0U) {
      uint64_t acc_w = 0;
      for (uint32_t k = 0; k < 8U; ++k) {
        uint64_t span_w = (uint64_t)ec_cnt[k] * cls_w[k];
        if (acc_w + span_w >= target_w) {
          pos += (uint32_t)((target_w - acc_w) / cls_w[k]);
          break;
        }
        acc_w += span_w;
        pos += ec_cnt[k];
      }
    } else {
      if (use_bkt) {
        while (bkt_idx < ZCVID_SORT_CLS8_BKTS) {
          uint32_t bbeg = bkt_info.bkt_beg[bkt_idx] - plain_records;
          uint32_t bcnt = bkt_info.bkt_cnt[bkt_idx];
          if (tq_scan_pos >= bbeg + bcnt) {
            bkt_idx++;
            continue;
          }
          if (tq_scan_pos == bbeg && tq_scan_acc + bkt_info.bkt_w[bkt_idx] < target_w) {
            tq_scan_acc += bkt_info.bkt_w[bkt_idx];
            tq_scan_pos = bbeg + bcnt;
            tq_scan_sb_end = 0U;
            tq_scan_blk_end = 0U;
            tq_scan_pl_end = 0U;
            tq_scan_prev_k = 0U;
            bkt_idx++;
            continue;
          }
          break;
        }
      }
      while (tq_scan_pos < num_records) {
        uint32_t id = (uint32_t)(keys[tq_scan_pos] >> 32) & 0x0FFFFFFFU;
        if (id >= tq_scan_sb_end && tq_scan_acc >= target_w && tq_scan_pos > raw_first[c - 1U]) break;
        tq_scan_acc += zcvid_tq_rec_weight(id, &tq_scan_sb_end, &tq_scan_blk_end,
                                          &tq_scan_pl_end, &tq_scan_prev_k);
        tq_scan_pos++;
      }
      pos = tq_scan_pos;
    }
    if (pos > num_records) pos = num_records;
    if (pos < raw_first[c - 1U]) pos = raw_first[c - 1U];
    raw_first[c] = pos;
  }
  raw_first[want_chunks] = num_records;

  uint32_t chunks = 0;
  job.first[0] = 0U;
  for (uint32_t c = 0; c < want_chunks; ++c) {
    if (raw_first[c + 1U] > raw_first[c]) {
      job.first[chunks] = raw_first[c];
      job.first[chunks + 1U] = raw_first[c + 1U];
      chunks++;
    }
  }
  if (chunks == 0U) return 0;

  size_t need = 0;
  for (uint32_t c = 0; c < chunks; ++c) {
    if (!zcvid_ec_alloc(p, c)) return 0;
    job.cap[c] = zcvid_ec_chunk_bound(job.first[c + 1U] - job.first[c]);
    need += job.cap[c];
  }
  if (need > p->zcvid_ec_scratch_cap) {
    if (p->zcvid_ec_scratch_cap != 0U) zcvid_note_late_alloc("range coder scratch", need + need / 2U);
    uint8_t *s = (uint8_t *)realloc(p->zcvid_ec_scratch, need + need / 2U);
    if (!s) return 0;
    p->zcvid_ec_scratch = s;
    p->zcvid_ec_scratch_cap = need + need / 2U;
  }
  size_t off = 0;
  for (uint32_t c = 0; c < chunks; ++c) {
    job.buf[c] = p->zcvid_ec_scratch + off;
    off += job.cap[c];
  }
  if (chunks > 1U && !p->zcvid_pool) p->zcvid_pool = zcvid_pool_create(p->zcvid_ec_threads - 1U);
  if (p->zcvid_prof_en) QueryPerformanceCounter(&pp0);
  zcvid_pool_run(p->zcvid_pool, zcvid_ec_put_job, &job, chunks);
  if (p->zcvid_prof_en && p->zcvid_stg_idx > 30U) {
    QueryPerformanceCounter(&pp1);
    double mx = 0.0;
    for (uint32_t c = 0; c < chunks; ++c) {
      if (job.chunk_us[c] > mx) mx = job.chunk_us[c];
    }
    p->zcvid_ec_prof_acc[0] += qpc_delta_us(ps0, ps1);
    p->zcvid_ec_prof_acc[1] += qpc_delta_us(pp0, pp1);
    p->zcvid_ec_prof_acc[2] += mx;
    uint32_t d_cnt = ec_cnt[0] + ec_cnt[1] + ec_cnt[2] + ec_cnt[3] + ec_cnt[4] + ec_cnt[5];
    uint32_t i_cnt = ec_cnt[6] + ec_cnt[7];
    p->zcvid_ec_prof_acc[5] += (double)d_cnt;
    p->zcvid_ec_prof_acc[6] += (double)i_cnt;
    p->zcvid_ec_prof_acc[7] += (double)ec_cnt[8];
    p->zcvid_ec_prof_chunks = chunks;
    p->zcvid_ec_prof_enc_n++;
  }

  size_t total = 1U + (size_t)chunks * 8U;
  for (uint32_t c = 0; c < chunks; ++c) {
    if (job.len[c] == 0U) return 0;
    total += job.len[c];
  }
  if (total > out_cap) return 0;
  uint8_t *o = out;
  *o++ = (uint8_t)chunks;
  for (uint32_t c = 0; c < chunks; ++c) {
    uint32_t words = (job.first[c + 1U] - job.first[c]) * 2U;
    uint32_t bytes = (uint32_t)job.len[c];
    memcpy(o, &words, 4);
    memcpy(o + 4, &bytes, 4);
    o += 8;
  }
  for (uint32_t c = 0; c < chunks; ++c) {
    memcpy(o, job.buf[c], job.len[c]);
    o += job.len[c];
  }
  if (plain_records > 0U) {
    if (total + zcvid_pack_bound(plain_records) > out_cap) return 0;
    const uint64_t *plain_keys = all_keys;
    if (plain_records > non_tq) {
      /* Plain tail overlaps class 8: re-sort its class-8 subset in raster order. */
      const uint32_t bw = (p->width + 3U) >> 2;
      const uint32_t sbw = (bw + 1U) >> 1;
      uint64_t *a = (all_keys == p->zcvid_sort_a) ? p->zcvid_sort_b : p->zcvid_sort_a;
      uint64_t *b = (a == p->zcvid_sort_a) ? p->zcvid_sort_b : p->zcvid_sort_a;
      memcpy(a, all_keys, (size_t)non_tq * sizeof(uint64_t));
      uint32_t tq_n = plain_records - non_tq;
      uint64_t *ta = a + non_tq;
      uint64_t *tb = b + non_tq;
      uint32_t kor = 0U, kand = 0xFFFFFFFFU;
      for (uint32_t k = 0; k < tq_n; ++k) {
        uint64_t kv = all_keys[non_tq + k];
        uint32_t sb_id = (uint32_t)(kv >> 32) & 0x0FFFFFFFU;
        uint32_t sbq = (uint32_t)(((uint64_t)sb_id * 0xAAAAAAABULL) >> 37);
        uint32_t rem = sb_id - sbq * 48U;
        uint32_t sb = sbq >> 2;
        uint32_t quad = sbq & 3U;
        uint32_t sby = sb / sbw;
        uint32_t sbx = sb - sby * sbw;
        uint32_t raw_id = (((sby << 1) | (quad >> 1)) * bw + ((sbx << 1) | (quad & 1U))) * 48U + rem;
        uint32_t k32 = (8U << 28) | raw_id;
        kor |= k32;
        kand &= k32;
        ta[k] = ((uint64_t)k32 << 32) | (kv & 0xFFFFFFFFULL);
      }
      zcvid_radix_sort_256(ta, tb, tq_n, kor ^ kand);
      plain_keys = a;
    }
    total += zcvid_pack_sorted(rec, plain_keys, plain_records, p->zcvid_ncls, o);
  }
  return total;
}

/* Expands a range-coded payload back to 2 words per record, optionally
 * overlapping tile MV range-decoding (mv_job) on the worker pool. Returns
 * the word count, or UINT32_MAX on a malformed stream. */
static uint32_t zcvid_ec_unpack_records_ex(gpu_pipeline_t *p, const uint8_t *in, size_t in_len,
                                          uint32_t *out, uint32_t max_words, uint32_t ncls,
                                          zcvid_mv_dec_job_t *mv_job) {
  if (in_len < 1U) return UINT32_MAX;
  const uint32_t chunks = in[0];
  if (chunks < 1U || chunks > ZCVID_EC_MAX_CHUNKS || in_len < 1U + (size_t)chunks * 8U) {
    return UINT32_MAX;
  }
  zcvid_ec_job_t job;
  job.p = p;
  job.mv_job = mv_job;
  job.ncls = ncls;
  const uint8_t *data = in + 1U + (size_t)chunks * 8U;
  size_t left = in_len - 1U - (size_t)chunks * 8U;
  uint32_t total_words = 0;
  for (uint32_t c = 0; c < chunks; ++c) {
    uint32_t words = 0, bytes = 0;
    memcpy(&words, in + 1U + (size_t)c * 8U, 4);
    memcpy(&bytes, in + 5U + (size_t)c * 8U, 4);
    if ((words & 1U) != 0U || words > max_words - total_words || bytes > left) {
      return UINT32_MAX;
    }
    if (!zcvid_ec_alloc(p, c)) return UINT32_MAX;
    job.buf[c] = (uint8_t *)data; /* read only */
    job.cap[c] = bytes;
    job.out[c] = out + total_words;
    job.words[c] = words;
    job.ok[c] = false;
    data += bytes;
    left -= bytes;
    total_words += words;
  }
  uint32_t total_jobs = chunks + (mv_job ? 1U : 0U);
  if (total_jobs > 1U && !p->zcvid_pool) p->zcvid_pool = zcvid_pool_create(p->zcvid_ec_threads - 1U);
  zcvid_pool_run(p->zcvid_pool, zcvid_ec_get_job, &job, total_jobs);
  if (p->zcvid_prof_en && p->dec_frame_idx >= 30U) {
    double mx = 0.0, mn = 1e12;
    for (uint32_t c = 0; c < chunks; ++c) {
      if (job.chunk_us[c] > mx) mx = job.chunk_us[c];
      if (job.chunk_us[c] < mn) mn = job.chunk_us[c];
    }
    p->zcvid_ec_prof_acc[3] += mx;
    p->zcvid_ec_prof_acc[4] += (mn < 1e11) ? mn : 0.0;
    p->zcvid_ec_prof_dec_n++;
  }
  if (mv_job && !mv_job->ok) return UINT32_MAX;
  bool any_tq = false;
  for (uint32_t c = 0; c < chunks; ++c) {
    if (!job.ok[c]) return UINT32_MAX;
    if (job.has_tq[c]) any_tq = true;
  }
  if (total_words < max_words) {
    /* The records the encoder had no time to range-code. */
    uint32_t plain = zcvid_unpack_records(data, left, out + total_words,
                                         max_words - total_words, ncls);
    if (plain == UINT32_MAX) return UINT32_MAX;
    if (ncls >= 9U && plain > 0U) any_tq = true;
    total_words += plain;
  }
  p->zcvid_dec_has_tq = any_tq;
  return total_words;
}

static inline uint32_t zcvid_ec_unpack_records(gpu_pipeline_t *p, const uint8_t *in, size_t in_len,
                                              uint32_t *out, uint32_t max_words, uint32_t ncls) {
  return zcvid_ec_unpack_records_ex(p, in, in_len, out, max_words, ncls, NULL);
}

/* Record words per frame a side can afford to range-code or decode.
 * Adapts smoothly across 4K cinema (24-60Hz, generous EC time) and
 * ultra-high-refresh gaming (240-480Hz, tight latency budget). */
static uint32_t zcvid_ec_budget_words(const gpu_pipeline_t *p, uint32_t nsw_q8) {
  uint32_t fps = (p->fps > 0U) ? p->fps : 60U;
  uint32_t pct = (fps <= 60U) ? 35U : (fps <= 144U) ? 25U : 18U;
  uint64_t budget_ns = (1000000000ULL / fps) * pct / 100U;
  uint64_t words = (budget_ns << 8) / (nsw_q8 ? nsw_q8 : 1U);
  uint64_t floor_w = (fps >= 240U) ? 2048U : 8192U;
  if (words < floor_w) words = floor_w;
  if (words > 0x07FFFFFEU) words = 0x07FFFFFEU;
  return (uint32_t)words & ~1U;
}

/* Folds one timing into a ns-per-word estimate. Small frames say little. */
static void zcvid_ec_note_time(uint32_t *nsw_q8, LARGE_INTEGER t0, LARGE_INTEGER t1,
                              uint32_t words) {
  if (words < 4096U) return;
  double ns = qpc_delta_us(t0, t1) * 1000.0;
  uint64_t sample = (uint64_t)(ns * 256.0 / (double)words);
  if (sample < 1U) sample = 1U;
  if (sample > (64U << 8)) sample = 64U << 8;
  *nsw_q8 = (uint32_t)(((uint64_t)*nsw_q8 * 7U + sample) / 8U);
}

/* Vectors as runs over the tile grid; returns 0 bytes when all are zero.
 * wide_mv = false: 3 bytes/run (run-1, int8 dx, int8 dy) for |mv| <= 127.
 * wide_mv = true : 5 bytes/run (run-1, int16_le dx, int16_le dy) for |mv| <= 255 qpel. */
static uint32_t zcvid_pack_mvs(const int32_t *mvs, uint32_t num_tiles, uint8_t *out, bool wide_mv) {
  bool any_nz = false;
  for (uint32_t i = 0; i < num_tiles && !any_nz; ++i) {
    any_nz = (mvs[i * 2U] | mvs[i * 2U + 1U]) != 0;
  }
  if (!any_nz) return 0;
  uint8_t *o = out;
  for (uint32_t i = 0; i < num_tiles;) {
    int32_t dx = mvs[i * 2U], dy = mvs[i * 2U + 1U];
    uint32_t run = 1;
    while (i + run < num_tiles && run < 256U && mvs[(i + run) * 2U] == dx &&
           mvs[(i + run) * 2U + 1U] == dy) {
      run++;
    }
    *o++ = (uint8_t)(run - 1U);
    if (wide_mv) {
      int16_t sdx = (int16_t)dx, sdy = (int16_t)dy;
      *o++ = (uint8_t)((uint16_t)sdx & 0xFFU);
      *o++ = (uint8_t)((uint16_t)sdx >> 8);
      *o++ = (uint8_t)((uint16_t)sdy & 0xFFU);
      *o++ = (uint8_t)((uint16_t)sdy >> 8);
    } else {
      *o++ = (uint8_t)(int8_t)dx;
      *o++ = (uint8_t)(int8_t)dy;
    }
    i += run;
  }
  return (uint32_t)(o - out);
}

static bool zcvid_unpack_mvs(const uint8_t *in, uint32_t in_len, int32_t *dst,
                            uint32_t num_tiles, bool wide_mv) {
  uint32_t step = wide_mv ? 5U : 3U;
  uint32_t t = 0;
  for (uint32_t i = 0; i + step <= in_len; i += step) {
    uint32_t run = (uint32_t)in[i] + 1U;
    if (t + run > num_tiles) return false;
    int32_t dx, dy;
    if (wide_mv) {
      dx = (int32_t)(int16_t)((uint16_t)in[i + 1U] | ((uint16_t)in[i + 2U] << 8));
      dy = (int32_t)(int16_t)((uint16_t)in[i + 3U] | ((uint16_t)in[i + 4U] << 8));
    } else {
      dx = (int32_t)(int8_t)in[i + 1U];
      dy = (int32_t)(int8_t)in[i + 2U];
    }
    for (uint32_t k = 0; k < run; ++k, ++t) {
      dst[t * 2U] = dx;
      dst[t * 2U + 1U] = dy;
    }
  }
  return t == num_tiles;
}

/* Clamped planar (MED) predictor for 2D motion vector fields:
 * exact zero residual on pan, zoom, rotation, and 2D affine motion,
 * while clamping cleanly at object boundaries without overshoot. */
static inline int32_t zcvid_mv_med(int32_t a, int32_t b, int32_t c) {
  int32_t mn = (a < b) ? a : b;
  int32_t mx = (a < b) ? b : a;
  if (c >= mx) return mn;
  if (c <= mn) return mx;
  return a + b - c;
}

#define ZCVID_MV_EC_MAG_CTX 8U
#define ZCVID_MV_EC_MAG_MAXLEN 9U /* covers |residual| up to 1023 */

typedef struct {
  zcrudp_rc_prob_t nz_tile[4];
  zcrudp_rc_prob_t nz_comp[2];
  zcrudp_rc_prob_t sign[2];
  zcrudp_rc_prob_t mag[2][ZCVID_MV_EC_MAG_CTX];
} zcvid_mv_ec_model_t;

static void zcvid_mv_ec_model_reset(zcvid_mv_ec_model_t *m) {
  static const uint16_t nz_priors[4] = {28000U, 16384U, 16384U, 10000U};
  for (uint32_t i = 0; i < 4U; ++i) zcrudp_rc_prob_init(&m->nz_tile[i], nz_priors[i]);
  for (uint32_t c = 0; c < 2U; ++c) {
    zcrudp_rc_prob_init(&m->nz_comp[c], 16384U);
    zcrudp_rc_prob_init(&m->sign[c], 16384U);
    for (uint32_t k = 0; k < ZCVID_MV_EC_MAG_CTX; ++k) {
      zcrudp_rc_prob_init(&m->mag[c][k], (k == 0U) ? 24000U : 16384U);
    }
  }
}

static inline void zcvid_ec_pack_mv_tile(zcrudp_rc_enc_t *rc, zcvid_mv_ec_model_t *m,
                                        int32_t rx, int32_t ry, uint32_t ctx, uint32_t *out_nz) {
  uint32_t nz = (rx != 0 || ry != 0) ? 1U : 0U;
  zcrudp_rc_enc_bit(rc, &m->nz_tile[ctx], nz);
  if (nz) {
    uint32_t nzx = (rx != 0) ? 1U : 0U;
    uint32_t nzy = (ry != 0) ? 1U : 0U;
    zcrudp_rc_enc_bit(rc, &m->nz_comp[0], nzx);
    if (nzx) {
      zcrudp_rc_enc_bit(rc, &m->sign[0], (rx < 0) ? 1U : 0U);
      uint32_t mag = (uint32_t)((rx < 0) ? -rx : rx) - 1U;
      zcrudp_rc_enc_egk(rc, m->mag[0], ZCVID_MV_EC_MAG_CTX, ZCVID_MV_EC_MAG_MAXLEN, mag);
      zcrudp_rc_enc_bit(rc, &m->nz_comp[1], nzy);
    }
    if (nzy) {
      zcrudp_rc_enc_bit(rc, &m->sign[1], (ry < 0) ? 1U : 0U);
      uint32_t mag = (uint32_t)((ry < 0) ? -ry : ry) - 1U;
      zcrudp_rc_enc_egk(rc, m->mag[1], ZCVID_MV_EC_MAG_CTX, ZCVID_MV_EC_MAG_MAXLEN, mag);
    }
  }
  *out_nz = nz;
}

/* Median-predicted DPCM + adaptive binary range coder for tile motion vectors.
 * Layout: [flags:1 (bit0=qpel)][range-coded stream]. Returns 0 on overflow. */
static uint32_t zcvid_ec_pack_mvs(const int32_t *mvs, uint32_t tiles_x, uint32_t tiles_y,
                                 bool qpel, uint8_t *out, size_t out_cap) {
  if (out_cap < 2U || tiles_x == 0U || tiles_y == 0U) return 0U;
  out[0] = qpel ? 1U : 0U;
  zcrudp_rc_enc_t rc;
  zcrudp_rc_enc_init(&rc, out + 1U, out_cap - 1U);
  zcvid_mv_ec_model_t m;
  zcvid_mv_ec_model_reset(&m);
  uint8_t row_nz[512];
  memset(row_nz, 0, (tiles_x < 512U) ? tiles_x : 512U);

  /* Row 0: tx=0 predicts (0,0), tx>0 predicts left. */
  {
    const int32_t *row = mvs;
    uint32_t left_nz = 0U;
    int32_t ax = 0, ay = 0;
    for (uint32_t tx = 0; tx < tiles_x; ++tx) {
      int32_t dx = row[tx * 2U + 0U];
      int32_t dy = row[tx * 2U + 1U];
      uint32_t nz = 0U;
      zcvid_ec_pack_mv_tile(&rc, &m, dx - ax, dy - ay, left_nz, &nz);
      ax = dx;
      ay = dy;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
      if (rc.overflow) return 0U;
    }
  }
  /* Rows 1..tiles_y-1: tx=0 predicts up, tx>0 uses register-cached MED(left, up, up-left). */
  for (uint32_t ty = 1U; ty < tiles_y; ++ty) {
    const int32_t *row = mvs + (size_t)ty * tiles_x * 2U;
    const int32_t *up = row - (size_t)tiles_x * 2U;
    int32_t cx = up[0], cy = up[1];
    int32_t ax = row[0], ay = row[1];
    uint32_t left_nz = 0U;
    zcvid_ec_pack_mv_tile(&rc, &m, ax - cx, ay - cy, (uint32_t)row_nz[0] << 1, &left_nz);
    row_nz[0] = (uint8_t)left_nz;
    if (rc.overflow) return 0U;
    for (uint32_t tx = 1U; tx < tiles_x; ++tx) {
      int32_t bx = up[tx * 2U + 0U];
      int32_t by = up[tx * 2U + 1U];
      int32_t dx = row[tx * 2U + 0U];
      int32_t dy = row[tx * 2U + 1U];
      int32_t px = zcvid_mv_med(ax, bx, cx);
      int32_t py = zcvid_mv_med(ay, by, cy);
      uint32_t nz = 0U;
      uint32_t ctx = left_nz | ((uint32_t)row_nz[tx & 511U] << 1);
      zcvid_ec_pack_mv_tile(&rc, &m, dx - px, dy - py, ctx, &nz);
      ax = dx; ay = dy;
      cx = bx; cy = by;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
      if (rc.overflow) return 0U;
    }
  }
  size_t len = zcrudp_rc_enc_finish(&rc);
  if (rc.overflow) return 0U;
  return (uint32_t)(1U + len);
}

static inline uint32_t zcvid_ec_unpack_mv_tile(zcrudp_rc_dec_t *rc, zcvid_mv_ec_model_t *m,
                                              uint32_t ctx, int32_t *out_rx, int32_t *out_ry) {
  uint32_t nz = zcrudp_rc_dec_bit(rc, &m->nz_tile[ctx]);
  if (!nz) {
    *out_rx = 0;
    *out_ry = 0;
    return 0U;
  }
  int32_t rx = 0, ry = 0;
  uint32_t nzx = zcrudp_rc_dec_bit(rc, &m->nz_comp[0]);
  uint32_t nzy = 1U;
  if (nzx) {
    uint32_t neg = zcrudp_rc_dec_bit(rc, &m->sign[0]);
    int32_t mag = (int32_t)zcrudp_rc_dec_egk(rc, m->mag[0], ZCVID_MV_EC_MAG_CTX,
                                             ZCVID_MV_EC_MAG_MAXLEN) + 1;
    rx = neg ? -mag : mag;
    nzy = zcrudp_rc_dec_bit(rc, &m->nz_comp[1]);
  }
  if (nzy) {
    uint32_t neg = zcrudp_rc_dec_bit(rc, &m->sign[1]);
    int32_t mag = (int32_t)zcrudp_rc_dec_egk(rc, m->mag[1], ZCVID_MV_EC_MAG_CTX,
                                             ZCVID_MV_EC_MAG_MAXLEN) + 1;
    ry = neg ? -mag : mag;
  }
  *out_rx = rx;
  *out_ry = ry;
  return 1U;
}

static bool zcvid_ec_unpack_mvs(const uint8_t *in, uint32_t in_len, int32_t *dst,
                               uint32_t tiles_x, uint32_t tiles_y, bool *out_qpel) {
  if (in_len < 2U || tiles_x == 0U || tiles_y == 0U) return false;
  if ((in[0] & ~1U) != 0U) return false;
  *out_qpel = (in[0] & 1U) != 0U;
  zcrudp_rc_dec_t rc;
  zcrudp_rc_dec_init(&rc, in + 1U, (size_t)(in_len - 1U));
  zcvid_mv_ec_model_t m;
  zcvid_mv_ec_model_reset(&m);
  uint8_t row_nz[512];
  memset(row_nz, 0, (tiles_x < 512U) ? tiles_x : 512U);

  /* Row 0: tx=0 predicts (0,0), tx>0 predicts left. */
  {
    int32_t *row = dst;
    uint32_t left_nz = 0U;
    int32_t ax = 0, ay = 0;
    for (uint32_t tx = 0; tx < tiles_x; ++tx) {
      int32_t rx, ry;
      uint32_t nz = zcvid_ec_unpack_mv_tile(&rc, &m, left_nz, &rx, &ry);
      if (nz) {
        ax += rx;
        ay += ry;
        if (ax < -512 || ax > 512 || ay < -512 || ay > 512) return false;
      }
      row[tx * 2U + 0U] = ax;
      row[tx * 2U + 1U] = ay;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
    }
  }
  /* Rows 1..tiles_y-1: tx=0 predicts up, tx>0 uses register-cached MED(left, up, up-left). */
  for (uint32_t ty = 1U; ty < tiles_y; ++ty) {
    int32_t *row = dst + (size_t)ty * tiles_x * 2U;
    const int32_t *up = row - (size_t)tiles_x * 2U;
    int32_t cx = up[0], cy = up[1];
    int32_t rx, ry;
    uint32_t left_nz = zcvid_ec_unpack_mv_tile(&rc, &m, (uint32_t)row_nz[0] << 1, &rx, &ry);
    int32_t ax = cx + rx, ay = cy + ry;
    if (left_nz && (ax < -512 || ax > 512 || ay < -512 || ay > 512)) return false;
    row[0] = ax;
    row[1] = ay;
    row_nz[0] = (uint8_t)left_nz;
    for (uint32_t tx = 1U; tx < tiles_x; ++tx) {
      int32_t bx = up[tx * 2U + 0U];
      int32_t by = up[tx * 2U + 1U];
      int32_t px = zcvid_mv_med(ax, bx, cx);
      int32_t py = zcvid_mv_med(ay, by, cy);
      uint32_t ctx = left_nz | ((uint32_t)row_nz[tx & 511U] << 1);
      uint32_t nz = zcvid_ec_unpack_mv_tile(&rc, &m, ctx, &rx, &ry);
      int32_t dx = px + rx;
      int32_t dy = py + ry;
      if (nz && (dx < -512 || dx > 512 || dy < -512 || dy > 512)) return false;
      row[tx * 2U + 0U] = dx;
      row[tx * 2U + 1U] = dy;
      ax = dx; ay = dy;
      cx = bx; cy = by;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
    }
  }
  return true;
}

/* Tile motion search radius in pixels per frame (up to 14,880 px/s at 120 fps).
 * The shader searches +-60 px in groupshared and conditionally extends to +-124 px
 * when no clean inner match exists; vectors travel in [-512, +512] qpel natively. */
#ifndef ZCVID_SEARCH_R
#define ZCVID_SEARCH_R 124U
#endif

/* ---------------------------------------------------------------------------
 * Compiled shader cache. D3DCompile takes seconds for these shaders; its
 * output only depends on the source text, the entry point, the profile and
 * the flags, so the bytecode is kept on disk under a hash of all four:
 * %LOCALAPPDATA%\zcrudp\shaders\<entry>-<hash>.dxbc (or the temp directory).
 * A changed shader gets a new hash, hence a fresh compile; stale files are
 * simply never read again. Any cache failure falls back to compiling.
 * ------------------------------------------------------------------------- */
#define ZCVID_HLSL_FLAGS D3DCOMPILE_OPTIMIZATION_LEVEL3
#define ZCVID_HLSL_PROFILE "cs_5_0"

static uint64_t zcvid_fnv1a(uint64_t h, const void *data, size_t len) {
  const uint8_t *d = (const uint8_t *)data;
  for (size_t i = 0; i < len; ++i) {
    h ^= d[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static bool zcvid_shader_cache_path(const char *entry, uint64_t hash, char *out, size_t cap) {
  char base[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    n = GetTempPathA(MAX_PATH, base);
    if (n == 0 || n >= MAX_PATH) return false;
  }
  char dir[MAX_PATH];
  snprintf(dir, sizeof(dir), "%s\\zcrudp", base);
  CreateDirectoryA(dir, NULL);
  snprintf(dir, sizeof(dir), "%s\\zcrudp\\shaders", base);
  CreateDirectoryA(dir, NULL);
  int w = snprintf(out, cap, "%s\\%s-%016llx.dxbc", dir, entry, (unsigned long long)hash);
  return w > 0 && (size_t)w < cap;
}

static uint8_t *zcvid_read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint8_t *buf = NULL;
  if (fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
    if (sz > 0 && fseek(f, 0, SEEK_SET) == 0) {
      buf = (uint8_t *)malloc((size_t)sz);
      if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        buf = NULL;
      } else if (buf) {
        *len = (size_t)sz;
      }
    }
  }
  fclose(f);
  return buf;
}

static int compile_hlsl_cs(ID3D11Device *dev, const char *filename,
                           const char *entry, ID3D11ComputeShader **out_cs) {
  char exe_shaders[MAX_PATH] = {0};
  DWORD exe_len = GetModuleFileNameA(NULL, exe_shaders, (DWORD)sizeof(exe_shaders));
  if (exe_len > 0 && exe_len < (DWORD)sizeof(exe_shaders)) {
    char *sep = strrchr(exe_shaders, '\\');
    char *sep2 = strrchr(exe_shaders, '/');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
    if (sep) {
      sep[1] = '\0';
      if (strlen(exe_shaders) + sizeof("shaders/") < sizeof(exe_shaders)) {
        strcat(exe_shaders, "shaders/");
      } else {
        exe_shaders[0] = '\0';
      }
    } else {
      exe_shaders[0] = '\0';
    }
  } else {
    exe_shaders[0] = '\0';
  }

  char env_shaders[MAX_PATH] = {0};
  const char *env_dir = getenv("ZCVID_SHADERS_DIR");
  if (env_dir && env_dir[0] != '\0') {
    size_t n = strlen(env_dir);
    if (n + 2 < sizeof(env_shaders)) {
      memcpy(env_shaders, env_dir, n);
      if (env_dir[n - 1] != '/' && env_dir[n - 1] != '\\') {
        env_shaders[n++] = '/';
      }
      env_shaders[n] = '\0';
    }
  }

  const char *prefixes[9] = {
    env_shaders[0] ? env_shaders : NULL,
    exe_shaders[0] ? exe_shaders : NULL,
    "shaders/",
#ifdef ZCVID_SHADERS_DIR
    ZCVID_SHADERS_DIR "/",
#else
    NULL,
#endif
    "_deps/zcvid-src/shaders/",
    "../_deps/zcvid-src/shaders/",
    "../zcvid/shaders/",
    "../zcvid-main/shaders/",
    "../../zcvid/shaders/"
  };
  for (int i = 0; i < 9; ++i) {
    if (!prefixes[i]) continue;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", prefixes[i], filename);
    size_t src_len = 0;
    uint8_t *src = zcvid_read_file(path, &src_len);
    if (!src) continue;

    uint64_t h = 14695981039346656037ULL;
    h = zcvid_fnv1a(h, src, src_len);
    h = zcvid_fnv1a(h, entry, strlen(entry) + 1U);
    h = zcvid_fnv1a(h, ZCVID_HLSL_PROFILE, sizeof(ZCVID_HLSL_PROFILE));
    uint32_t flags = (uint32_t)ZCVID_HLSL_FLAGS;
    h = zcvid_fnv1a(h, &flags, sizeof(flags));

    char cache[MAX_PATH];
    bool have_cache_path = zcvid_shader_cache_path(entry, h, cache, sizeof(cache));
    if (have_cache_path) {
      size_t bc_len = 0;
      uint8_t *bc = zcvid_read_file(cache, &bc_len);
      if (bc) {
        HRESULT hr = ID3D11Device_CreateComputeShader(dev, bc, bc_len, NULL, out_cs);
        free(bc);
        if (SUCCEEDED(hr)) {
          free(src);
          return 0;
        }
        /* unreadable or foreign bytecode: compile again below */
      }
    }

    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, src_len, path, NULL, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                            entry, ZCVID_HLSL_PROFILE, ZCVID_HLSL_FLAGS, 0, &code, &err);
    free(src);
    if (FAILED(hr) || !code) {
      if (err) {
        fprintf(stderr, "HLSL compile error (%s:%s): %s\n", path, entry,
                (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
      }
      if (code) ID3D10Blob_Release(code);
      continue;
    }
    if (err) ID3D10Blob_Release(err);
    const void *bc = ID3D10Blob_GetBufferPointer(code);
    size_t bc_len = ID3D10Blob_GetBufferSize(code);
    if (have_cache_path) {
      /* Written under a temporary name then renamed, so a reader never sees
       * half a file. */
      char tmp[MAX_PATH];
      snprintf(tmp, sizeof(tmp), "%s.%lu.tmp", cache, (unsigned long)GetCurrentProcessId());
      FILE *f = fopen(tmp, "wb");
      if (f) {
        bool ok = fwrite(bc, 1, bc_len, f) == bc_len;
        ok = (fclose(f) == 0) && ok;
        if (!ok || !MoveFileExA(tmp, cache, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
      }
    }
    hr = ID3D11Device_CreateComputeShader(dev, bc, bc_len, NULL, out_cs);
    ID3D10Blob_Release(code);
    return SUCCEEDED(hr) ? 0 : -2;
  }
  return -1;
}

static int create_yuvd_pingpong(ID3D11Device *dev, size_t num_px,
                                uint32_t neutral_y, uint32_t delta_min,
                                ID3D11Buffer *bufs[2],
                                ID3D11ShaderResourceView *srvs[2],
                                ID3D11UnorderedAccessView *uavs[2]) {
  uint32_t init_w = (neutral_y & 0xFFU) | (128U << 8) | (128U << 16) | ((delta_min & 0xFFU) << 24);
  uint32_t *init_data = (uint32_t *)malloc(num_px * sizeof(uint32_t));
  if (!init_data) return -1;
  for (size_t i = 0; i < num_px; ++i) init_data[i] = init_w;

  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = (UINT)(num_px * sizeof(uint32_t));
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);

  D3D11_SUBRESOURCE_DATA sd = {0};
  sd.pSysMem = init_data;

  D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {0};
  srvd.Format = DXGI_FORMAT_UNKNOWN;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srvd.Buffer.NumElements = (UINT)num_px;

  D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {0};
  uavd.Format = DXGI_FORMAT_UNKNOWN;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = (UINT)num_px;

  int rc = 0;
  for (int i = 0; i < 2; ++i) {
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, &sd, &bufs[i])) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)bufs[i], &srvd, &srvs[i])) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)bufs[i], &uavd, &uavs[i]))) {
      rc = -1;
      break;
    }
  }
  free(init_data);
  return rc;
}

/* Zero-initialised structured buffer of uints with a UAV (and an SRV if asked). */
static int zcvid_create_uint_buffer(ID3D11Device *dev, uint32_t count, ID3D11Buffer **buf,
                                   ID3D11UnorderedAccessView **uav,
                                   ID3D11ShaderResourceView **srv) {
  uint32_t *zeros = (uint32_t *)calloc(count, sizeof(uint32_t));
  if (!zeros) return -1;
  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = count * (UINT)sizeof(uint32_t);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0U);
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);
  D3D11_SUBRESOURCE_DATA init = {0};
  init.pSysMem = zeros;
  HRESULT hr = ID3D11Device_CreateBuffer(dev, &bd, &init, buf);
  free(zeros);
  if (FAILED(hr)) return -1;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {0};
  ud.Format = DXGI_FORMAT_UNKNOWN;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = count;
  if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)*buf, &ud, uav))) {
    return -1;
  }
  if (srv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = count;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)*buf, &sd, srv))) {
      return -1;
    }
  }
  return 0;
}

static int setup_zcvid_hlsl(gpu_pipeline_t *p, gpu_pipeline_info_t *info) {
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSLumaHalf", &p->zcvid_cs_luma_half) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSLumaQuarter", &p->zcvid_cs_luma_quarter) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSTileMV", &p->zcvid_cs_tile_mv) != 0)
    return -19;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSMvSmooth", &p->zcvid_cs_mv_smooth) != 0)
    return -19;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSMotionPred", &p->zcvid_cs_motion_pred) != 0)
    return -19;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSEncode", &p->zcvid_cs_encode) != 0)
    return -20;
  if (compile_hlsl_cs(p->dev, "zcvid_decode.hlsl", "CSMotionDecay", &p->zcvid_cs_motion_decay) != 0)
    return -21;
  if (compile_hlsl_cs(p->dev, "zcvid_decode.hlsl", "CSApply", &p->zcvid_cs_apply) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "zcvid_decode.hlsl", "CSYCbCrToRGBA", &p->zcvid_cs_ycbcr_to_rgba) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSIntraT", &p->zcvid_cs_intra_t) != 0)
    return -20;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSIntraTCount", &p->zcvid_cs_intra_t_count) != 0)
    return -20;
  if (compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSTqScanA", &p->zcvid_cs_tq_scan_a) != 0 ||
      compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSTqMid", &p->zcvid_cs_tq_mid) != 0 ||
      compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSTqClear", &p->zcvid_cs_tq_clear) != 0 ||
      compile_hlsl_cs(p->dev, "zcvid_encode.hlsl", "CSTqFinal", &p->zcvid_cs_tq_final) != 0)
    return -20;
  if (compile_hlsl_cs(p->dev, "zcvid_decode.hlsl", "CSITrans", &p->zcvid_cs_itrans) != 0)
    return -22;

  HRESULT hr = ID3D11Device_CreateShaderResourceView(
      p->dev, (ID3D11Resource *)p->cap_bgra_tex, NULL, &p->cap_bgra_srv);
  if (FAILED(hr)) return -23;

  {
    UINT zero4[4] = {0, 0, 0, 0};
    float zero_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    D3D11_TEXTURE2D_DESC ptd;
    ID3D11Texture2D_GetDesc(p->cap_bgra_tex, &ptd);
    ptd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(p->dev, &ptd, NULL, &p->zcvid_prev_src_tex);
    if (FAILED(hr)) return -23;
    hr = ID3D11Device_CreateRenderTargetView(
        p->dev, (ID3D11Resource *)p->zcvid_prev_src_tex, NULL, &p->zcvid_prev_src_rtv);
    if (FAILED(hr)) return -23;
    hr = ID3D11Device_CreateShaderResourceView(
        p->dev, (ID3D11Resource *)p->zcvid_prev_src_tex, NULL, &p->zcvid_prev_src_srv);
    if (FAILED(hr)) return -23;
    ID3D11DeviceContext_ClearRenderTargetView(p->ctx, p->zcvid_prev_src_rtv, zero_rgba);

    D3D11_TEXTURE2D_DESC htd = {0};
    htd.Width = (p->width + 1U) / 2U;
    htd.Height = (p->height + 1U) / 2U;
    htd.MipLevels = 1;
    htd.ArraySize = 1;
    htd.Format = DXGI_FORMAT_R8_UINT;
    htd.SampleDesc.Count = 1;
    htd.Usage = D3D11_USAGE_DEFAULT;
    htd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (int i = 0; i < 2; ++i) {
      hr = ID3D11Device_CreateTexture2D(p->dev, &htd, NULL, &p->zcvid_half_tex[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateShaderResourceView(
          p->dev, (ID3D11Resource *)p->zcvid_half_tex[i], NULL, &p->zcvid_half_srv[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateUnorderedAccessView(
          p->dev, (ID3D11Resource *)p->zcvid_half_tex[i], NULL, &p->zcvid_half_uav[i]);
      if (FAILED(hr)) return -23;
      ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->zcvid_half_uav[i], zero4);
    }
    htd.Width = (p->width + 3U) / 4U;
    htd.Height = (p->height + 3U) / 4U;
    for (int i = 0; i < 2; ++i) {
      hr = ID3D11Device_CreateTexture2D(p->dev, &htd, NULL, &p->zcvid_quarter_tex[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateShaderResourceView(
          p->dev, (ID3D11Resource *)p->zcvid_quarter_tex[i], NULL, &p->zcvid_quarter_srv[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateUnorderedAccessView(
          p->dev, (ID3D11Resource *)p->zcvid_quarter_tex[i], NULL, &p->zcvid_quarter_uav[i]);
      if (FAILED(hr)) return -23;
      ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->zcvid_quarter_uav[i], zero4);
    }
  }

  size_t num_px = (size_t)p->width * (size_t)p->height;
  p->zcvid_total_blocks = (uint32_t)((num_px + 9U) / 10U);
  p->zcvid_tiles_x = (p->width + 31U) / 32U;
  p->zcvid_tiles_y = (p->height + 31U) / 32U;
  uint32_t num_tiles = p->zcvid_tiles_x * p->zcvid_tiles_y;
  p->zcvid_max_record_words = (uint32_t)(2U * num_px + (size_t)p->zcvid_total_blocks * 6U);

  if (create_yuvd_pingpong(p->dev, num_px, p->zcvid_neutral_y, p->zcvid_delta_min,
                           p->zcvid_enc_yuvd_buf, p->zcvid_enc_yuvd_srv,
                           p->zcvid_enc_yuvd_uav) != 0) {
    return -24;
  }
  if (create_yuvd_pingpong(p->dev, num_px, p->zcvid_neutral_y, p->zcvid_delta_min,
                           p->zcvid_dec_yuvd_buf, p->zcvid_dec_yuvd_srv,
                           p->zcvid_dec_yuvd_uav) != 0) {
    return -24;
  }
  {
    /* Capture hints for CSTileMV: one int4 per tile, all "no hint" to start. */
    uint32_t nt = ((p->width + 31U) / 32U) * ((p->height + 31U) / 32U);
    p->zcvid_hint_cpu = (int32_t *)calloc((size_t)nt * 4U, sizeof(int32_t));
    D3D11_BUFFER_DESC hbd = {0};
    hbd.ByteWidth = nt * 16U;
    hbd.Usage = D3D11_USAGE_DEFAULT;
    hbd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    hbd.StructureByteStride = 16U;
    D3D11_SUBRESOURCE_DATA hinit = {0};
    hinit.pSysMem = p->zcvid_hint_cpu;
    if (!p->zcvid_hint_cpu ||
        FAILED(ID3D11Device_CreateBuffer(p->dev, &hbd, &hinit, &p->zcvid_hint_buf))) {
      return -24;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC hsd = {0};
    hsd.Format = DXGI_FORMAT_UNKNOWN;
    hsd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    hsd.Buffer.NumElements = nt;
    if (FAILED(ID3D11Device_CreateShaderResourceView(p->dev, (ID3D11Resource *)p->zcvid_hint_buf,
                                                     &hsd, &p->zcvid_hint_srv))) {
      return -24;
    }
  }
  p->zcvid_tq_blocks = ((p->width + 3U) / 4U) * ((p->height + 3U) / 4U);
  if (zcvid_create_uint_buffer(p->dev, (uint32_t)num_px, &p->zcvid_enc_pred_buf,
                              &p->zcvid_enc_pred_uav, &p->zcvid_enc_pred_srv) != 0 ||
      zcvid_create_uint_buffer(p->dev, p->zcvid_tq_blocks, &p->zcvid_enc_tflag_buf,
                              &p->zcvid_enc_tflag_uav, &p->zcvid_enc_tflag_srv) != 0 ||
      /* 2 words per position (c, stage_slot), 4 per group of 64 (hi/eq sums+bases), 16 hist + 8 misc */
      zcvid_create_uint_buffer(p->dev, ((p->zcvid_tq_blocks + 63U) & ~63U) * 2U +
                                          ((p->zcvid_tq_blocks + 63U) / 64U) * 4U + 24U,
                              &p->zcvid_tq_scratch_buf,
                              &p->zcvid_tq_scratch_uav, NULL) != 0 ||
      zcvid_create_uint_buffer(p->dev, p->zcvid_max_record_words, &p->zcvid_tq_stage_buf,
                              &p->zcvid_tq_stage_uav, NULL) != 0 ||
      zcvid_create_uint_buffer(p->dev, p->zcvid_tq_blocks * 48U, &p->zcvid_dec_coef_buf,
                              &p->zcvid_dec_coef_uav, NULL) != 0 ||
      zcvid_create_uint_buffer(p->dev, p->zcvid_tq_blocks, &p->zcvid_dec_tflag_buf,
                              &p->zcvid_dec_tflag_uav, NULL) != 0) {
    return -24;
  }

  /* Tile MV ping-pong buffers (enc SRV+UAV + triple-buffered staging, dec DYNAMIC SRV) */
  UINT zero4[4] = {0, 0, 0, 0};
  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = 8U;

  D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {0};
  srvd.Format = DXGI_FORMAT_UNKNOWN;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srvd.Buffer.NumElements = num_tiles;

  D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {0};
  uavd.Format = DXGI_FORMAT_UNKNOWN;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = num_tiles;

  for (int i = 0; i < 2; ++i) {
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_enc_mvs_buf[i]);
    if (FAILED(hr)) return -24;
    hr = ID3D11Device_CreateShaderResourceView(
        p->dev, (ID3D11Resource *)p->zcvid_enc_mvs_buf[i], &srvd, &p->zcvid_enc_mvs_srv[i]);
    if (FAILED(hr)) return -24;
    hr = ID3D11Device_CreateUnorderedAccessView(
        p->dev, (ID3D11Resource *)p->zcvid_enc_mvs_buf[i], &uavd, &p->zcvid_enc_mvs_uav[i]);
    if (FAILED(hr)) return -24;
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->zcvid_enc_mvs_uav[i], zero4);
  }

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (uint32_t s = 0; s < ZCVID_STG_RING; ++s) {
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_enc_mvs_stg[s]);
    if (FAILED(hr)) return -24;
  }

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = 8U;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_dec_mvs_buf);
  if (FAILED(hr)) return -24;

  memset(&srvd, 0, sizeof(srvd));
  srvd.Format = DXGI_FORMAT_UNKNOWN;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srvd.Buffer.NumElements = num_tiles;
  hr = ID3D11Device_CreateShaderResourceView(
      p->dev, (ID3D11Resource *)p->zcvid_dec_mvs_buf, &srvd, &p->zcvid_dec_mvs_srv);
  if (FAILED(hr)) return -24;

  /* Output active records buffer + triple-buffered staging (worst-case Intra + Delta) */
  UINT rec_bytes = p->zcvid_max_record_words * (UINT)sizeof(uint32_t);
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = rec_bytes;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_out_records_buf);
  if (FAILED(hr)) return -25;

  memset(&uavd, 0, sizeof(uavd));
  uavd.Format = DXGI_FORMAT_UNKNOWN;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = p->zcvid_max_record_words;
  hr = ID3D11Device_CreateUnorderedAccessView(
      p->dev, (ID3D11Resource *)p->zcvid_out_records_buf, &uavd, &p->zcvid_out_records_uav);
  if (FAILED(hr)) return -25;

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = rec_bytes;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (uint32_t s = 0; s < ZCVID_STG_RING; ++s) {
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_out_records_stg[s]);
    if (FAILED(hr)) return -25;
  }

  /* Record counter: [0] words written (the slot allocator), [1] transform
   * demand (only the rate control reads it), [2] unmatched scene-cut tile
   * count from CSTileMV, [3] reserved, plus triple-buffered staging + event queries */
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = 16U;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_out_counter_buf);
  if (FAILED(hr)) return -26;

  memset(&uavd, 0, sizeof(uavd));
  uavd.Format = DXGI_FORMAT_R32_TYPELESS;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = 4U;
  uavd.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  hr = ID3D11Device_CreateUnorderedAccessView(
      p->dev, (ID3D11Resource *)p->zcvid_out_counter_buf, &uavd, &p->zcvid_out_counter_uav);
  if (FAILED(hr)) return -26;

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = 16U;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (uint32_t s = 0; s < ZCVID_STG_RING; ++s) {
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_out_counter_stg[s]);
    if (FAILED(hr)) return -26;
  }
  if (getenv("ZCVID_PROFILE")) {
    D3D11_QUERY_DESC dq = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC tq = {D3D11_QUERY_TIMESTAMP, 0};
    if (SUCCEEDED(ID3D11Device_CreateQuery(p->dev, &dq, &p->zcvid_prof_disj))) {
      bool ok = true;
      for (int i = 0; i < 8; ++i) {
        if (FAILED(ID3D11Device_CreateQuery(p->dev, &tq, &p->zcvid_prof_ts[i]))) ok = false;
      }
      p->zcvid_prof_en = ok;
    }
  }

  /* Decoder input records (DYNAMIC) + SRV */
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = rec_bytes;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_in_records_buf);
  if (FAILED(hr)) return -27;

  memset(&srvd, 0, sizeof(srvd));
  srvd.Format = DXGI_FORMAT_UNKNOWN;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srvd.Buffer.NumElements = p->zcvid_max_record_words;
  hr = ID3D11Device_CreateShaderResourceView(
      p->dev, (ID3D11Resource *)p->zcvid_in_records_buf, &srvd, &p->zcvid_in_records_srv);
  if (FAILED(hr)) return -27;

  /* Decoder output texture in VRAM */
  UINT fmt_sup = 0;
  ID3D11Device_CheckFormatSupport(p->dev, DXGI_FORMAT_B8G8R8A8_UNORM, &fmt_sup);
  DXGI_FORMAT dec_fmt = (fmt_sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
      ? DXGI_FORMAT_B8G8R8A8_UNORM
      : DXGI_FORMAT_R8G8B8A8_UNORM;
  p->zcvid_dec_use_copy = (dec_fmt == DXGI_FORMAT_B8G8R8A8_UNORM);

  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = p->width;
  td.Height = p->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = dec_fmt;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  hr = ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->zcvid_dec_tex);
  if (FAILED(hr)) return -28;

  hr = ID3D11Device_CreateUnorderedAccessView(
      p->dev, (ID3D11Resource *)p->zcvid_dec_tex, NULL, &p->zcvid_dec_uav);
  if (FAILED(hr)) return -28;

  /* Constant buffers */
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = sizeof(zcvid_enc_cb_t);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_enc_cb);
  if (FAILED(hr)) return -29;

  bd.ByteWidth = sizeof(zcvid_dec_cb_t);
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_dec_cb);
  if (FAILED(hr)) return -29;

  free(p->enc_out_buf);
  p->enc_out_cap = 8U + (size_t)num_tiles * 8U + (size_t)rec_bytes + (size_t)rec_bytes / 4U + 64U;
  p->enc_out_buf = (uint8_t *)malloc(p->enc_out_cap);
  if (!p->enc_out_buf) return -30;

  snprintf(info->encoder_name, sizeof(info->encoder_name),
           "Zcvid YCbCr+TileMV HLSL (cs_5_0)");
  snprintf(info->decoder_name, sizeof(info->decoder_name),
           "Zcvid YCbCr+TileMV HLSL (cs_5_0)");
  return 0;
}

/* Every host buffer the frame loop uses, at its working size and with its
 * pages touched, so that nothing is allocated or faulted in after frame 0
 * (a lazy realloc or first touch costs an OS call in the middle of a frame:
 * a p99 spike at 480 Hz). The bound is what a frame can realistically carry
 * (4 start bursts, at least 1M record words), not the absolute maximum
 * (tens of MB at 4K); beyond it the old growth paths still work and say so. */
static int zcvid_prealloc_host(gpu_pipeline_t *p) {
  uint32_t num_px = p->width * p->height;
  uint32_t burst_words = num_px / 14U;
  if (burst_words < 140000U) burst_words = 140000U;
  uint32_t words = burst_words * 4U;
  if (words < (1U << 20)) words = 1U << 20;
  if (words > p->zcvid_max_record_words) words = p->zcvid_max_record_words;
  uint32_t records = words / 2U + 1024U;

  p->zcvid_sort_a = (uint64_t *)malloc((size_t)records * sizeof(uint64_t));
  p->zcvid_sort_b = (uint64_t *)malloc((size_t)records * sizeof(uint64_t));
  if (!p->zcvid_sort_a || !p->zcvid_sort_b) return -1;
  memset(p->zcvid_sort_a, 0, (size_t)records * sizeof(uint64_t));
  memset(p->zcvid_sort_b, 0, (size_t)records * sizeof(uint64_t));
  p->zcvid_sort_cap = records;

  for (uint32_t c = 0; c < ZCVID_EC_MAX_CHUNKS; ++c) {
    if (!zcvid_ec_alloc(p, c)) return -1;
    memset(p->zcvid_ec_line[c], 0, (size_t)p->width * 3U);
    memset(p->zcvid_ec_lrow[c], 0, (size_t)p->width * sizeof(uint32_t));
    memset(p->zcvid_ec_model[c], 0, sizeof(zcrudp_tec_model_t));
  }
  size_t ec = zcvid_ec_chunk_bound(records) + ZCVID_EC_MAX_CHUNKS * zcvid_ec_chunk_bound(0U);
  ec += ec / 2U;
  p->zcvid_ec_scratch = (uint8_t *)malloc(ec);
  if (!p->zcvid_ec_scratch) return -1;
  memset(p->zcvid_ec_scratch, 0, ec);
  p->zcvid_ec_scratch_cap = ec;

  /* move + dirty rectangles: 64 KB is some 2700 of them */
  p->dupl_meta_cap = 64U * 1024U;
  p->dupl_meta = (uint8_t *)calloc(p->dupl_meta_cap, 1U);
  if (!p->dupl_meta) return -1;

  uint32_t num_tiles = p->zcvid_tiles_x * p->zcvid_tiles_y;
  p->zcvid_dec_mv_scratch = (int32_t *)calloc((size_t)num_tiles * 2U, sizeof(int32_t));
  if (!p->zcvid_dec_mv_scratch) return -1;
  p->zcvid_dec_mv_cap = num_tiles;
  p->zcvid_dec_unpack_buf = (uint32_t *)malloc((size_t)words * sizeof(uint32_t));
  if (!p->zcvid_dec_unpack_buf) return -1;
  memset(p->zcvid_dec_unpack_buf, 0, (size_t)words * sizeof(uint32_t));
  p->zcvid_dec_unpack_cap = words;
  return 0;
}

int gpu_pipeline_create_zcvid(gpu_pipeline_t **out_pipe, uint32_t width,
                                uint32_t height, uint32_t fps,
                                uint32_t threshold, uint32_t delta_step,
                                uint32_t gamma_q8, uint32_t neutral_y,
                                bool show_window, gpu_pipeline_info_t *out_info) {
  if (!out_pipe || !out_info || width == 0U || height == 0U || (width & 1U) != 0U) return -1;
  gpu_pipeline_t *p = (gpu_pipeline_t *)calloc(1, sizeof(*p));
  if (!p) return -1;

  p->width = width;
  p->height = height;
  p->fps = fps;
  p->bitrate_bps = 50000000U;
  p->use_zcvid = true;
  p->zcvid_threshold = threshold;
  p->zcvid_delta_min = delta_step;
  p->zcvid_delta_max = 32U;
  p->zcvid_gamma_q8 = gamma_q8;
  p->zcvid_neutral_y = neutral_y;
  p->zcvid_rc_threshold = threshold;
  p->zcvid_rc_intra_err_thr = 32U;
  p->zcvid_rc_2x = false;
  p->zcvid_intra8 = true;
  /* Unified codec enabled out of the box: compact entropy-coded bitstream,
   * wide-range quarter-pel motion compensation, and 2-tier priority hybrid
   * 4x4 transform + zcvid deltas. Adapts automatically to 4K cinema and
   * 1080p 480Hz / 2K 240Hz gaming without requiring manual feature toggles. */
  p->zcvid_packed = true;
  p->zcvid_ec = true;
  p->zcvid_qpel = true;
  p->zcvid_tq = true;
  p->zcvid_tq_inter = true;
  p->zcvid_dither = true;
  p->zcvid_pack_ratio_q16 = 32768U;
  p->zcvid_ec_ratio_q16 = 16384U;
  p->zcvid_tq_qp = ZCVID_TQ_QP_START;
  p->zcvid_hint_gpu_state = -1;
  p->zcvid_tq_inter_thr = 0U;
  p->zcvid_dec_post_qp = 0U;
  p->zcvid_dma_spec_words = 32768U;
  {
    /* Every logical processor but one. What the coder may take is bounded
     * in time, not in threads: see zcvid_ec_budget_words. */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint32_t t = (si.dwNumberOfProcessors > 2U) ? (uint32_t)si.dwNumberOfProcessors - 2U : 1U;
    if (t > 22U) t = 22U;
    if (t > ZCVID_EC_MAX_CHUNKS) t = ZCVID_EC_MAX_CHUNKS;
    p->zcvid_ec_threads = t;
    /* First guess, replaced by measurements: 16 ns per word on one thread. */
    p->zcvid_ec_enc_nsw_q8 = (16U << 8) / t;
    p->zcvid_ec_dec_nsw_q8 = (16U << 8) / t;
    if (t > 1U) {
      /* Pre-spawn t-1 worker threads (plus caller = t active threads, leaving
       * CPU headroom for D3D11/WDDM and the foreground game/player). */
      p->zcvid_pool = zcvid_pool_create(t - 1U);
    }
  }

  gpu_pipeline_info_t info;
  memset(&info, 0, sizeof(info));
  info.width = width;
  info.height = height;

  int rc = create_d3d11_and_swapchain(p, show_window, &info);
  if (rc != 0) {
    gpu_pipeline_destroy(p);
    return rc;
  }
  rc = setup_zcvid_hlsl(p, &info);
  if (rc != 0) {
    gpu_pipeline_destroy(p);
    return rc;
  }
  if (zcvid_prealloc_host(p) != 0) {
    gpu_pipeline_destroy(p);
    return -31;
  }
  *out_pipe = p;
  if (out_info) *out_info = info;
  return 0;
}

#ifdef ZC_HAVE_PYROWAVE
static const char k_pyro_yuv2rgba_hlsl[] =
    "cbuffer PyroCB : register(b0) { uint width; uint height; uint pad0; uint pad1; };\n"
    "Texture2D<float> g_y  : register(t0);\n"
    "Texture2D<float> g_cb : register(t1);\n"
    "Texture2D<float> g_cr : register(t2);\n"
    "RWTexture2D<unorm float4> g_out : register(u0);\n"
    "[numthreads(16, 16, 1)]\n"
    "void CSPyroYUVToRGBA(uint3 tid : SV_DispatchThreadID) {\n"
    "  if (tid.x >= width || tid.y >= height) return;\n"
    "  float y = g_y.Load(int3(tid.xy, 0));\n"
    "  float2 cf = ((float2)tid.xy + 0.5) * 0.5 - 0.5;\n"
    "  int2 c0 = int2(floor(cf));\n"
    "  float2 f = cf - (float2)c0;\n"
    "  int2 cmax = int2((int)(width >> 1) - 1, (int)(height >> 1) - 1);\n"
    "  int2 p00 = clamp(c0, int2(0, 0), cmax);\n"
    "  int2 p10 = clamp(c0 + int2(1, 0), int2(0, 0), cmax);\n"
    "  int2 p01 = clamp(c0 + int2(0, 1), int2(0, 0), cmax);\n"
    "  int2 p11 = clamp(c0 + int2(1, 1), int2(0, 0), cmax);\n"
    "  float cb = lerp(lerp(g_cb.Load(int3(p00, 0)), g_cb.Load(int3(p10, 0)), f.x),\n"
    "                  lerp(g_cb.Load(int3(p01, 0)), g_cb.Load(int3(p11, 0)), f.x), f.y) - (128.0 / 255.0);\n"
    "  float cr = lerp(lerp(g_cr.Load(int3(p00, 0)), g_cr.Load(int3(p10, 0)), f.x),\n"
    "                  lerp(g_cr.Load(int3(p01, 0)), g_cr.Load(int3(p11, 0)), f.x), f.y) - (128.0 / 255.0);\n"
    "  float r = saturate(y + 1.5748 * cr);\n"
    "  float g = saturate(y - 0.187324 * cb - 0.468124 * cr);\n"
    "  float b = saturate(y + 1.8556 * cb);\n"
    "  g_out[tid.xy] = float4(r, g, b, 1.0);\n"
    "}\n";

static int setup_pyrowave_d3d11_fence(gpu_pipeline_t *p) {
  if (p->pyro_fence_handle) return 0;
  if (FAILED(ID3D11Device_QueryInterface(p->dev, &IID_ID3D11Device5, (void **)&p->pyro_dev5)) ||
      FAILED(ID3D11DeviceContext_QueryInterface(p->ctx, &IID_ID3D11DeviceContext4, (void **)&p->pyro_ctx4))) {
    return -4;
  }
  if (FAILED(ID3D11Device5_CreateFence(p->pyro_dev5, 0, D3D11_FENCE_FLAG_SHARED,
                                       &IID_ID3D11Fence, (void **)&p->pyro_fence))) {
    return -5;
  }
  if (FAILED(ID3D11Fence_CreateSharedHandle(p->pyro_fence, NULL, GENERIC_ALL, NULL,
                                            &p->pyro_fence_handle)) || !p->pyro_fence_handle) {
    return -6;
  }
  return 0;
}

static int setup_pyrowave_d3d11_interop(gpu_pipeline_t *p) {
  if (setup_pyrowave_d3d11_fence(p) != 0) return -4;

  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = p->width;
  td.Height = p->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->pyro_shared_bgra))) {
    return -1;
  }

  IDXGIResource1 *res1 = NULL;
  if (FAILED(ID3D11Texture2D_QueryInterface(p->pyro_shared_bgra, &IID_IDXGIResource1, (void **)&res1))) {
    return -2;
  }
  HRESULT hr = IDXGIResource1_CreateSharedHandle(res1, NULL, GENERIC_ALL, NULL,
                                                 &p->pyro_shared_tex_handle);
  IDXGIResource1_Release(res1);
  if (FAILED(hr) || !p->pyro_shared_tex_handle) return -3;

  return zc_pyro_setup_d3d11_enc_interop(p->pyro, p->pyro_shared_tex_handle,
                                         p->pyro_fence_handle, p->width, p->height);
}

static void release_pyrowave_dec_shared(gpu_pipeline_t *p) {
  for (int i = 0; i < 3; ++i) {
    if (p->pyro_dec_shared_srv[i]) {
      ID3D11ShaderResourceView_Release(p->pyro_dec_shared_srv[i]);
      p->pyro_dec_shared_srv[i] = NULL;
    }
    if (p->pyro_dec_shared_handle[i]) {
      CloseHandle(p->pyro_dec_shared_handle[i]);
      p->pyro_dec_shared_handle[i] = NULL;
    }
    if (p->pyro_dec_shared_tex[i]) {
      ID3D11Texture2D_Release(p->pyro_dec_shared_tex[i]);
      p->pyro_dec_shared_tex[i] = NULL;
    }
  }
}

static int setup_pyrowave_d3d11_dec_interop(gpu_pipeline_t *p) {
  const char *force_cpu = getenv("PYRO_DEC_CPU");
  if (force_cpu && strcmp(force_cpu, "1") == 0) {
    return -100;
  }
  if (setup_pyrowave_d3d11_fence(p) != 0) return -4;

  for (int i = 0; i < 3; ++i) {
    uint32_t pw = (i == 0) ? p->width : (p->width >> 1);
    uint32_t ph = (i == 0) ? p->height : (p->height >> 1);

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = pw;
    td.Height = ph;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                   D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->pyro_dec_shared_tex[i]))) {
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->pyro_dec_shared_tex[i]))) {
        release_pyrowave_dec_shared(p);
        return -1;
      }
    }

    if (FAILED(ID3D11Device_CreateShaderResourceView(p->dev, (ID3D11Resource *)p->pyro_dec_shared_tex[i],
                                                    NULL, &p->pyro_dec_shared_srv[i]))) {
      release_pyrowave_dec_shared(p);
      return -2;
    }

    IDXGIResource1 *res1 = NULL;
    if (FAILED(ID3D11Texture2D_QueryInterface(p->pyro_dec_shared_tex[i], &IID_IDXGIResource1, (void **)&res1))) {
      release_pyrowave_dec_shared(p);
      return -3;
    }
    HRESULT hr = IDXGIResource1_CreateSharedHandle(res1, NULL, GENERIC_ALL, NULL,
                                                   &p->pyro_dec_shared_handle[i]);
    IDXGIResource1_Release(res1);
    if (FAILED(hr) || !p->pyro_dec_shared_handle[i]) {
      release_pyrowave_dec_shared(p);
      return -5;
    }
  }

  int rc = zc_pyro_setup_d3d11_dec_interop(p->pyro, p->pyro_dec_shared_handle[0],
                                           p->pyro_dec_shared_handle[1],
                                           p->pyro_dec_shared_handle[2],
                                           p->pyro_fence_handle, p->width, p->height);
  if (rc != 0) {
    release_pyrowave_dec_shared(p);
    return rc;
  }
  return 0;
}

static int setup_pyrowave_decode_d3d11(gpu_pipeline_t *p, bool need_cpu_staging) {
  const uint32_t w = p->width;
  const uint32_t h = p->height;
  const uint32_t cw = w >> 1;
  const uint32_t ch = h >> 1;

  if (need_cpu_staging) {
    p->pyro_dec_y = (uint8_t *)malloc((size_t)w * (size_t)h);
    p->pyro_dec_cb = (uint8_t *)malloc((size_t)cw * (size_t)ch);
    p->pyro_dec_cr = (uint8_t *)malloc((size_t)cw * (size_t)ch);
    if (!p->pyro_dec_y || !p->pyro_dec_cb || !p->pyro_dec_cr) return -1;

    D3D11_TEXTURE2D_DESC td = {0};
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    td.Width = w;
    td.Height = h;
    if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->pyro_y_tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(p->dev, (ID3D11Resource *)p->pyro_y_tex, NULL, &p->pyro_y_srv))) {
      return -2;
    }

    td.Width = cw;
    td.Height = ch;
    if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->pyro_cb_tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(p->dev, (ID3D11Resource *)p->pyro_cb_tex, NULL, &p->pyro_cb_srv)) ||
        FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->pyro_cr_tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(p->dev, (ID3D11Resource *)p->pyro_cr_tex, NULL, &p->pyro_cr_srv))) {
      return -3;
    }
  }

  D3D11_TEXTURE2D_DESC od = {0};
  od.Width = w;
  od.Height = h;
  od.MipLevels = 1;
  od.ArraySize = 1;
  od.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  od.SampleDesc.Count = 1;
  od.Usage = D3D11_USAGE_DEFAULT;
  od.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  if (SUCCEEDED(ID3D11Device_CreateTexture2D(p->dev, &od, NULL, &p->pyro_dec_tex)) &&
      SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(p->dev, (ID3D11Resource *)p->pyro_dec_tex, NULL, &p->pyro_dec_uav))) {
    p->pyro_dec_use_copy = true;
  } else {
    if (p->pyro_dec_tex) { ID3D11Texture2D_Release(p->pyro_dec_tex); p->pyro_dec_tex = NULL; }
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &od, NULL, &p->pyro_dec_tex)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(p->dev, (ID3D11Resource *)p->pyro_dec_tex, NULL, &p->pyro_dec_uav))) {
      return -4;
    }
    p->pyro_dec_use_copy = false;
  }

  D3D11_BUFFER_DESC cbd = {0};
  cbd.ByteWidth = 16;
  cbd.Usage = D3D11_USAGE_DEFAULT;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  uint32_t cb_init[4] = {w, h, 0U, 0U};
  D3D11_SUBRESOURCE_DATA cbs = {cb_init, 0, 0};
  if (FAILED(ID3D11Device_CreateBuffer(p->dev, &cbd, &cbs, &p->pyro_dec_cbuf))) {
    return -5;
  }

  ID3DBlob *cs_blob = NULL;
  ID3DBlob *err_blob = NULL;
  HRESULT hr = D3DCompile(k_pyro_yuv2rgba_hlsl, sizeof(k_pyro_yuv2rgba_hlsl) - 1,
                          "pyro_yuv2rgba.hlsl", NULL, NULL, "CSPyroYUVToRGBA",
                          "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_blob, &err_blob);
  if (FAILED(hr) || !cs_blob) {
    if (err_blob) {
      gpu_log_err("[GPU][PYRO] HLSL compile error: %s\n", (const char *)ID3D10Blob_GetBufferPointer(err_blob));
      ID3D10Blob_Release(err_blob);
    }
    if (cs_blob) ID3D10Blob_Release(cs_blob);
    return -6;
  }
  if (err_blob) ID3D10Blob_Release(err_blob);
  hr = ID3D11Device_CreateComputeShader(p->dev, ID3D10Blob_GetBufferPointer(cs_blob),
                                        ID3D10Blob_GetBufferSize(cs_blob), NULL,
                                        &p->pyro_cs_yuv2rgba);
  ID3D10Blob_Release(cs_blob);
  return SUCCEEDED(hr) ? 0 : -7;
}

static int upload_r8_plane(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex,
                           const uint8_t *src, uint32_t w, uint32_t h) {
  D3D11_MAPPED_SUBRESOURCE mr = {0};
  if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)tex, 0,
                                     D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
    return -1;
  }
  uint8_t *dst = (uint8_t *)mr.pData;
  for (uint32_t y = 0; y < h; ++y) {
    memcpy(dst + (size_t)y * mr.RowPitch, src + (size_t)y * w, w);
  }
  ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)tex, 0);
  return 0;
}

int gpu_pipeline_create_pyrowave(gpu_pipeline_t **out_pipe, uint32_t width,
                                 uint32_t height, uint32_t fps,
                                 uint32_t bitrate_bps, bool show_window,
                                 gpu_pipeline_info_t *out_info) {
  if (!out_pipe || width < 16U || height < 16U) return -1;
  width &= ~1U;
  height &= ~1U;
  QueryPerformanceFrequency(&g_qpc_freq);

  gpu_pipeline_t *p = (gpu_pipeline_t *)calloc(1, sizeof(*p));
  if (!p) return -1;
  p->use_pyrowave = true;
  p->width = width;
  p->height = height;
  p->fps = fps ? fps : 60U;
  p->bitrate_bps = bitrate_bps ? bitrate_bps : 40000000U;
  p->enc_out_cap = 4 * 1024 * 1024;
  p->enc_out_buf = (uint8_t *)malloc(p->enc_out_cap);
  if (!p->enc_out_buf) {
    free(p);
    return -2;
  }

  gpu_pipeline_info_t info;
  memset(&info, 0, sizeof(info));
  info.width = width;
  info.height = height;

  int rc = create_d3d11_and_swapchain(p, show_window, &info);
  if (rc != 0) {
    gpu_pipeline_destroy(p);
    return rc;
  }

  /* Match Vulkan device to D3D11 adapter LUID for zero-copy external memory sharing */
  uint8_t luid[8] = {0};
  bool have_luid = false;
  IDXGIDevice *dxgi_dev = NULL;
  if (SUCCEEDED(ID3D11Device_QueryInterface(p->dev, &IID_IDXGIDevice, (void **)&dxgi_dev))) {
    IDXGIAdapter *ad = NULL;
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &ad))) {
      DXGI_ADAPTER_DESC ad_desc;
      if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &ad_desc))) {
        memcpy(luid, &ad_desc.AdapterLuid, 8);
        have_luid = true;
      }
      IDXGIAdapter_Release(ad);
    }
    IDXGIDevice_Release(dxgi_dev);
  }

  char pyro_name[128] = {0};
  rc = zc_pyro_create(&p->pyro, width, height, have_luid ? luid : NULL,
                      true, true, pyro_name, sizeof(pyro_name));
  if (rc != 0) {
    gpu_log_err("[GPU][PYRO] zc_pyro_create failed (%d)\n", rc);
    gpu_pipeline_destroy(p);
    return -20;
  }

  bool interop_ok = (setup_pyrowave_d3d11_interop(p) == 0);
  if (!interop_ok) {
    gpu_log_err("[GPU][PYRO] D3D11<->Vulkan interop unavailable; using NV12 staging fallback\n");
    D3D11_TEXTURE2D_DESC sd = {0};
    sd.Width = width;
    sd.Height = height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_NV12;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &sd, NULL, &p->pyro_nv12_stg))) {
      gpu_pipeline_destroy(p);
      return -21;
    }
  }

  bool dec_interop_ok = (setup_pyrowave_d3d11_dec_interop(p) == 0);
  if (!dec_interop_ok) {
    gpu_log_err("[GPU][PYRO] D3D11<->Vulkan decode interop unavailable; using CPU staging fallback\n");
  }

  rc = setup_pyrowave_decode_d3d11(p, !dec_interop_ok);
  if (rc != 0) {
    gpu_log_err("[GPU][PYRO] setup_pyrowave_decode_d3d11 failed (%d)\n", rc);
    gpu_pipeline_destroy(p);
    return -22;
  }

  snprintf(info.encoder_name, sizeof(info.encoder_name),
           "Pyrowave Vulkan Wavelet (%s)", interop_ok ? "D3D11 Zero-Copy" : "NV12 Staging");
  snprintf(info.decoder_name, sizeof(info.decoder_name),
           "Pyrowave Vulkan Wavelet Decoder (%s, BT.709 4:2:0)",
           dec_interop_ok ? "D3D11 Zero-Copy" : "CPU Staging");

  *out_pipe = p;
  if (out_info) *out_info = info;
  return 0;
}
#else
int gpu_pipeline_create_pyrowave(gpu_pipeline_t **out_pipe, uint32_t width,
                                 uint32_t height, uint32_t fps,
                                 uint32_t bitrate_bps, bool show_window,
                                 gpu_pipeline_info_t *out_info) {
  (void)out_pipe; (void)width; (void)height; (void)fps;
  (void)bitrate_bps; (void)show_window; (void)out_info;
  fprintf(stderr, "Pyrowave codec support was not compiled into this binary (ZC_HAVE_PYROWAVE=0)\n");
  return -1;
}
#endif

void gpu_pipeline_set_bitrate(gpu_pipeline_t *p, uint32_t bitrate_bps) {
  if (!p) return;
  p->bitrate_bps = bitrate_bps;
}

void gpu_pipeline_zcvid_request_keyframe(gpu_pipeline_t *p) {
  if (p && p->use_zcvid) p->zcvid_kf_countdown = p->zcvid_tq ? 1U : 16U;
}

void gpu_pipeline_zcvid_set_packed(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_zcvid) p->zcvid_packed = enable;
}

void gpu_pipeline_zcvid_set_transform(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_zcvid) p->zcvid_tq = enable;
}

void gpu_pipeline_zcvid_hint_stats(gpu_pipeline_t *p, uint64_t *frames, uint64_t *move_rects,
                                     uint64_t *moved_tiles, uint64_t *static_tiles) {
  if (!p) return;
  if (frames) *frames = p->hint_frames;
  if (move_rects) *move_rects = p->hint_move_rects;
  if (moved_tiles) *moved_tiles = p->hint_moved_tiles;
  if (static_tiles) *static_tiles = p->hint_static_tiles;
}

void gpu_pipeline_zcvid_set_dither(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_zcvid) p->zcvid_dither = enable;
}

void gpu_pipeline_zcvid_set_qpel(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_zcvid) p->zcvid_qpel = enable;
}

void gpu_pipeline_zcvid_set_transform_inter(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_zcvid) p->zcvid_tq_inter = enable;
}

void gpu_pipeline_zcvid_set_entropy(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_zcvid) p->zcvid_ec = enable;
}

uint32_t gpu_pipeline_zcvid_entropy_budget(gpu_pipeline_t *p) {
  if (!p || !p->use_zcvid) return 0;
  return zcvid_ec_budget_words(p, p->zcvid_ec_dec_nsw_q8);
}

void gpu_pipeline_zcvid_set_entropy_limit(gpu_pipeline_t *p, uint32_t record_words) {
  if (!p || !p->use_zcvid) return;
  uint32_t floor_w = (p->fps >= 240U) ? 2048U : 8192U;
  if (record_words != 0U && record_words < floor_w) record_words = floor_w;
  p->zcvid_ec_peer_limit = record_words & ~1U;
}

void gpu_pipeline_force_synth(gpu_pipeline_t *p, bool enable) {
  if (!p) return;
  if (enable) {
    if (p->dupl) IDXGIOutputDuplication_Release(p->dupl);
    p->dupl = NULL;
    if (p->dupl_out1) IDXGIOutput1_Release(p->dupl_out1); /* no retry either */
    p->dupl_out1 = NULL;
    p->dupl_has_frame = false;
    p->fallback_bg_init = false;
  }
}

void gpu_pipeline_destroy(gpu_pipeline_t *p) {
  if (!p) return;
#ifdef ZC_HAVE_PYROWAVE
  if (p->pyro) zc_pyro_destroy(p->pyro);
  release_pyrowave_dec_shared(p);
  if (p->pyro_cs_yuv2rgba) ID3D11ComputeShader_Release(p->pyro_cs_yuv2rgba);
  if (p->pyro_dec_cbuf) ID3D11Buffer_Release(p->pyro_dec_cbuf);
  if (p->pyro_dec_uav) ID3D11UnorderedAccessView_Release(p->pyro_dec_uav);
  if (p->pyro_dec_tex) ID3D11Texture2D_Release(p->pyro_dec_tex);
  if (p->pyro_cr_srv) ID3D11ShaderResourceView_Release(p->pyro_cr_srv);
  if (p->pyro_cb_srv) ID3D11ShaderResourceView_Release(p->pyro_cb_srv);
  if (p->pyro_y_srv) ID3D11ShaderResourceView_Release(p->pyro_y_srv);
  if (p->pyro_cr_tex) ID3D11Texture2D_Release(p->pyro_cr_tex);
  if (p->pyro_cb_tex) ID3D11Texture2D_Release(p->pyro_cb_tex);
  if (p->pyro_y_tex) ID3D11Texture2D_Release(p->pyro_y_tex);
  free(p->pyro_dec_y);
  free(p->pyro_dec_cb);
  free(p->pyro_dec_cr);
  if (p->pyro_nv12_stg) ID3D11Texture2D_Release(p->pyro_nv12_stg);
  if (p->pyro_fence_handle) CloseHandle(p->pyro_fence_handle);
  if (p->pyro_fence) ID3D11Fence_Release(p->pyro_fence);
  if (p->pyro_ctx4) ID3D11DeviceContext4_Release(p->pyro_ctx4);
  if (p->pyro_dev5) ID3D11Device5_Release(p->pyro_dev5);
  if (p->pyro_shared_tex_handle) CloseHandle(p->pyro_shared_tex_handle);
  if (p->pyro_shared_bgra) ID3D11Texture2D_Release(p->pyro_shared_bgra);
#endif
  if (p->zcvid_dec_cb) ID3D11Buffer_Release(p->zcvid_dec_cb);
  if (p->cursor_stg) ID3D11Texture2D_Release(p->cursor_stg);
  free(p->cursor_shape);
  if (p->zcvid_enc_cb) ID3D11Buffer_Release(p->zcvid_enc_cb);
  if (p->zcvid_dec_uav) ID3D11UnorderedAccessView_Release(p->zcvid_dec_uav);
  if (p->zcvid_dec_tex) ID3D11Texture2D_Release(p->zcvid_dec_tex);
  if (p->zcvid_in_records_srv) ID3D11ShaderResourceView_Release(p->zcvid_in_records_srv);
  if (p->zcvid_in_records_buf) ID3D11Buffer_Release(p->zcvid_in_records_buf);
  if (p->zcvid_dec_mvs_srv) ID3D11ShaderResourceView_Release(p->zcvid_dec_mvs_srv);
  if (p->zcvid_dec_mvs_buf) ID3D11Buffer_Release(p->zcvid_dec_mvs_buf);
  for (int i = 0; i < 2; ++i) {
    if (p->zcvid_dec_yuvd_uav[i]) ID3D11UnorderedAccessView_Release(p->zcvid_dec_yuvd_uav[i]);
    if (p->zcvid_dec_yuvd_srv[i]) ID3D11ShaderResourceView_Release(p->zcvid_dec_yuvd_srv[i]);
    if (p->zcvid_dec_yuvd_buf[i]) ID3D11Buffer_Release(p->zcvid_dec_yuvd_buf[i]);
    if (p->zcvid_enc_yuvd_uav[i]) ID3D11UnorderedAccessView_Release(p->zcvid_enc_yuvd_uav[i]);
    if (p->zcvid_enc_yuvd_srv[i]) ID3D11ShaderResourceView_Release(p->zcvid_enc_yuvd_srv[i]);
    if (p->zcvid_enc_yuvd_buf[i]) ID3D11Buffer_Release(p->zcvid_enc_yuvd_buf[i]);
  }
  for (uint32_t s = 0; s < ZCVID_STG_RING; ++s) {
    if (p->zcvid_out_counter_stg[s]) ID3D11Buffer_Release(p->zcvid_out_counter_stg[s]);
    if (p->zcvid_out_records_stg[s]) ID3D11Buffer_Release(p->zcvid_out_records_stg[s]);
    if (p->zcvid_enc_mvs_stg[s]) ID3D11Buffer_Release(p->zcvid_enc_mvs_stg[s]);
  }
  if (p->zcvid_prof_en && p->zcvid_prof_count > 0U) {
    double n = (double)p->zcvid_prof_count;
    fprintf(stderr,
            "[ZCVID_PROFILE %ux%u@%uHz n=%u] GPU(us): luma=%.0f mv=%.0f pred=%.0f tq=%.0f enc=%.0f dma=%.0f | "
            "CPU(us): wait=%.0f mvpack=%.0f ecpack=%.0f | tot=%.0f max=%.0f\n",
            p->width, p->height, p->fps, p->zcvid_prof_count,
            p->zcvid_prof_acc[0] / n, p->zcvid_prof_acc[1] / n, p->zcvid_prof_acc[2] / n,
            p->zcvid_prof_acc[3] / n, p->zcvid_prof_acc[4] / n, p->zcvid_prof_acc[5] / n,
            p->zcvid_prof_acc[6] / n, p->zcvid_prof_acc[7] / n, p->zcvid_prof_acc[8] / n,
            p->zcvid_prof_acc[9] / n, p->zcvid_prof_acc[10]);
  }
  if (p->zcvid_prof_en && p->zcvid_dec_prof_count > 0U) {
    double n = (double)p->zcvid_dec_prof_count;
    fprintf(stderr,
            "[ZCVID_DEC_PROFILE %ux%u@%uHz n=%u] mv_dec=%.0f mv_up=%.0f ec_dec=%.0f rec_up=%.0f disp=%.0f | tot=%.0f\n",
            p->width, p->height, p->fps, p->zcvid_dec_prof_count,
            p->zcvid_dec_prof_acc[0] / n, p->zcvid_dec_prof_acc[1] / n, p->zcvid_dec_prof_acc[2] / n,
            p->zcvid_dec_prof_acc[3] / n, p->zcvid_dec_prof_acc[4] / n, p->zcvid_dec_prof_acc[5] / n);
  }
  if (p->zcvid_prof_en && p->zcvid_ec_prof_enc_n > 0U && p->zcvid_ec_prof_dec_n > 0U) {
    double ne = (double)p->zcvid_ec_prof_enc_n, nd = (double)p->zcvid_ec_prof_dec_n;
    fprintf(stderr,
            "[ZCVID_EC_XRAY chunks=%u] enc: sort=%.0f pool=%.0f max_c=%.0f | dec: max_c=%.0f min_c=%.0f | recs: d=%.0f i=%.0f tq=%.0f\n",
            p->zcvid_ec_prof_chunks,
            p->zcvid_ec_prof_acc[0] / ne, p->zcvid_ec_prof_acc[1] / ne, p->zcvid_ec_prof_acc[2] / ne,
            p->zcvid_ec_prof_acc[3] / nd, p->zcvid_ec_prof_acc[4] / nd,
            p->zcvid_ec_prof_acc[5] / ne, p->zcvid_ec_prof_acc[6] / ne, p->zcvid_ec_prof_acc[7] / ne);
  }
  if (p->zcvid_prof_disj) ID3D11Query_Release(p->zcvid_prof_disj);
  for (int i = 0; i < 8; ++i) {
    if (p->zcvid_prof_ts[i]) ID3D11Query_Release(p->zcvid_prof_ts[i]);
  }
  if (p->zcvid_out_counter_uav) ID3D11UnorderedAccessView_Release(p->zcvid_out_counter_uav);
  if (p->zcvid_out_counter_buf) ID3D11Buffer_Release(p->zcvid_out_counter_buf);
  if (p->zcvid_out_records_uav) ID3D11UnorderedAccessView_Release(p->zcvid_out_records_uav);
  if (p->zcvid_out_records_buf) ID3D11Buffer_Release(p->zcvid_out_records_buf);
  for (int i = 0; i < 2; ++i) {
    if (p->zcvid_enc_mvs_uav[i]) ID3D11UnorderedAccessView_Release(p->zcvid_enc_mvs_uav[i]);
    if (p->zcvid_enc_mvs_srv[i]) ID3D11ShaderResourceView_Release(p->zcvid_enc_mvs_srv[i]);
    if (p->zcvid_enc_mvs_buf[i]) ID3D11Buffer_Release(p->zcvid_enc_mvs_buf[i]);
  }
  for (int i = 0; i < 2; ++i) {
    if (p->zcvid_half_uav[i]) ID3D11UnorderedAccessView_Release(p->zcvid_half_uav[i]);
    if (p->zcvid_half_srv[i]) ID3D11ShaderResourceView_Release(p->zcvid_half_srv[i]);
    if (p->zcvid_half_tex[i]) ID3D11Texture2D_Release(p->zcvid_half_tex[i]);
    if (p->zcvid_quarter_uav[i]) ID3D11UnorderedAccessView_Release(p->zcvid_quarter_uav[i]);
    if (p->zcvid_quarter_srv[i]) ID3D11ShaderResourceView_Release(p->zcvid_quarter_srv[i]);
    if (p->zcvid_quarter_tex[i]) ID3D11Texture2D_Release(p->zcvid_quarter_tex[i]);
  }
  if (p->zcvid_cs_luma_quarter) ID3D11ComputeShader_Release(p->zcvid_cs_luma_quarter);
  if (p->zcvid_cs_luma_half) ID3D11ComputeShader_Release(p->zcvid_cs_luma_half);
  if (p->zcvid_cs_motion_pred) ID3D11ComputeShader_Release(p->zcvid_cs_motion_pred);
  if (p->zcvid_enc_pred_srv) ID3D11ShaderResourceView_Release(p->zcvid_enc_pred_srv);
  if (p->zcvid_enc_pred_uav) ID3D11UnorderedAccessView_Release(p->zcvid_enc_pred_uav);
  if (p->zcvid_enc_pred_buf) ID3D11Buffer_Release(p->zcvid_enc_pred_buf);
  if (p->zcvid_prev_src_srv) ID3D11ShaderResourceView_Release(p->zcvid_prev_src_srv);
  if (p->zcvid_prev_src_rtv) ID3D11RenderTargetView_Release(p->zcvid_prev_src_rtv);
  if (p->zcvid_prev_src_tex) ID3D11Texture2D_Release(p->zcvid_prev_src_tex);
  free(p->zcvid_dec_unpack_buf);
  free(p->zcvid_dec_mv_scratch);
  if (p->cap_bgra_srv) ID3D11ShaderResourceView_Release(p->cap_bgra_srv);
  if (p->zcvid_cs_psnr) ID3D11ComputeShader_Release(p->zcvid_cs_psnr);
  if (p->zcvid_psnr_cb) ID3D11Buffer_Release(p->zcvid_psnr_cb);
  if (p->zcvid_psnr_uav) ID3D11UnorderedAccessView_Release(p->zcvid_psnr_uav);
  if (p->zcvid_psnr_buf) ID3D11Buffer_Release(p->zcvid_psnr_buf);
  if (p->zcvid_psnr_stg) ID3D11Buffer_Release(p->zcvid_psnr_stg);
  if (p->zcvid_dec_srv) ID3D11ShaderResourceView_Release(p->zcvid_dec_srv);
  if (p->zcvid_cs_ycbcr_to_rgba) ID3D11ComputeShader_Release(p->zcvid_cs_ycbcr_to_rgba);
  if (p->zcvid_cs_apply) ID3D11ComputeShader_Release(p->zcvid_cs_apply);
  if (p->zcvid_cs_motion_decay) ID3D11ComputeShader_Release(p->zcvid_cs_motion_decay);
  if (p->zcvid_cs_encode) ID3D11ComputeShader_Release(p->zcvid_cs_encode);
  if (p->zcvid_cs_intra_t) ID3D11ComputeShader_Release(p->zcvid_cs_intra_t);
  if (p->zcvid_cs_intra_t_count) ID3D11ComputeShader_Release(p->zcvid_cs_intra_t_count);
  if (p->zcvid_cs_tq_scan_a) ID3D11ComputeShader_Release(p->zcvid_cs_tq_scan_a);
  if (p->zcvid_cs_tq_mid) ID3D11ComputeShader_Release(p->zcvid_cs_tq_mid);
  if (p->zcvid_cs_tq_clear) ID3D11ComputeShader_Release(p->zcvid_cs_tq_clear);
  if (p->zcvid_cs_tq_final) ID3D11ComputeShader_Release(p->zcvid_cs_tq_final);
  if (p->zcvid_tq_scratch_uav) ID3D11UnorderedAccessView_Release(p->zcvid_tq_scratch_uav);
  if (p->zcvid_tq_scratch_buf) ID3D11Buffer_Release(p->zcvid_tq_scratch_buf);
  if (p->zcvid_tq_stage_uav) ID3D11UnorderedAccessView_Release(p->zcvid_tq_stage_uav);
  if (p->zcvid_tq_stage_buf) ID3D11Buffer_Release(p->zcvid_tq_stage_buf);
  if (p->zcvid_hint_srv) ID3D11ShaderResourceView_Release(p->zcvid_hint_srv);
  if (p->zcvid_hint_buf) ID3D11Buffer_Release(p->zcvid_hint_buf);
  free(p->zcvid_hint_cpu);
  free(p->dupl_meta);
  if (p->zcvid_cs_itrans) ID3D11ComputeShader_Release(p->zcvid_cs_itrans);
  if (p->zcvid_enc_tflag_srv) ID3D11ShaderResourceView_Release(p->zcvid_enc_tflag_srv);
  if (p->zcvid_enc_tflag_uav) ID3D11UnorderedAccessView_Release(p->zcvid_enc_tflag_uav);
  if (p->zcvid_enc_tflag_buf) ID3D11Buffer_Release(p->zcvid_enc_tflag_buf);
  if (p->zcvid_dec_coef_uav) ID3D11UnorderedAccessView_Release(p->zcvid_dec_coef_uav);
  if (p->zcvid_dec_coef_buf) ID3D11Buffer_Release(p->zcvid_dec_coef_buf);
  if (p->zcvid_dec_tflag_uav) ID3D11UnorderedAccessView_Release(p->zcvid_dec_tflag_uav);
  if (p->zcvid_dec_tflag_buf) ID3D11Buffer_Release(p->zcvid_dec_tflag_buf);
  if (p->zcvid_cs_mv_smooth) ID3D11ComputeShader_Release(p->zcvid_cs_mv_smooth);
  if (p->zcvid_cs_tile_mv) ID3D11ComputeShader_Release(p->zcvid_cs_tile_mv);
#ifdef ZC_HAVE_NVENC
  if (p->use_direct_nvenc && p->nvenc_session) {
    if (p->nvenc_bitstream) p->nvenc_fn.nvEncDestroyBitstreamBuffer(p->nvenc_session, p->nvenc_bitstream);
    if (p->nvenc_reg_res) p->nvenc_fn.nvEncUnregisterResource(p->nvenc_session, p->nvenc_reg_res);
    p->nvenc_fn.nvEncDestroyEncoder(p->nvenc_session);
  }
  if (p->nvenc_dll) FreeLibrary(p->nvenc_dll);
#endif
  if (p->enc_ev) IMFMediaEventGenerator_Release(p->enc_ev);
  if (p->enc_mft) IMFTransform_Release(p->enc_mft);
  if (p->dec_mft) IMFTransform_Release(p->dec_mft);
  if (p->dxgi_mgr) IMFDXGIDeviceManager_Release(p->dxgi_mgr);
  if (p->vp) ID3D11VideoProcessor_Release(p->vp);
  if (p->vp_enum) ID3D11VideoProcessorEnumerator_Release(p->vp_enum);
  if (p->vctx) ID3D11VideoContext_Release(p->vctx);
  if (p->vdev) ID3D11VideoDevice_Release(p->vdev);
  if (p->dupl) IDXGIOutputDuplication_Release(p->dupl);
  if (p->dupl_out1) IDXGIOutput1_Release(p->dupl_out1);
  if (p->enc_nv12_tex) ID3D11Texture2D_Release(p->enc_nv12_tex);
  if (p->cap_rtv) ID3D11RenderTargetView_Release(p->cap_rtv);
  if (p->cap_bgra_tex) ID3D11Texture2D_Release(p->cap_bgra_tex);
  if (p->dupl_stage_tex) ID3D11Texture2D_Release(p->dupl_stage_tex);
  if (p->swapchain) IDXGISwapChain1_Release(p->swapchain);
  if (p->ctx1) ID3D11DeviceContext1_Release(p->ctx1);
  if (p->ctx) ID3D11DeviceContext_Release(p->ctx);
  if (p->dev) ID3D11Device_Release(p->dev);
  if (p->hwnd) DestroyWindow(p->hwnd);
  free(p->zcvid_sort_a);
  free(p->zcvid_sort_b);
  zcvid_pool_destroy(p->zcvid_pool);
  for (uint32_t i = 0; i < ZCVID_EC_MAX_CHUNKS; ++i) {
    free(p->zcvid_ec_line[i]);
    free(p->zcvid_ec_lrow[i]);
    free(p->zcvid_ec_model[i]);
  }
  free(p->zcvid_ec_scratch);
  free(p->enc_out_buf);
  const bool mf_started = !p->use_zcvid && !p->use_pyrowave; /* only gpu_pipeline_create starts it */
  free(p);
  if (mf_started) MFShutdown();
}

/* Video-processor blit of the top-left src_w x src_h of src_tex onto the
 * whole of dst_tex (p->width x p->height): converts the format, and scales
 * with the aspect ratio kept (black bars) when the sizes differ. */
static int vp_blit_scaled(gpu_pipeline_t *p, ID3D11Texture2D *src_tex, UINT src_slice,
                          uint32_t src_w, uint32_t src_h, ID3D11Texture2D *dst_tex) {
  static unsigned vp_err_count = 0;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {0};
  ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  ivd.Texture2D.ArraySlice = src_slice;
  ID3D11VideoProcessorInputView *iv = NULL;
  HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorInputView(
      p->vdev, (ID3D11Resource *)src_tex, p->vp_enum, &ivd, &iv);
  if (FAILED(hr)) {
    if (vp_err_count++ < 10) {
      gpu_log_err("[GPU][VP] CreateVideoProcessorInputView failed: hr=0x%08lx slice=%u\n",
                  (unsigned long)hr, src_slice);
    }
    return -1;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {0};
  ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  ID3D11VideoProcessorOutputView *ov = NULL;
  hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(
      p->vdev, (ID3D11Resource *)dst_tex, p->vp_enum, &ovd, &ov);
  if (FAILED(hr)) {
    if (vp_err_count++ < 10) {
      gpu_log_err("[GPU][VP] CreateVideoProcessorOutputView failed: hr=0x%08lx\n",
                  (unsigned long)hr);
    }
    ID3D11VideoProcessorInputView_Release(iv);
    return -1;
  }

  RECT sr = {0, 0, (LONG)src_w, (LONG)src_h};
  RECT r = {0, 0, (LONG)p->width, (LONG)p->height};
  RECT dr = r;
  if (src_w != p->width || src_h != p->height) {
    /* Largest rectangle of the source's shape that fits, centred. */
    uint64_t fit_w = (uint64_t)p->height * src_w / (src_h ? src_h : 1U);
    if (fit_w <= p->width) {
      dr.left = (LONG)((p->width - (uint32_t)fit_w) / 2U);
      dr.right = dr.left + (LONG)fit_w;
    } else {
      uint64_t fit_h = (uint64_t)p->width * src_h / (src_w ? src_w : 1U);
      dr.top = (LONG)((p->height - (uint32_t)fit_h) / 2U);
      dr.bottom = dr.top + (LONG)fit_h;
    }
  }
  ID3D11VideoContext_VideoProcessorSetStreamSourceRect(p->vctx, p->vp, 0, TRUE, &sr);
  ID3D11VideoContext_VideoProcessorSetStreamDestRect(p->vctx, p->vp, 0, TRUE, &dr);
  ID3D11VideoContext_VideoProcessorSetOutputTargetRect(p->vctx, p->vp, TRUE, &r);

  D3D11_VIDEO_PROCESSOR_STREAM stream = {0};
  stream.Enable = TRUE;
  stream.pInputSurface = iv;
  hr = ID3D11VideoContext_VideoProcessorBlt(p->vctx, p->vp, ov, 0, 1, &stream);
  if (FAILED(hr) && vp_err_count++ < 10) {
    gpu_log_err("[GPU][VP] VideoProcessorBlt failed: hr=0x%08lx slice=%u\n",
                (unsigned long)hr, src_slice);
  }

  ID3D11VideoProcessorOutputView_Release(ov);
  ID3D11VideoProcessorInputView_Release(iv);
  return SUCCEEDED(hr) ? 0 : -1;
}

static int vp_blit_textures(gpu_pipeline_t *p, ID3D11Texture2D *src_tex,
                            UINT src_slice, ID3D11Texture2D *dst_tex) {
  return vp_blit_scaled(p, src_tex, src_slice, p->width, p->height, dst_tex);
}

/* Brings a duplicated desktop frame into cap_bgra_tex. Returns false when the
 * frame could not be used (the previous capture then stays on screen). */
static bool capture_desktop_frame(gpu_pipeline_t *p, ID3D11Texture2D *dupl_tex) {
  D3D11_TEXTURE2D_DESC sd;
  ID3D11Texture2D_GetDesc(dupl_tex, &sd);
  if (sd.Width == p->width && sd.Height == p->height &&
      sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->cap_bgra_tex,
                                     (ID3D11Resource *)dupl_tex);
    return true;
  }
  /* Another mode: the duplication surface cannot feed the video processor
   * itself, so it goes through a texture of its own size that can. */
  if (!p->dupl_stage_tex || p->dupl_stage_w != sd.Width || p->dupl_stage_h != sd.Height ||
      p->dupl_stage_fmt != sd.Format) {
    if (p->dupl_stage_tex) ID3D11Texture2D_Release(p->dupl_stage_tex);
    p->dupl_stage_tex = NULL;
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = sd.Width;
    td.Height = sd.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = sd.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->dupl_stage_tex))) {
      p->dupl_stage_tex = NULL;
      return false;
    }
    p->dupl_stage_w = sd.Width;
    p->dupl_stage_h = sd.Height;
    p->dupl_stage_fmt = sd.Format;
    gpu_log_err("[GPU][CAP] desktop is %ux%u (format %u): scaling to %ux%u\n",
                (unsigned)sd.Width, (unsigned)sd.Height, (unsigned)sd.Format,
                (unsigned)p->width, (unsigned)p->height);
  }
  ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->dupl_stage_tex,
                                   (ID3D11Resource *)dupl_tex);
  return vp_blit_scaled(p, p->dupl_stage_tex, 0U, sd.Width, sd.Height, p->cap_bgra_tex) == 0;
}

/* Turns the move and dirty rectangles DXGI reports with a desktop frame into
 * per-tile hints for CSTileMV (see g_mv_hint): tiles nothing touched skip the
 * motion search, tiles inside a moved rectangle get its vector as a
 * candidate. Rectangles are in desktop pixels and scaled to the stream when
 * the desktop has another size. Returns false when no hint could be built
 * (every tile is then searched as usual). */
static bool build_capture_hints(gpu_pipeline_t *p, const DXGI_OUTDUPL_FRAME_INFO *fi,
                                uint32_t desk_w, uint32_t desk_h) {
  if (!p->use_zcvid || !p->zcvid_hint_cpu || fi->TotalMetadataBufferSize == 0U) return false;
  if (fi->TotalMetadataBufferSize > p->dupl_meta_cap) {
    if (p->dupl_meta_cap != 0U) zcvid_note_late_alloc("capture metadata", fi->TotalMetadataBufferSize);
    uint8_t *m = (uint8_t *)realloc(p->dupl_meta, fi->TotalMetadataBufferSize);
    if (!m) return false;
    p->dupl_meta = m;
    p->dupl_meta_cap = fi->TotalMetadataBufferSize;
  }
  UINT move_bytes = 0, dirty_bytes = 0;
  if (FAILED(IDXGIOutputDuplication_GetFrameMoveRects(p->dupl, p->dupl_meta_cap,
                                                     (DXGI_OUTDUPL_MOVE_RECT *)p->dupl_meta,
                                                     &move_bytes))) {
    return false;
  }
  UINT moves = move_bytes / (UINT)sizeof(DXGI_OUTDUPL_MOVE_RECT);
  uint8_t *dirty_at = p->dupl_meta + move_bytes;
  if (FAILED(IDXGIOutputDuplication_GetFrameDirtyRects(p->dupl, p->dupl_meta_cap - move_bytes,
                                                      (RECT *)dirty_at, &dirty_bytes))) {
    return false;
  }
  UINT dirties = dirty_bytes / (UINT)sizeof(RECT);
  const uint32_t tx_n = p->zcvid_tiles_x, ty_n = p->zcvid_tiles_y;
  /* stream pixels per desktop pixel, Q16 */
  const uint64_t sx = ((uint64_t)p->width << 16) / (desk_w ? desk_w : 1U);
  const uint64_t sy = ((uint64_t)p->height << 16) / (desk_h ? desk_h : 1U);
  int32_t *h = p->zcvid_hint_cpu;
  for (uint32_t t = 0; t < tx_n * ty_n; ++t) {
    h[t * 4U] = 2; /* unchanged unless a rectangle says otherwise */
    h[t * 4U + 1U] = h[t * 4U + 2U] = h[t * 4U + 3U] = 0;
  }
  /* Tiles a rectangle touches, in stream tiles, clamped to the frame. */
#define ZCVID_RECT_TILES(L, T, Rt, B, X0, Y0, X1, Y1)                                   \
  do {                                                                              \
    int64_t l_ = ((int64_t)(L) * (int64_t)sx) >> 16, t_ = ((int64_t)(T) * (int64_t)sy) >> 16; \
    int64_t r_ = (((int64_t)(Rt) * (int64_t)sx) + 65535) >> 16;                       \
    int64_t b_ = (((int64_t)(B) * (int64_t)sy) + 65535) >> 16;                        \
    X0 = (uint32_t)((l_ < 0) ? 0 : l_ / 32); Y0 = (uint32_t)((t_ < 0) ? 0 : t_ / 32);  \
    X1 = (uint32_t)((r_ <= 0) ? 0 : (r_ - 1) / 32 + 1);                                \
    Y1 = (uint32_t)((b_ <= 0) ? 0 : (b_ - 1) / 32 + 1);                                \
    if (X1 > tx_n) X1 = tx_n;                                                         \
    if (Y1 > ty_n) Y1 = ty_n;                                                         \
  } while (0)
  const RECT *dr = (const RECT *)dirty_at;
  for (UINT i = 0; i < dirties; ++i) {
    uint32_t x0, y0, x1, y1;
    ZCVID_RECT_TILES(dr[i].left, dr[i].top, dr[i].right, dr[i].bottom, x0, y0, x1, y1);
    for (uint32_t ty = y0; ty < y1; ++ty)
      for (uint32_t tx = x0; tx < x1; ++tx) h[(ty * tx_n + tx) * 4U] = 0;
  }
  const DXGI_OUTDUPL_MOVE_RECT *mr = (const DXGI_OUTDUPL_MOVE_RECT *)p->dupl_meta;
  for (UINT i = 0; i < moves; ++i) {
    const RECT *d = &mr[i].DestinationRect;
    /* reference pixel = pixel - vector: the vector points from source to destination */
    int32_t vx = (int32_t)((((int64_t)(d->left - mr[i].SourcePoint.x)) * (int64_t)sx) >> 16);
    int32_t vy = (int32_t)((((int64_t)(d->top - mr[i].SourcePoint.y)) * (int64_t)sy) >> 16);
    uint32_t x0, y0, x1, y1;
    ZCVID_RECT_TILES(d->left, d->top, d->right, d->bottom, x0, y0, x1, y1);
    for (uint32_t ty = y0; ty < y1; ++ty) {
      for (uint32_t tx = x0; tx < x1; ++tx) {
        int32_t *e = &h[(ty * tx_n + tx) * 4U];
        e[0] = 1;
        e[1] = vx;
        e[2] = vy;
      }
    }
  }
  /* Also mark previous and current cursor bounding boxes dirty so the encoder
   * never skips tiles where the hardware cursor appeared, moved, or disappeared. */
  if (p->cursor_has_shape) {
    uint32_t ch = (p->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
                      ? (p->cursor_info.Height / 2U)
                      : p->cursor_info.Height;
    uint32_t cw = p->cursor_info.Width;
    if (cw > 0U && ch > 0U) {
      if (p->cursor_prev_visible) {
        uint32_t x0, y0, x1, y1;
        ZCVID_RECT_TILES(p->cursor_prev_x, p->cursor_prev_y,
                        p->cursor_prev_x + (int32_t)cw, p->cursor_prev_y + (int32_t)ch,
                        x0, y0, x1, y1);
        for (uint32_t ty = y0; ty < y1; ++ty)
          for (uint32_t tx = x0; tx < x1; ++tx) h[(ty * tx_n + tx) * 4U] = 0;
      }
      if (p->cursor_visible) {
        uint32_t x0, y0, x1, y1;
        ZCVID_RECT_TILES(p->cursor_x, p->cursor_y,
                        p->cursor_x + (int32_t)cw, p->cursor_y + (int32_t)ch,
                        x0, y0, x1, y1);
        for (uint32_t ty = y0; ty < y1; ++ty)
          for (uint32_t tx = x0; tx < x1; ++tx) h[(ty * tx_n + tx) * 4U] = 0;
      }
    }
  }
#undef ZCVID_RECT_TILES
  p->hint_frames++;
  p->hint_move_rects += moves;
  for (uint32_t t = 0; t < tx_n * ty_n; ++t) {
    if (h[t * 4U] == 1) p->hint_moved_tiles++;
    else if (h[t * 4U] == 2) p->hint_static_tiles++;
  }
  return true;
}

static void update_desktop_cursor(gpu_pipeline_t *p, const DXGI_OUTDUPL_FRAME_INFO *fi) {
  p->cursor_prev_x = p->cursor_x;
  p->cursor_prev_y = p->cursor_y;
  p->cursor_prev_visible = p->cursor_visible;
  if (fi->LastMouseUpdateTime.QuadPart == 0) return;
  p->cursor_visible = (fi->PointerPosition.Visible != FALSE);
  p->cursor_x = (int32_t)fi->PointerPosition.Position.x;
  p->cursor_y = (int32_t)fi->PointerPosition.Position.y;
  if (fi->PointerShapeBufferSize > 0U) {
    if (fi->PointerShapeBufferSize > p->cursor_shape_cap) {
      uint8_t *nb = (uint8_t *)realloc(p->cursor_shape, fi->PointerShapeBufferSize);
      if (!nb) return;
      p->cursor_shape = nb;
      p->cursor_shape_cap = fi->PointerShapeBufferSize;
    }
    UINT req = 0;
    HRESULT hr = IDXGIOutputDuplication_GetFramePointerShape(
        p->dupl, p->cursor_shape_cap, p->cursor_shape, &req, &p->cursor_info);
    if (SUCCEEDED(hr)) {
      p->cursor_has_shape = true;
    }
  }
}

static void draw_desktop_cursor(gpu_pipeline_t *p, uint32_t desk_w, uint32_t desk_h) {
  if (!p->cursor_visible || !p->cursor_has_shape || !p->cursor_shape) return;
  uint32_t cw = p->cursor_info.Width;
  uint32_t ch = (p->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
                    ? (p->cursor_info.Height / 2U)
                    : p->cursor_info.Height;
  if (cw == 0U || ch == 0U || cw > 256U || ch > 256U) return;

  int32_t dst_x = (desk_w > 0U && desk_w != p->width)
                      ? (int32_t)(((int64_t)p->cursor_x * (int64_t)p->width) / (int64_t)desk_w)
                      : p->cursor_x;
  int32_t dst_y = (desk_h > 0U && desk_h != p->height)
                      ? (int32_t)(((int64_t)p->cursor_y * (int64_t)p->height) / (int64_t)desk_h)
                      : p->cursor_y;

  int32_t x0 = (dst_x < 0) ? 0 : dst_x;
  int32_t y0 = (dst_y < 0) ? 0 : dst_y;
  int32_t x1 = dst_x + (int32_t)cw;
  int32_t y1 = dst_y + (int32_t)ch;
  if (x1 > (int32_t)p->width) x1 = (int32_t)p->width;
  if (y1 > (int32_t)p->height) y1 = (int32_t)p->height;
  if (x0 >= x1 || y0 >= y1) return;

  uint32_t rw = (uint32_t)(x1 - x0);
  uint32_t rh = (uint32_t)(y1 - y0);
  if (!p->cursor_stg || rw > p->cursor_stg_w || rh > p->cursor_stg_h) {
    if (p->cursor_stg) {
      ID3D11Texture2D_Release(p->cursor_stg);
      p->cursor_stg = NULL;
    }
    uint32_t sw = (rw < 64U) ? 64U : rw;
    uint32_t sh = (rh < 64U) ? 64U : rh;
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = sw;
    td.Height = sh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->cursor_stg))) return;
    p->cursor_stg_w = sw;
    p->cursor_stg_h = sh;
  }

  D3D11_BOX src_box = {(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
  ID3D11DeviceContext_CopySubresourceRegion(p->ctx, (ID3D11Resource *)p->cursor_stg, 0, 0, 0, 0,
                                            (ID3D11Resource *)p->cap_bgra_tex, 0, &src_box);
  D3D11_MAPPED_SUBRESOURCE mr = {0};
  if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->cursor_stg, 0,
                                     D3D11_MAP_READ_WRITE, 0, &mr))) {
    return;
  }

  const uint32_t sx0 = (uint32_t)(x0 - dst_x);
  const uint32_t sy0 = (uint32_t)(y0 - dst_y);
  const UINT pitch = p->cursor_info.Pitch;
  for (uint32_t y = 0; y < rh; ++y) {
    uint8_t *drow = (uint8_t *)mr.pData + (size_t)y * mr.RowPitch;
    uint32_t sy = sy0 + y;
    if (p->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
      const uint8_t *srow = p->cursor_shape + (size_t)sy * pitch;
      for (uint32_t x = 0; x < rw; ++x) {
        const uint8_t *sp = srow + (size_t)(sx0 + x) * 4U;
        uint8_t *dp = drow + (size_t)x * 4U;
        uint32_t a = sp[3];
        if (a == 255U) {
          dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255U;
        } else if (a > 0U) {
          uint32_t ia = 255U - a;
          dp[0] = (uint8_t)((sp[0] * a + dp[0] * ia + 127U) / 255U);
          dp[1] = (uint8_t)((sp[1] * a + dp[1] * ia + 127U) / 255U);
          dp[2] = (uint8_t)((sp[2] * a + dp[2] * ia + 127U) / 255U);
          dp[3] = 255U;
        }
      }
    } else if (p->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
      const uint8_t *srow = p->cursor_shape + (size_t)sy * pitch;
      for (uint32_t x = 0; x < rw; ++x) {
        const uint8_t *sp = srow + (size_t)(sx0 + x) * 4U;
        uint8_t *dp = drow + (size_t)x * 4U;
        if (sp[3] != 0U) {
          dp[0] ^= sp[0]; dp[1] ^= sp[1]; dp[2] ^= sp[2]; dp[3] = 255U;
        } else {
          dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255U;
        }
      }
    } else if (p->cursor_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
      const uint8_t *and_row = p->cursor_shape + (size_t)sy * pitch;
      const uint8_t *xor_row = p->cursor_shape + (size_t)(sy + ch) * pitch;
      for (uint32_t x = 0; x < rw; ++x) {
        uint32_t sx = sx0 + x;
        uint8_t mask = (uint8_t)(0x80U >> (sx & 7U));
        uint8_t and_m = (and_row[sx >> 3] & mask) ? 0xFFU : 0x00U;
        uint8_t xor_m = (xor_row[sx >> 3] & mask) ? 0xFFU : 0x00U;
        uint8_t *dp = drow + (size_t)x * 4U;
        dp[0] = (uint8_t)((dp[0] & and_m) ^ xor_m);
        dp[1] = (uint8_t)((dp[1] & and_m) ^ xor_m);
        dp[2] = (uint8_t)((dp[2] & and_m) ^ xor_m);
        dp[3] = 255U;
      }
    }
  }

  ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->cap_bgra_tex, 0, &src_box,
                                        mr.pData, mr.RowPitch, 0);
  ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->cursor_stg, 0);
}

int gpu_pipeline_capture_and_encode(gpu_pipeline_t *p, uint32_t frame_idx,
                                    const uint8_t **out_bitstream,
                                    size_t *out_len, bool *out_keyframe,
                                    double *out_cap_us, double *out_enc_us) {
  LARGE_INTEGER t0, t1, t2;
  QueryPerformanceCounter(&t0);

  if (p->use_zcvid && p->zcvid_has_prev_src) {
    ID3D11Texture2D *tmp_tex = p->cap_bgra_tex;
    p->cap_bgra_tex = p->zcvid_prev_src_tex;
    p->zcvid_prev_src_tex = tmp_tex;
    ID3D11RenderTargetView *tmp_rtv = p->cap_rtv;
    p->cap_rtv = p->zcvid_prev_src_rtv;
    p->zcvid_prev_src_rtv = tmp_rtv;
    ID3D11ShaderResourceView *tmp_srv = p->cap_bgra_srv;
    p->cap_bgra_srv = p->zcvid_prev_src_srv;
    p->zcvid_prev_src_srv = tmp_srv;
  }

  ID3D11Texture2D *src_bgra = p->cap_bgra_tex;
  IDXGIResource *dupl_res = NULL;
  bool acquired_dupl = false;

  if (!p->dupl && p->dupl_out1) {
    /* Duplication was lost (or never started): try again 4 times a second. */
    uint64_t now_us = qpc_now_us();
    if (now_us - p->dupl_retry_us >= 250000ULL) {
      p->dupl_retry_us = now_us;
      if (try_start_duplication(p)) {
        gpu_log_err("[GPU][CAP] desktop duplication (re)started\n");
      }
    }
  }

  if (p->dupl) {
    DXGI_OUTDUPL_FRAME_INFO fi;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(p->dupl, 0, &fi, &dupl_res);
    if (SUCCEEDED(hr) && dupl_res) {
      ID3D11Texture2D *dupl_tex = NULL;
      if (SUCCEEDED(IDXGIResource_QueryInterface(dupl_res, &IID_ID3D11Texture2D,
                                                 (void **)&dupl_tex))) {
        if (capture_desktop_frame(p, dupl_tex)) {
          p->dupl_has_frame = true;
          D3D11_TEXTURE2D_DESC dd;
          ID3D11Texture2D_GetDesc(dupl_tex, &dd);
          update_desktop_cursor(p, &fi);
          draw_desktop_cursor(p, dd.Width, dd.Height);
          p->zcvid_hints_set = build_capture_hints(p, &fi, dd.Width, dd.Height);
        }
        ID3D11Texture2D_Release(dupl_tex);
      }
      acquired_dupl = true;
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
      /* Access lost (mode switch, fullscreen transition, secure desktop...):
       * the duplication object is dead. Keep showing the last captured frame
       * and let the retry above start a new one. */
      gpu_log_err("[GPU][CAP] desktop duplication lost (hr=0x%08lx), retrying\n",
                  (unsigned long)hr);
      IDXGIOutputDuplication_Release(p->dupl);
      p->dupl = NULL;
      p->dupl_retry_us = qpc_now_us();
    }
  }

  /* A timeout only means the desktop did not change: the previous capture is
   * still the current picture, so do not paint the test pattern over it. */
  if (!acquired_dupl && p->dupl_has_frame && p->use_zcvid && p->zcvid_has_prev_src) {
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->cap_bgra_tex,
                                     (ID3D11Resource *)p->zcvid_prev_src_tex);
  } else if (!acquired_dupl && !p->dupl_has_frame) {
    if (p->ctx1) {
      float bg[4] = {0.10f, 0.12f, 0.16f, 1.0f};
      float hud[4] = {0.88f, 0.90f, 0.94f, 1.0f};
      float box_col[4] = {0.25f, 0.65f, 0.92f, 1.0f};
      if (p->use_zcvid || !p->fallback_bg_init) {
        ID3D11DeviceContext_ClearRenderTargetView(p->ctx, p->cap_rtv, bg);
        p->fallback_bg_init = true;
      } else if (frame_idx > 0) {
        uint32_t prev_x = ((frame_idx - 1U) * 4U) % (p->width > 340U ? p->width - 340U : 1U);
        D3D11_RECT erase_r = {(LONG)prev_x, (LONG)(p->height / 3U),
                              (LONG)(prev_x + 320U), (LONG)(p->height / 3U + 200U)};
        ID3D11DeviceContext1_ClearView(p->ctx1, (ID3D11View *)p->cap_rtv, bg, &erase_r, 1);
      }
      uint32_t cur_x = (frame_idx * 4U) % (p->width > 340U ? p->width - 340U : 1U);
      D3D11_RECT sprite_r = {(LONG)cur_x, (LONG)(p->height / 3U),
                             (LONG)(cur_x + 320U), (LONG)(p->height / 3U + 200U)};
      ID3D11DeviceContext1_ClearView(p->ctx1, (ID3D11View *)p->cap_rtv, box_col, &sprite_r, 1);

      D3D11_RECT hud_r = {40, (LONG)(p->height - 100U),
                          (LONG)(p->width > 560U ? 520U : p->width - 20U),
                          (LONG)(p->height - 40U)};
      ID3D11DeviceContext1_ClearView(p->ctx1, (ID3D11View *)p->cap_rtv, hud, &hud_r, 1);
    } else {
      float c = (float)(frame_idx % 120U) / 120.0f;
      float rgba[4] = {0.1f + 0.7f * c, 0.25f, 0.85f - 0.6f * c, 1.0f};
      ID3D11DeviceContext_ClearRenderTargetView(p->ctx, p->cap_rtv, rgba);
    }
  }

  if (!p->use_direct_nvenc && !p->use_zcvid && !p->use_pyrowave) {
    /* Zero-copy GPU BGRA -> NV12 conversion in VRAM (MFT fallback only) */
    vp_blit_textures(p, src_bgra, 0U, p->enc_nv12_tex);
  }

  p->zcvid_hints_static = (p->dupl != NULL) && p->dupl_has_frame && !acquired_dupl;
  if (acquired_dupl) {
    IDXGIResource_Release(dupl_res);
    IDXGIOutputDuplication_ReleaseFrame(p->dupl);
  }

  QueryPerformanceCounter(&t1);

#ifdef ZC_HAVE_PYROWAVE
  if (p->use_pyrowave) {
    uint32_t fps = (p->fps > 0U) ? p->fps : 60U;
    uint32_t target_bytes = (p->bitrate_bps > 0U) ? (p->bitrate_bps / (8U * fps)) : 52083U;
    if (target_bytes < 4096U) target_bytes = 4096U;
    if (target_bytes > (uint32_t)(p->enc_out_cap - 4096U)) {
      target_bytes = (uint32_t)(p->enc_out_cap - 4096U);
    }

    size_t out_bytes = 0;
    int erc = -1;
    if (zc_pyro_has_d3d11_enc_interop(p->pyro)) {
      ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->pyro_shared_bgra,
                                       (ID3D11Resource *)src_bgra);
      uint64_t wait_val = ++p->pyro_fence_val;
      uint64_t sig_val = ++p->pyro_fence_val;
      ID3D11DeviceContext4_Signal(p->pyro_ctx4, p->pyro_fence, wait_val);
      ID3D11DeviceContext_Flush(p->ctx);
      erc = zc_pyro_encode_gpu(p->pyro, wait_val, sig_val, target_bytes,
                               p->enc_out_buf, p->enc_out_cap, &out_bytes);
      ID3D11DeviceContext4_Wait(p->pyro_ctx4, p->pyro_fence, sig_val);
    } else if (p->pyro_nv12_stg && p->enc_nv12_tex) {
      /* The receiver's k_pyro_yuv2rgba_hlsl expects full-range BT.709, but the
       * VP default output is BT.601 with an undefined nominal range (washed-out
       * / tinted colours). Force full-range RGB -> full-range BT.709 NV12 for
       * this blit only, then restore the shared VP state for MFT/NVENC. */
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE prev_in_cs, prev_out_cs;
      ID3D11VideoContext_VideoProcessorGetStreamColorSpace(p->vctx, p->vp, 0, &prev_in_cs);
      ID3D11VideoContext_VideoProcessorGetOutputColorSpace(p->vctx, p->vp, &prev_out_cs);
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = {0};
      in_cs.RGB_Range = 0; /* 0-255 RGB source */
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {0};
      out_cs.YCbCr_Matrix = 1; /* BT.709 */
      out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(p->vctx, p->vp, 0, &in_cs);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(p->vctx, p->vp, &out_cs);
      vp_blit_textures(p, src_bgra, 0U, p->enc_nv12_tex);
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(p->vctx, p->vp, 0, &prev_in_cs);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(p->vctx, p->vp, &prev_out_cs);
      ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->pyro_nv12_stg,
                                       (ID3D11Resource *)p->enc_nv12_tex);
      D3D11_MAPPED_SUBRESOURCE mr = {0};
      if (SUCCEEDED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->pyro_nv12_stg,
                                            0, D3D11_MAP_READ, 0, &mr))) {
        const uint8_t *y_ptr = (const uint8_t *)mr.pData;
        const uint8_t *uv_ptr = y_ptr + (size_t)mr.RowPitch * (size_t)p->height;
        erc = zc_pyro_encode_nv12(p->pyro, y_ptr, mr.RowPitch, uv_ptr, mr.RowPitch,
                                  target_bytes, p->enc_out_buf, p->enc_out_cap, &out_bytes);
        ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->pyro_nv12_stg, 0);
      }
    }

    QueryPerformanceCounter(&t2);
    if (out_cap_us) *out_cap_us = qpc_delta_us(t0, t1);
    if (out_enc_us) *out_enc_us = qpc_delta_us(t1, t2);
    if (out_bitstream) *out_bitstream = p->enc_out_buf;
    if (out_len) *out_len = out_bytes;
    if (out_keyframe) *out_keyframe = true; /* Pyrowave is intra-only */
    return (erc == 0 && out_bytes > 0) ? 0 : -38;
  }
#endif

  if (p->use_zcvid) {
    uint32_t ping = p->zcvid_enc_ping & 1U;
    uint32_t pong = ping ^ 1U;
    p->zcvid_enc_ping = pong;

    UINT zero4[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->zcvid_out_counter_uav, zero4);

    uint32_t fps = (p->fps > 0U) ? p->fps : 60U;
    uint32_t target_bytes = (p->bitrate_bps > 0U) ? (p->bitrate_bps / (8U * fps)) : 52083U;
    uint32_t num_tiles = p->zcvid_tiles_x * p->zcvid_tiles_y;
    /* Packed, a record byte costs zcvid_pack_ratio_q16 / 65536 wire bytes and
     * the vectors what they cost last frame: the same wire budget buys that
     * many more records. */
    uint32_t mv_reserve = p->zcvid_packed ? (8U + p->zcvid_pack_mv_bytes) : (8U + num_tiles * 2U);
    if (mv_reserve > target_bytes / 2U) mv_reserve = target_bytes / 2U;
    uint32_t avail_bytes = (target_bytes > mv_reserve + 256U)
                               ? (target_bytes - mv_reserve)
                               : 256U;
    uint32_t target_words = avail_bytes / 4U;
    p->zcvid_ec_frame = false;
    if (p->zcvid_packed) {
      /* zcvid_ec_ratio_q16 is what a record byte really cost on the wire,
       * range-coded part and plain part together, so a frame larger than the
       * coder's time budget just sees its ratio drift toward the plain one. */
      uint32_t ratio = p->zcvid_ec ? p->zcvid_ec_ratio_q16 : p->zcvid_pack_ratio_q16;
      target_words = (uint32_t)(((uint64_t)target_words << 16) / ratio);
      p->zcvid_ec_frame = p->zcvid_ec;
    }
    /* 25 % of slack lets the controller absorb a frame of extra motion. A
     * scene it cannot bring under the budget even at its highest threshold
     * would otherwise sit on that cap for good, 25 % over the target.
     * On an abrupt scene cut (zcvid_cut_burst > 0), allow a short 2.5x VBV
     * burst (matching ZC_PACE_HEADROOM) so 4K cinema cuts refresh cleanly. */
    bool kf_resync = (p->zcvid_kf_countdown > 0U);
    bool run_tq = p->zcvid_tq;
    bool kf_burst = run_tq ? ((frame_idx == 0U) || kf_resync) : ((frame_idx < 16U) || kf_resync);
    uint32_t ecb_frame_id = frame_idx;
    if (p->zcvid_kf_countdown > 0U) {
      ecb_frame_id = run_tq ? 0U : (16U - p->zcvid_kf_countdown);
      p->zcvid_kf_countdown--;
    }

    bool at_high_qp = run_tq ? (p->zcvid_tq_qp >= 38U) : (p->zcvid_rc_threshold >= 14U);
    uint32_t hard_cap_words = (p->zcvid_cut_burst > 0U)
                                  ? ((target_words * 5U) / 2U) & ~1U
                                  : at_high_qp
                                        ? (target_words & ~1U)
                                        : ((target_words + (target_words >> 2)) & ~1U);
    if (p->zcvid_cut_burst > 0U) p->zcvid_cut_burst--;
    if (hard_cap_words < 256U) hard_cap_words = 256U;
    if (hard_cap_words > p->zcvid_max_record_words) hard_cap_words = p->zcvid_max_record_words & ~1U;

    /* Scale rolling intra-refresh cycles so it uses ~12.5% of target_words,
     * keeping 87.5% for real picture detail. */
    uint32_t intra_budget = target_words / 8U;
    uint32_t words_per_row = (p->width * 3U) / 5U;
    uint32_t rows_per_frame = (words_per_row > 0U && intra_budget >= words_per_row)
                                  ? (intra_budget / words_per_row)
                                  : 1U;
    uint32_t min_cycles = (fps > 60U) ? fps : 60U;
    uint32_t intra_cycles = (p->height + rows_per_frame - 1U) / rows_per_frame;
    if (run_tq) {
      /* CSIntraT handles rolling refresh on 8x8 superblock rows (self-contained
       * hybrid 8x8/4x4 intra): refresh 1 SB row (8 px) per frame at comfortable
       * bitrates (or slower under tight bitrates / high refresh). */
      uint32_t roll_bands = (((p->height + 3U) >> 2) + 1U) >> 1;
      uint32_t tq_min_cycles = (fps * 2U > roll_bands) ? (fps * 2U) : roll_bands;
      if (tq_min_cycles > min_cycles) min_cycles = tq_min_cycles;
    }
    if (intra_cycles < min_cycles) intra_cycles = min_cycles;
    if (intra_cycles > 600U) intra_cycles = 600U;
    if (kf_burst && !run_tq) {
      intra_cycles = 16U;
    } else if (!kf_burst && p->zcvid_static_scene) {
      uint32_t resync_mod = (fps > 0U) ? (fps * 3U) : 360U;
      if (p->zcvid_static_sweep > 0U) {
        intra_cycles = p->zcvid_static_sweep_cycles ? p->zcvid_static_sweep_cycles : 60U;
        p->zcvid_static_sweep--;
      } else if (((frame_idx - 16U) % resync_mod) < 30U) {
        intra_cycles = p->zcvid_static_sweep_cycles ? p->zcvid_static_sweep_cycles : 60U;
      } else {
        intra_cycles = (p->height > 720U) ? p->height : 720U;
      }
    }

    uint32_t mode_flags = run_tq ? 0U : (kf_burst ? 1U : (p->zcvid_rc_2x ? 1U : 0U));
    if (!run_tq && !kf_burst && p->zcvid_intra8) mode_flags |= 2U;
    if (run_tq) mode_flags |= 4U;
    if (run_tq && p->zcvid_tq_inter) mode_flags |= 8U;
    bool frame_qpel = p->zcvid_qpel;
    if (frame_qpel) mode_flags |= 16U;
    /* bit 5: allow GPU scene-cut burst (gated by cooldown so sustained fast
     * whips / noise storms cannot fire scene-cut burst every frame);
     * bit 7: single-frame pure transform keyframe / resync. */
    bool allow_cut_burst = run_tq && (p->zcvid_cut_cooldown == 0U);
    if (allow_cut_burst) mode_flags |= 32U;
    if (p->zcvid_dither) mode_flags |= 64U;
    if (run_tq && kf_burst) mode_flags |= 128U;
    uint32_t eff_blocks = ((mode_flags & 1U) != 0U) ? ((p->zcvid_total_blocks + 1U) >> 1)
                                             : p->zcvid_total_blocks;
    uint32_t burst_cap_words = (p->width * p->height) / 14U;
    if (burst_cap_words < 140000U) burst_cap_words = 140000U;
    uint32_t enc_cap_words = run_tq ? hard_cap_words : (kf_burst ? (burst_cap_words & ~1U) : hard_cap_words);
    if (enc_cap_words > (p->zcvid_max_record_words & ~1U)) {
      enc_cap_words = p->zcvid_max_record_words & ~1U;
    }
    uint32_t enc_thr = kf_burst ? 255U : p->zcvid_rc_threshold;

    /* Ultra-wide velocity-invariant motion search radius: inner +-60 px runs in
     * groupshared and conditionally expands to +-124 px/f (<=240 Hz) or +-80 px/f
     * (360/480 Hz = 38,400 px/s!) only on tiles with fast camera whips. */
    uint32_t dyn_search_r = (fps >= 360U) ? 80U : 124U;
    if (dyn_search_r > ZCVID_SEARCH_R) dyn_search_r = ZCVID_SEARCH_R;

    uint32_t cur_tq_qp = ZCVID_TQ_ADAPTIVE ? p->zcvid_tq_qp : (ZCVID_TQ_QP_BASE + ZCVID_TQ_QP_PER_THR * enc_thr);
    uint32_t cur_tq_cqp = (cur_tq_qp <= 28U) ? ZCVID_TQ_CQP : (ZCVID_TQ_CQP + 1U);
    uint32_t cur_tq_inter_thr = ZCVID_TQ_ADAPTIVE ? p->zcvid_tq_inter_thr
                                                 : (ZCVID_TQ_INTER_BASE + ZCVID_TQ_INTER_PER_THR * enc_thr);
    uint32_t cur_tq_hi_thr = 0U;
    uint32_t cur_ierr_thr = run_tq ? (24U + ((cur_tq_qp > 24U) ? ((cur_tq_qp - 24U) * 2U) : 0U))
                                   : (kf_burst ? 255U : p->zcvid_rc_intra_err_thr);
    if (run_tq && cur_ierr_thr > 56U) cur_ierr_thr = 56U;

    zcvid_enc_cb_t ecb = {
        p->width, p->height, enc_thr, p->zcvid_delta_min,
        p->zcvid_delta_max, p->zcvid_gamma_q8, p->zcvid_neutral_y, p->zcvid_total_blocks,
        ecb_frame_id, intra_cycles, cur_ierr_thr,
        p->zcvid_tiles_x, p->zcvid_tiles_y,
        (kf_resync || frame_idx == 0U) ? 0U : dyn_search_r, enc_cap_words, mode_flags,
        cur_tq_qp,
        {cur_tq_cqp, cur_tq_inter_thr, cur_tq_hi_thr}
    };
    ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->zcvid_enc_cb,
                                          0, NULL, &ecb, 0, 0);

    uint32_t half_cur = p->zcvid_half_ping & 1U;
    uint32_t half_prev = half_cur ^ 1U;
    p->zcvid_half_ping = half_prev;
    uint32_t mv_cur = p->zcvid_mv_ping & 1U;
    uint32_t mv_prev = mv_cur ^ 1U;
    p->zcvid_mv_ping = mv_prev;

    ID3D11ShaderResourceView *srvs[8] = {p->cap_bgra_srv, p->zcvid_enc_yuvd_srv[ping],
                                         p->zcvid_prev_src_srv, p->zcvid_half_srv[half_cur],
                                         p->zcvid_half_srv[half_prev], p->zcvid_enc_mvs_srv[mv_prev],
                                         p->zcvid_quarter_srv[half_cur],
                                         p->zcvid_quarter_srv[half_prev]};
    ID3D11UnorderedAccessView *uavs[4] = {
        p->zcvid_enc_yuvd_uav[pong], p->zcvid_enc_mvs_uav[mv_cur],
        p->zcvid_out_records_uav, p->zcvid_out_counter_uav
    };
    if (p->zcvid_prof_en) {
      ID3D11DeviceContext_Begin(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_disj);
      ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[0]);
    }
    ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->zcvid_enc_cb);
    /* Pass 0: CSLumaHalf + CSLumaQuarter (reduced luma of this frame for the motion search) */
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 1, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 4, 1, &p->zcvid_half_uav[half_cur], NULL);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_luma_half, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, ((p->width + 1U) / 2U + 15U) / 16U,
                                 ((p->height + 1U) / 2U + 15U) / 16U, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 4, 1, &p->zcvid_quarter_uav[half_cur], NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 3, 1, &srvs[3]);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_luma_quarter, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, ((p->width + 3U) / 4U + 15U) / 16U,
                                 ((p->height + 3U) / 4U + 15U) / 16U, 1);
    ID3D11UnorderedAccessView *no_uav = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 4, 1, &no_uav, NULL);
    if (p->zcvid_prof_en) ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[1]);

    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 8, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, uavs, NULL);

    /* Capture hints: this frame's move / dirty rectangles, every tile
     * unchanged when no new desktop frame came, nothing otherwise. Skip
     * redundant CPU fills + UpdateSubresource uploads when state is unchanged. */
    if (p->zcvid_hint_buf) {
      uint32_t nt = p->zcvid_tiles_x * p->zcvid_tiles_y;
      bool desk_static = !p->zcvid_hints_set && p->zcvid_hints_static;
      int want_state = p->zcvid_hints_set ? -1 : (desk_static ? 2 : 0);
      if (want_state == -1 || want_state != p->zcvid_hint_gpu_state) {
        if (!p->zcvid_hints_set) {
          for (uint32_t t = 0; t < nt; ++t) {
            p->zcvid_hint_cpu[t * 4U] = desk_static ? 2 : 0;
            p->zcvid_hint_cpu[t * 4U + 1U] = p->zcvid_hint_cpu[t * 4U + 2U] = 0;
            p->zcvid_hint_cpu[t * 4U + 3U] = 0;
          }
        }
        ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->zcvid_hint_buf, 0, NULL,
                                              p->zcvid_hint_cpu, 0, 0);
        p->zcvid_hint_gpu_state = want_state;
      }
      ID3D11DeviceContext_CSSetShaderResources(p->ctx, 9, 1, &p->zcvid_hint_srv);
      p->zcvid_hints_set = false;
    }
    /* Pass 1: CSTileMV (32x32 tiles) + Pass 1s: CSMvSmooth (spatial-median
     * coherence filter) - skip cleanly when search_r == 0. */
    if (ecb.search_r == 0U) {
      ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->zcvid_enc_mvs_uav[mv_cur], zero4);
    } else {
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_tile_mv, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, p->zcvid_tiles_x, p->zcvid_tiles_y, 1);

      /* Unbind u1 + t5 before ping-ponging into CSMvSmooth (zero D3D11 hazard). */
      ID3D11ShaderResourceView *no_srv = NULL;
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 1, 1, &no_uav, NULL);
      ID3D11DeviceContext_CSSetShaderResources(p->ctx, 5, 1, &no_srv);
      ID3D11DeviceContext_CSSetShaderResources(p->ctx, 5, 1, &p->zcvid_enc_mvs_srv[mv_cur]);
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 1, 1, &p->zcvid_enc_mvs_uav[mv_prev], NULL);
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_mv_smooth, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, p->zcvid_tiles_x, p->zcvid_tiles_y, 1);
      uint32_t tmp_mv = mv_cur;
      mv_cur = mv_prev;
      mv_prev = tmp_mv;
      p->zcvid_mv_ping = mv_prev;
    }
    if (p->zcvid_prof_en) ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[2]);

    /* Unbind g_tile_mvs UAV (u1) and bind current frame's MVs as read-only SRV (t5). */
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 1, 1, &no_uav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 5, 1, &p->zcvid_enc_mvs_srv[mv_cur]);

    /* Pass 1a: CSMotionPred computes FetchRefQ + LeakyStep ONCE per pixel into
     * zcvid_enc_pred_buf (via u5), pre-initializes g_cur_yuvd (u0), and clears
     * the 24-word transform histogram/misc tail in g_tq_scratch (u6) when run_tq. */
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 5, 1, &p->zcvid_enc_pred_uav, NULL);
    if (run_tq) {
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 6, 1, &p->zcvid_tq_scratch_uav, NULL);
    }
    ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_motion_pred, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U, (p->height + 15U) / 16U, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 5, 1, &no_uav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 10, 1, &p->zcvid_enc_pred_srv);
    if (p->zcvid_prof_en) ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[3]);

    /* Pass 1b: 16-bin global RDO 4x4/8x8 transform (CSTqClear folded into
     * CSMotionPred). Single unified transform codec: dispatch CSTqFinal
     * directly without running legacy CSEncode; keep CSEncode only if
     * zcvid_tq is explicitly disabled. */
    if (run_tq) {
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 5, 1, &p->zcvid_enc_tflag_uav, NULL);
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 7, 1, &p->zcvid_tq_stage_uav, NULL);
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_intra_t_count, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, (p->zcvid_tq_blocks + 63U) / 64U, 1, 1);
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_tq_mid, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, (p->zcvid_tq_blocks + 63U) / 64U, 1, 1);
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_tq_scan_a, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, 1, 1, 1);
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_intra_t, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, (p->zcvid_tq_blocks + 63U) / 64U, 1, 1);
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 7, 1, &no_uav, NULL);
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 5, 1, &no_uav, NULL);
      if (p->zcvid_prof_en) ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[4]);
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_tq_final, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, 1, 1, 1);
      if (p->zcvid_prof_en) ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[5]);
      ID3D11ShaderResourceView *null_t8910[3] = {NULL, NULL, NULL};
      ID3D11DeviceContext_CSSetShaderResources(p->ctx, 8, 3, null_t8910);
      ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 6, 1, &no_uav, NULL);
    } else {
      if (p->zcvid_prof_en) ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[4]);
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_encode, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, (eff_blocks + 63U) / 64U, 1, 1);
      if (p->zcvid_prof_en) ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[5]);
      ID3D11ShaderResourceView *null_t8910[3] = {NULL, NULL, NULL};
      ID3D11DeviceContext_CSSetShaderResources(p->ctx, 8, 3, null_t8910);
    }

    ID3D11ShaderResourceView *null_srv[8] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    ID3D11UnorderedAccessView *null_uav[4] = {NULL, NULL, NULL, NULL};
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 8, null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, null_uav, NULL);

    p->zcvid_has_prev_src = true;

    uint32_t stg_idx = p->zcvid_stg_idx % ZCVID_STG_RING;
    p->zcvid_stg_idx++;
    /* Copy records (up to adaptive high-water mark) -> MVs -> counter LAST so
     * one WDDM packet submits encode + DMA with minimal PCIe traffic; rare spikes
     * or scene cuts fetch only the remaining tail [dma_words .. record_words). */
    uint32_t spec_cap = kf_burst ? enc_cap_words : p->zcvid_dma_spec_words;
    if (spec_cap < 4096U) spec_cap = 4096U;
    uint32_t dma_words = (enc_cap_words < spec_cap) ? enc_cap_words : spec_cap;
    dma_words &= ~1U;
    if (dma_words > 0U) {
      D3D11_BOX box = {0, 0, 0, dma_words * (UINT)sizeof(uint32_t), 1, 1};
      ID3D11DeviceContext_CopySubresourceRegion(
          p->ctx, (ID3D11Resource *)p->zcvid_out_records_stg[stg_idx], 0, 0, 0, 0,
          (ID3D11Resource *)p->zcvid_out_records_buf, 0, &box);
    }
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->zcvid_enc_mvs_stg[stg_idx],
                                     (ID3D11Resource *)p->zcvid_enc_mvs_buf[mv_cur]);
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->zcvid_out_counter_stg[stg_idx],
                                     (ID3D11Resource *)p->zcvid_out_counter_buf);
    if (p->zcvid_prof_en) {
      ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[6]);
      ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_disj);
    }
    ID3D11DeviceContext_Flush(p->ctx);
    LARGE_INTEGER pf_w0, pf_w1, pf_mv1, pf_ec0, pf_ec1;
    if (p->zcvid_prof_en) QueryPerformanceCounter(&pf_w0);

    D3D11_MAPPED_SUBRESOURCE mr = {0};
    uint32_t enc_written = 0, tq_wanted = 0, cut_tiles = 0, tq_accepted = 0;
    if (SUCCEEDED(map_read_sync(p->ctx, (ID3D11Resource *)p->zcvid_out_counter_stg[stg_idx], &mr))) {
      enc_written = ((const uint32_t *)mr.pData)[0];
      tq_wanted = ((const uint32_t *)mr.pData)[1];
      cut_tiles = ((const uint32_t *)mr.pData)[2];
      tq_accepted = ((const uint32_t *)mr.pData)[3];
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->zcvid_out_counter_stg[stg_idx], 0);
    }
    uint32_t written_words = run_tq ? tq_accepted : enc_written;
    bool detected_cut = allow_cut_burst && (num_tiles > 0U) && (cut_tiles * 3U >= num_tiles);
    bool gpu_scene_cut = detected_cut || (run_tq && kf_burst);
    if (detected_cut) {
      p->zcvid_cut_cooldown = 8U;
    } else if (p->zcvid_cut_cooldown > 0U) {
      p->zcvid_cut_cooldown--;
    }
    uint32_t valid_cap = enc_cap_words;
    if (run_tq && tq_accepted > valid_cap) {
      /* GPU-detected abrupt scene cut expanded Frame-0 CSIntraT cap beyond
       * enc_cap_words: copy ONLY up to tq_accepted (exact words written by
       * CSIntraT), NEVER CSEncode's dropped overflow written_words, so we
       * never read stale VRAM from previous frames. */
      uint32_t cut_limit = p->zcvid_max_record_words & ~1U;
      uint32_t need_words = (tq_accepted > cut_limit) ? cut_limit : (tq_accepted & ~1U);
      if (need_words > valid_cap) valid_cap = need_words;
    }
    uint32_t record_words = (written_words > valid_cap) ? valid_cap : written_words;
    record_words &= ~1U;
    if (record_words > dma_words) {
      D3D11_BOX tail_box = {dma_words * (UINT)sizeof(uint32_t), 0, 0,
                            record_words * (UINT)sizeof(uint32_t), 1, 1};
      ID3D11DeviceContext_CopySubresourceRegion(
          p->ctx, (ID3D11Resource *)p->zcvid_out_records_stg[stg_idx], 0,
          dma_words * (UINT)sizeof(uint32_t), 0, 0,
          (ID3D11Resource *)p->zcvid_out_records_buf, 0, &tail_box);
      ID3D11DeviceContext_Flush(p->ctx);
    }
    if (!kf_burst) {
      uint32_t want_spec = record_words + (record_words >> 1) + 4096U;
      if (want_spec < 4096U) want_spec = 4096U;
      if (want_spec > p->zcvid_max_record_words) want_spec = p->zcvid_max_record_words;
      if (want_spec > p->zcvid_dma_spec_words) {
        p->zcvid_dma_spec_words = want_spec;
      } else {
        p->zcvid_dma_spec_words = (p->zcvid_dma_spec_words * 15U + want_spec) / 16U;
      }
    }
    if (p->zcvid_prof_en) QueryPerformanceCounter(&pf_w1);
    p->zcvid_last_raw_words = record_words;
    /* Total demand seen by the rate controller: max of written and transform demand. */
    uint32_t raw_words = (tq_wanted > written_words) ? tq_wanted : written_words;


    /* Compact MVs: 0 bytes if all (0,0); median-DPCM + range coder (ZCVID_EC_MV_FLAG)
     * when smaller than RLE, else int8 runs (<=31.75 px qpel) or int16 runs
     * (ZCVID_WIDE_MV_FLAG, up to +-63.75 px) preserving quarter-pel accuracy everywhere. */
    uint32_t mv_bytes = 0U;
    bool wide_mv = false;
    bool ec_mv = false;
    if (SUCCEEDED(map_read_sync(p->ctx, (ID3D11Resource *)p->zcvid_enc_mvs_stg[stg_idx], &mr))) {
      const int32_t *mvs = (const int32_t *)mr.pData;
      int32_t max_mv = 0;
      for (uint32_t i = 0; i < num_tiles; ++i) {
        int32_t ax = (mvs[i * 2U] < 0) ? -mvs[i * 2U] : mvs[i * 2U];
        int32_t ay = (mvs[i * 2U + 1U] < 0) ? -mvs[i * 2U + 1U] : mvs[i * 2U + 1U];
        int32_t m = (ax > ay) ? ax : ay;
        if (m > max_mv) max_mv = m;
      }
      bool any_nz = (max_mv != 0);
      wide_mv = (max_mv > 127);
      if (p->zcvid_packed) {
        if (any_nz) {
          mv_bytes = zcvid_pack_mvs(mvs, num_tiles, p->enc_out_buf + 8U, wide_mv);
          if (frame_qpel && mv_bytes > 0U) p->enc_out_buf[8U + mv_bytes++] = 0x51U;
          if (p->zcvid_ec && mv_bytes > 16U && max_mv <= 512) {
            uint8_t *ec_tmp = p->enc_out_buf + 8U + mv_bytes;
            size_t ec_cap = (p->enc_out_cap > 8U + mv_bytes) ? (p->enc_out_cap - 8U - mv_bytes) : 0U;
            if (ec_cap > mv_bytes) ec_cap = mv_bytes;
            uint32_t ec_bytes = zcvid_ec_pack_mvs(mvs, p->zcvid_tiles_x, p->zcvid_tiles_y,
                                                 frame_qpel, ec_tmp, ec_cap);
            if (ec_bytes > 0U && ec_bytes < mv_bytes) {
              memmove(p->enc_out_buf + 8U, ec_tmp, ec_bytes);
              mv_bytes = ec_bytes;
              ec_mv = true;
            }
          }
        }
        p->zcvid_pack_mv_bytes = gpu_scene_cut ? 0U : mv_bytes;
      } else if (any_nz) {
        if (wide_mv) {
          mv_bytes = num_tiles * 4U;
          int16_t *dst_mv = (int16_t *)(p->enc_out_buf + 8U);
          for (uint32_t i = 0; i < num_tiles; ++i) {
            dst_mv[i * 2U + 0U] = (int16_t)mvs[i * 2U + 0U];
            dst_mv[i * 2U + 1U] = (int16_t)mvs[i * 2U + 1U];
          }
        } else {
          mv_bytes = num_tiles * 2U;
          int8_t *dst_mv = (int8_t *)(p->enc_out_buf + 8U);
          for (uint32_t i = 0; i < num_tiles; ++i) {
            dst_mv[i * 2U + 0U] = (int8_t)mvs[i * 2U + 0U];
            dst_mv[i * 2U + 1U] = (int8_t)mvs[i * 2U + 1U];
          }
        }
        if (frame_qpel) p->enc_out_buf[8U + mv_bytes++] = 0x51U;
      }
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->zcvid_enc_mvs_stg[stg_idx], 0);
      if (gpu_scene_cut) {
        ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->zcvid_enc_mvs_uav[mv_cur], zero4);
      }
    }
    uint32_t hdr_word = p->zcvid_packed ? (ZCVID_PACKED_FLAG | record_words) : record_words;
    p->zcvid_ncls = p->zcvid_tq ? 9U : 8U;
    if (p->zcvid_packed && p->zcvid_tq) hdr_word |= ZCVID_V2_FLAG;
    if (p->zcvid_packed && ec_mv && mv_bytes > 0U) {
      hdr_word |= ZCVID_EC_MV_FLAG;
    } else if (p->zcvid_packed && wide_mv && mv_bytes > 0U) {
      hdr_word |= ZCVID_WIDE_MV_FLAG;
    }
    memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
    memcpy(p->enc_out_buf + sizeof(uint32_t), &mv_bytes, sizeof(uint32_t));

    /* Closed-loop frame rate controller (skip initial 16-frame convergence burst) */
    if (mv_bytes != 0U || raw_words > target_words) {
      p->zcvid_static_scene = false;
      p->zcvid_static_sweep = 0U;
    } else if (raw_words < (target_words / 2U)) {
      if (!p->zcvid_static_scene) {
        uint32_t w_safe = p->width ? p->width : 1U;
        uint32_t max_rows = (target_words / 2U) / w_safe;
        uint32_t max_rows_cap = 24000U / w_safe;
        if (max_rows_cap < 4U) max_rows_cap = 4U;
        if (max_rows > max_rows_cap) max_rows = max_rows_cap;
        if (max_rows < 1U) max_rows = 1U;
        uint32_t sc = (p->height + max_rows - 1U) / max_rows;
        if (sc < 60U) sc = 60U;
        if (sc > 240U) sc = 240U;
        p->zcvid_static_sweep_cycles = sc;
        p->zcvid_static_sweep = sc;
      }
      p->zcvid_static_scene = true;
    }
    if (!kf_burst) {
      /* Separated transform QP loop driven by actual transform demand (tq_wanted):
       * adapts smoothly from QP 6 (near-lossless 4K cinema polish) up to QP 36
       * under normal motion, and handles GPU-detected scene cuts on Frame 0 so
       * Frame 1 refines smoothly within the normal single-frame budget. */
      if (gpu_scene_cut) {
        /* Mirror HLSL TqSceneCutQp() on ecb.max_record_words and step down
         * gently on f+1 so post-cut frames refine smoothly without triggering
         * a high-demand QP spike on low-bitrate 4K60. */
        uint32_t nb = p->zcvid_tq_blocks;
        uint32_t mrw = ecb.max_record_words;
        uint32_t cut_qp = (mrw == 0U || mrw >= nb * 3U) ? 28U
                        : (mrw >= nb) ? 30U
                        : (mrw * 3U >= nb) ? 36U : 38U;
        uint32_t next_qp = (cut_qp >= 36U) ? (cut_qp - 2U)
                         : (cut_qp > 30U) ? (cut_qp - 4U)
                         : (cut_qp > 24U ? cut_qp - 3U : cut_qp);
        if (p->zcvid_tq_qp < next_qp) p->zcvid_tq_qp = next_qp;
        if (p->zcvid_tq_qp > cut_qp) p->zcvid_tq_qp = cut_qp;
        p->zcvid_cut_burst = 0U;
      } else if (tq_wanted > target_words * 3U) {
        bool scene_cut = (p->zcvid_cut_cooldown == 0U) &&
                         (p->zcvid_tq_qp >= 22U || (num_tiles > 0U && cut_tiles * 6U >= num_tiles)) &&
                         (tq_wanted > p->zcvid_tq_blocks * 6U && tq_wanted > target_words * 6U);
        if (scene_cut) {
          uint32_t next_qp = p->zcvid_tq_qp + 6U;
          if (next_qp < 34U) next_qp = 34U;
          if (next_qp > 40U) next_qp = 40U;
          if (next_qp > p->zcvid_tq_qp) p->zcvid_tq_qp = next_qp;
          if (p->zcvid_cut_burst == 0U) p->zcvid_cut_burst = 2U;
          p->zcvid_cut_cooldown = 8U;
        } else if (tq_wanted > target_words * 12U) {
          /* Severe bitrate starvation (e.g. fast non-translational motion): ramp
           * smoothly up to QP 48 so 8x8 superblocks cover the frame cleanly instead
           * of dropping blocks and leaving speckled noise. */
          uint32_t next_qp = p->zcvid_tq_qp + 5U;
          if (next_qp > 48U) next_qp = 48U;
          if (next_qp > p->zcvid_tq_qp) p->zcvid_tq_qp = next_qp;
        } else if (tq_wanted > target_words * 6U) {
          uint32_t next_qp = p->zcvid_tq_qp + 3U;
          if (next_qp > 44U) next_qp = 44U;
          if (next_qp > p->zcvid_tq_qp) p->zcvid_tq_qp = next_qp;
        } else if (p->zcvid_tq_qp < 40U) {
          uint32_t next_qp = p->zcvid_tq_qp + 2U;
          if (next_qp > 40U) next_qp = 40U;
          p->zcvid_tq_qp = next_qp;
        }
      } else if (p->zcvid_tq_qp > 34U && tq_wanted <= target_words * 2U) {
        p->zcvid_tq_qp = (p->zcvid_tq_qp >= 36U) ? (p->zcvid_tq_qp - 2U) : 34U;
      } else if (tq_wanted < (target_words * 3U) / 2U && written_words <= target_words) {
        if (p->zcvid_tq_qp > 34U) {
          p->zcvid_tq_qp = (p->zcvid_tq_qp >= 36U) ? (p->zcvid_tq_qp - 2U) : 34U;
        } else if (p->zcvid_tq_qp > ZCVID_TQ_QP_MIN) {
          p->zcvid_tq_qp--;
        }
      }
      p->zcvid_tq_inter_thr = (p->zcvid_tq_qp >= 20U) ? ((p->zcvid_tq_qp - 18U) / 4U) : 0U;
    }
    if (raw_words >= (target_words >> 1)) p->zcvid_rc_2x_calm = 0U;
    if (!kf_burst && !run_tq) {
      /* Legacy CSEncode rate-control loop (active only when zcvid_tq is disabled). */
      if (raw_words < (target_words * 3U) / 4U) {
        p->zcvid_intra8 = true;
      } else if (raw_words > (target_words * 5U) / 4U) {
        p->zcvid_intra8 = false;
      }
      if (raw_words > target_words + (target_words >> 3)) {
        if (!p->zcvid_rc_2x && !ZCVID_RC_NO_2X && raw_words > target_words * 2U) {
          p->zcvid_rc_2x = true;
        }
        if (p->zcvid_rc_threshold < 14U) {
          uint32_t step = (raw_words > target_words * 2U) ? 2U : 1U;
          p->zcvid_rc_threshold += step;
          if (p->zcvid_rc_threshold > 14U) p->zcvid_rc_threshold = 14U;
        }
        if (p->zcvid_rc_intra_err_thr < 56U) {
          p->zcvid_rc_intra_err_thr += 8U;
          if (p->zcvid_rc_intra_err_thr > 56U) p->zcvid_rc_intra_err_thr = 56U;
        }
      } else if (raw_words < (target_words * 3U) / 4U) {
        uint32_t thr_floor = (raw_words < (target_words * 2U) / 5U) ? ZCVID_RC_MIN_THRESHOLD
                           : (raw_words < (target_words * 3U) / 5U) ? ZCVID_RC_MIN_THRESHOLD + 1U
                                                                     : p->zcvid_threshold;
        if (thr_floor > p->zcvid_threshold) thr_floor = p->zcvid_threshold;
        uint32_t ierr_floor = (raw_words < (target_words * 3U) / 5U) ? ZCVID_RC_MIN_INTRA_ERR : 32U;
        if (p->zcvid_rc_threshold > thr_floor) {
          p->zcvid_rc_threshold--;
        }
        if (p->zcvid_rc_intra_err_thr > ierr_floor) {
          p->zcvid_rc_intra_err_thr -= 4U;
          if (p->zcvid_rc_intra_err_thr < ierr_floor) p->zcvid_rc_intra_err_thr = ierr_floor;
        }
        if (p->zcvid_rc_2x && raw_words < (target_words >> 1)) {
          if (++p->zcvid_rc_2x_calm >= ZCVID_RC_2X_CALM) {
            p->zcvid_rc_2x = false;
            p->zcvid_rc_2x_calm = 0U;
          }
        }
      }
    }

    if (p->zcvid_prof_en) QueryPerformanceCounter(&pf_mv1);
    size_t out_bytes = 8U + mv_bytes;
    if (p->zcvid_prof_en) QueryPerformanceCounter(&pf_ec0);
    if (record_words > 0) {
      if (SUCCEEDED(map_read_sync(p->ctx, (ID3D11Resource *)p->zcvid_out_records_stg[stg_idx], &mr))) {
        size_t rec_bytes = (size_t)record_words * sizeof(uint32_t);
        size_t packed = 0;
        bool coded = false;
        bool fits = p->zcvid_packed &&
                    out_bytes + zcvid_pack_bound(record_words / 2U) <= p->enc_out_cap;
        if (fits && p->zcvid_ec) {
          /* As many records as this side and the decoder have time for. On a
           * GPU-detected scene cut, range-code up to record_words (clamped by
           * peer_limit if negotiated) because class-8 I-frame DC/AC records
           * range-code ~6-8x smaller than plain-packed (preventing 400+ KB
           * plain-packed wire bursts at 240/480 Hz). */
          uint32_t budget = zcvid_ec_budget_words(p, p->zcvid_ec_enc_nsw_q8);
          if (gpu_scene_cut && budget < record_words) {
            budget = record_words & ~1U;
          }
          if (p->zcvid_ec_peer_limit != 0U && budget > p->zcvid_ec_peer_limit) {
            budget = p->zcvid_ec_peer_limit;
          }
          uint32_t ec_words = (record_words < budget) ? record_words : budget;
          LARGE_INTEGER q0, q1;
          QueryPerformanceCounter(&q0);
          packed = zcvid_ec_pack_records(p, (const uint32_t *)mr.pData, record_words / 2U,
                                        ec_words / 2U, p->enc_out_buf + out_bytes,
                                        p->enc_out_cap - out_bytes);
          QueryPerformanceCounter(&q1);
          coded = (packed > 0);
          if (coded) zcvid_ec_note_time(&p->zcvid_ec_enc_nsw_q8, q0, q1, ec_words);
        }
        if (fits && !coded) {
          packed = zcvid_pack_records(p, (const uint32_t *)mr.pData, record_words / 2U,
                                     p->enc_out_buf + out_bytes);
        }
        if (packed > 0) {
          out_bytes += packed;
          /* The header announces the records actually packed: slots the
           * encoder left unused are not part of the stream. */
          hdr_word = (hdr_word & ~ZCVID_RECORD_WORDS_MASK) | (p->zcvid_sort_kept * 2U);
          memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
          uint32_t ratio = (uint32_t)(((uint64_t)packed << 16) / rec_bytes);
          if (ratio > 65536U) ratio = 65536U;
          if (coded) {
            hdr_word |= ZCVID_EC_FLAG;
            if (p->zcvid_tq) hdr_word |= ZCVID_TQ_V3_FLAG;
            memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
            if (ratio < 4096U) ratio = 4096U;
            /* Burst and scene-cut frames are not what the controller is sizing. */
            if (!kf_burst && !gpu_scene_cut) p->zcvid_ec_ratio_q16 = (p->zcvid_ec_ratio_q16 * 7U + ratio) / 8U;
          } else {
            if (ratio < 8192U) ratio = 8192U;
            p->zcvid_pack_ratio_q16 = (p->zcvid_pack_ratio_q16 * 7U + ratio) / 8U;
          }
        } else {
          if (p->zcvid_packed) { /* sort scratch allocation failed: raw records */
            hdr_word |= ZCVID_RAW_RECORDS_FLAG;
            memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
          }
          memcpy(p->enc_out_buf + out_bytes, mr.pData, rec_bytes);
          out_bytes += rec_bytes;
        }
        ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->zcvid_out_records_stg[stg_idx], 0);
      }
    }

    QueryPerformanceCounter(&t2);
    if (p->zcvid_prof_en && frame_idx >= 30U) {
      pf_ec1 = t2;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disj = {0};
      while (ID3D11DeviceContext_GetData(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_disj,
                                         &disj, sizeof(disj), 0) == S_FALSE) YieldProcessor();
      if (!disj.Disjoint && disj.Frequency > 0ULL) {
        UINT64 ts[7] = {0};
        for (int i = 0; i < 7; ++i) {
          ID3D11DeviceContext_GetData(p->ctx, (ID3D11Asynchronous *)p->zcvid_prof_ts[i],
                                      &ts[i], sizeof(UINT64), 0);
        }
        double inv = 1e6 / (double)disj.Frequency;
        for (int i = 0; i < 6; ++i) p->zcvid_prof_acc[i] += (double)(ts[i + 1] - ts[i]) * inv;
        p->zcvid_prof_acc[6] += qpc_delta_us(pf_w0, pf_w1);
        p->zcvid_prof_acc[7] += qpc_delta_us(pf_w1, pf_mv1);
        p->zcvid_prof_acc[8] += qpc_delta_us(pf_ec0, pf_ec1);
        double tot = qpc_delta_us(t1, t2);
        p->zcvid_prof_acc[9] += tot;
        if (tot > p->zcvid_prof_acc[10]) p->zcvid_prof_acc[10] = tot;
        p->zcvid_prof_count++;
      }
    }
    if (out_cap_us) *out_cap_us = qpc_delta_us(t0, t1);
    if (out_enc_us) *out_enc_us = qpc_delta_us(t1, t2);
    if (out_bitstream) *out_bitstream = p->enc_out_buf;
    if (out_len) *out_len = out_bytes;
    if (out_keyframe) *out_keyframe = kf_burst || gpu_scene_cut || (fps > 0U && (frame_idx % fps) == 0U);
    return 0;
  }

#ifdef ZC_HAVE_NVENC
  if (p->use_direct_nvenc) {
    NV_ENC_MAP_INPUT_RESOURCE map = {0};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = p->nvenc_reg_res;
    if (p->nvenc_fn.nvEncMapInputResource(p->nvenc_session, &map) != NV_ENC_SUCCESS) {
      return -35;
    }

    NV_ENC_PIC_PARAMS pic = {0};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = p->width;
    pic.inputHeight = p->height;
    pic.inputPitch = p->width;
    pic.inputBuffer = map.mappedResource;
    pic.outputBitstream = p->nvenc_bitstream;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = frame_idx;
    if (frame_idx == 0) {
      pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    }

    NVENCSTATUS est = p->nvenc_fn.nvEncEncodePicture(p->nvenc_session, &pic);
    p->nvenc_fn.nvEncUnmapInputResource(p->nvenc_session, map.mappedResource);
    if (est != NV_ENC_SUCCESS) {
      return -36;
    }

    NV_ENC_LOCK_BITSTREAM lck = {0};
    lck.version = NV_ENC_LOCK_BITSTREAM_VER;
    lck.outputBitstream = p->nvenc_bitstream;
    if (p->nvenc_fn.nvEncLockBitstream(p->nvenc_session, &lck) != NV_ENC_SUCCESS) {
      return -37;
    }

    size_t copy_len = (lck.bitstreamSizeInBytes <= p->enc_out_cap)
                          ? lck.bitstreamSizeInBytes
                          : p->enc_out_cap;
    memcpy(p->enc_out_buf, lck.bitstreamBufferPtr, copy_len);
    bool keyframe = (lck.pictureType == NV_ENC_PIC_TYPE_IDR ||
                     lck.pictureType == NV_ENC_PIC_TYPE_I);
    p->nvenc_fn.nvEncUnlockBitstream(p->nvenc_session, p->nvenc_bitstream);

    QueryPerformanceCounter(&t2);
    if (out_cap_us) *out_cap_us = qpc_delta_us(t0, t1);
    if (out_enc_us) *out_enc_us = qpc_delta_us(t1, t2);
    if (out_bitstream) *out_bitstream = p->enc_out_buf;
    if (out_len) *out_len = copy_len;
    if (out_keyframe) *out_keyframe = keyframe;
    return (copy_len > 0) ? 0 : 1;
  }
#endif

  /* Wrap VRAM NV12 texture into IMFSample (zero-copy) and encode */
  IMFMediaBuffer *in_buf = NULL;
  HRESULT hr = MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D,
                                         (IUnknown *)p->enc_nv12_tex, 0, FALSE,
                                         &in_buf);
  if (FAILED(hr)) return -30;

  IMFSample *in_sample = NULL;
  MFCreateSample(&in_sample);
  IMFSample_AddBuffer(in_sample, in_buf);
  LONGLONG pts = (LONGLONG)frame_idx * (10000000LL / (LONGLONG)p->fps);
  IMFSample_SetSampleTime(in_sample, pts);
  IMFSample_SetSampleDuration(in_sample, 10000000LL / (LONGLONG)p->fps);

  /* Wait for METransformNeedInput if async MFT */
  if (p->enc_ev) {
    for (int ev_try = 0; ev_try < 32; ++ev_try) {
      IMFMediaEvent *ev = NULL;
      hr = IMFMediaEventGenerator_GetEvent(p->enc_ev, 0, &ev);
      if (FAILED(hr) || !ev) break;
      MediaEventType met = 0;
      IMFMediaEvent_GetType(ev, &met);
      IMFMediaEvent_Release(ev);
      if (met == METransformNeedInput) break;
    }
  }

  hr = IMFTransform_ProcessInput(p->enc_mft, 0, in_sample, 0);
  IMFSample_Release(in_sample);
  IMFMediaBuffer_Release(in_buf);
  if (FAILED(hr)) {
    fprintf(stderr, "enc ProcessInput failed: 0x%08lx\n", (unsigned long)hr);
    return -31;
  }

  if (p->enc_ev) {
    for (int ev_try = 0; ev_try < 32; ++ev_try) {
      IMFMediaEvent *ev = NULL;
      hr = IMFMediaEventGenerator_GetEvent(p->enc_ev, 0, &ev);
      if (FAILED(hr) || !ev) break;
      MediaEventType met = 0;
      IMFMediaEvent_GetType(ev, &met);
      IMFMediaEvent_Release(ev);
      if (met == METransformHaveOutput) break;
    }
  }

  MFT_OUTPUT_DATA_BUFFER out_data = {0};
  MFT_OUTPUT_STREAM_INFO osi = {0};
  IMFTransform_GetOutputStreamInfo(p->enc_mft, 0, &osi);
  bool mft_allocs = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

  IMFSample *out_sample = NULL;
  if (!mft_allocs) {
    IMFMediaBuffer *ob = NULL;
    MFCreateMemoryBuffer(osi.cbSize ? osi.cbSize : 1024 * 1024, &ob);
    MFCreateSample(&out_sample);
    IMFSample_AddBuffer(out_sample, ob);
    IMFMediaBuffer_Release(ob);
    out_data.pSample = out_sample;
  }

  DWORD status = 0;
  size_t total_out = 0;
  bool keyframe = false;

  for (int tries = 0; tries < 8; ++tries) {
    hr = IMFTransform_ProcessOutput(p->enc_mft, 0, 1, &out_data, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      break;
    }
    if (SUCCEEDED(hr) && out_data.pSample) {
      UINT32 clean = 0;
      if (SUCCEEDED(IMFSample_GetUINT32(out_data.pSample, &MFSampleExtension_CleanPoint,
                                        &clean)) && clean) {
        keyframe = true;
      }
      IMFMediaBuffer *cb = NULL;
      if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(out_data.pSample, &cb))) {
        BYTE *ptr = NULL;
        DWORD cur_len = 0;
        if (SUCCEEDED(IMFMediaBuffer_Lock(cb, &ptr, NULL, &cur_len))) {
          if (total_out + cur_len <= p->enc_out_cap) {
            memcpy(p->enc_out_buf + total_out, ptr, cur_len);
            total_out += cur_len;
          }
          IMFMediaBuffer_Unlock(cb);
        }
        IMFMediaBuffer_Release(cb);
      }
      if (mft_allocs) {
        IMFSample_Release(out_data.pSample);
        out_data.pSample = NULL;
      }
      break;
    }
  }
  if (out_sample) IMFSample_Release(out_sample);

  QueryPerformanceCounter(&t2);
  if (out_cap_us) *out_cap_us = qpc_delta_us(t0, t1);
  if (out_enc_us) *out_enc_us = qpc_delta_us(t1, t2);
  if (out_bitstream) *out_bitstream = p->enc_out_buf;
  if (out_len) *out_len = total_out;
  if (out_keyframe) *out_keyframe = keyframe;
  return (total_out > 0) ? 0 : 1; /* 1 = encoder warming up */
}

int gpu_pipeline_decode_and_present(gpu_pipeline_t *p,
                                    const uint8_t *bitstream, size_t len,
                                    double *out_dec_us,
                                    double *out_present_us) {
  if (!bitstream || len == 0) return -1;
  LARGE_INTEGER t0, t1, t2;
  QueryPerformanceCounter(&t0);

#ifdef ZC_HAVE_PYROWAVE
  if (p->use_pyrowave) {
    const uint32_t w = p->width;
    const uint32_t h = p->height;
    const uint32_t cw = w >> 1;
    const uint32_t ch = h >> 1;
    ID3D11ShaderResourceView *srvs[3] = {NULL, NULL, NULL};

    if (zc_pyro_has_d3d11_dec_interop(p->pyro)) {
      uint64_t wait_val = ++p->pyro_fence_val;
      uint64_t sig_val = ++p->pyro_fence_val;
      ID3D11DeviceContext4_Signal(p->pyro_ctx4, p->pyro_fence, wait_val);
      ID3D11DeviceContext_Flush(p->ctx);
      int drc = zc_pyro_decode_gpu(p->pyro, bitstream, len, wait_val, sig_val);
      if (drc != 0) return -45;
      ID3D11DeviceContext4_Wait(p->pyro_ctx4, p->pyro_fence, sig_val);
      srvs[0] = p->pyro_dec_shared_srv[0];
      srvs[1] = p->pyro_dec_shared_srv[1];
      srvs[2] = p->pyro_dec_shared_srv[2];
    } else {
      int drc = zc_pyro_decode_yuv420p(p->pyro, bitstream, len,
                                       p->pyro_dec_y, w,
                                       p->pyro_dec_cb, cw,
                                       p->pyro_dec_cr, cw);
      if (drc != 0) return -45;

      if (upload_r8_plane(p->ctx, p->pyro_y_tex, p->pyro_dec_y, w, h) != 0 ||
          upload_r8_plane(p->ctx, p->pyro_cb_tex, p->pyro_dec_cb, cw, ch) != 0 ||
          upload_r8_plane(p->ctx, p->pyro_cr_tex, p->pyro_dec_cr, cw, ch) != 0) {
        return -46;
      }
      srvs[0] = p->pyro_y_srv;
      srvs[1] = p->pyro_cb_srv;
      srvs[2] = p->pyro_cr_srv;
    }

    ID3D11UnorderedAccessView *uav0 = p->pyro_dec_uav;
    ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->pyro_dec_cbuf);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 1, &uav0, NULL);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->pyro_cs_yuv2rgba, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (w + 15U) / 16U, (h + 15U) / 16U, 1);

    ID3D11ShaderResourceView *null_srv[3] = {NULL, NULL, NULL};
    ID3D11UnorderedAccessView *null_uav = NULL;
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 1, &null_uav, NULL);

    QueryPerformanceCounter(&t1);

    if (p->show_window) {
      ID3D11Texture2D *backbuf = NULL;
      if (SUCCEEDED(IDXGISwapChain1_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                              (void **)&backbuf))) {
        if (p->pyro_dec_use_copy) {
          ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)backbuf,
                                           (ID3D11Resource *)p->pyro_dec_tex);
        } else {
          (void)vp_blit_textures(p, p->pyro_dec_tex, 0U, backbuf);
        }
        ID3D11Texture2D_Release(backbuf);
      }
      UINT present_flags = p->tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
      IDXGISwapChain1_Present(p->swapchain, 0, present_flags);
    } else {
      ID3D11DeviceContext_Flush(p->ctx);
    }

    QueryPerformanceCounter(&t2);
    if (out_dec_us) *out_dec_us = qpc_delta_us(t0, t1);
    if (out_present_us) *out_present_us = qpc_delta_us(t1, t2);
    return 0;
  }
#endif

  if (p->use_zcvid) {
    if (len < 8U) return -1;
    uint32_t hdr_word = 0, record_words = 0, mv_bytes = 0;
    memcpy(&hdr_word, bitstream, sizeof(uint32_t));
    memcpy(&mv_bytes, bitstream + sizeof(uint32_t), sizeof(uint32_t));
    bool packed = (hdr_word & ZCVID_PACKED_FLAG) != 0U;
    bool packed_records = packed && (hdr_word & ZCVID_RAW_RECORDS_FLAG) == 0U;
    record_words = packed ? (hdr_word & ZCVID_RECORD_WORDS_MASK) : hdr_word;
    const uint32_t ncls = (packed && (hdr_word & ZCVID_V2_FLAG) != 0U) ? 9U : 8U;
    bool mv_qpel = false;
    if (record_words > p->zcvid_max_record_words) record_words = p->zcvid_max_record_words;
    if (mv_bytes > len - 8U) return -1;
    if (!packed_records &&
        len < 8U + (size_t)mv_bytes + (size_t)record_words * sizeof(uint32_t)) return -1;

    uint32_t num_tiles = p->zcvid_tiles_x * p->zcvid_tiles_y;
    if (num_tiles > p->zcvid_dec_mv_cap) {
      if (p->zcvid_dec_mv_cap != 0U) zcvid_note_late_alloc("decoder vectors", (size_t)num_tiles * 8U);
      int32_t *nb = (int32_t *)realloc(p->zcvid_dec_mv_scratch,
                                       (size_t)num_tiles * 2U * sizeof(int32_t));
      if (!nb) return -1;
      p->zcvid_dec_mv_scratch = nb;
      p->zcvid_dec_mv_cap = num_tiles;
    }
    const void *mv_src = NULL;
    uint32_t mv_copy_bytes = num_tiles * 8U;
    bool ec_mv = packed && ((hdr_word & ZCVID_EC_MV_FLAG) != 0U);
    bool wide_mv = packed ? ((hdr_word & ZCVID_WIDE_MV_FLAG) != 0U)
                          : (mv_bytes == num_tiles * 4U || mv_bytes == num_tiles * 4U + 1U);
    bool overlap_mv = (mv_bytes > 0U) && ec_mv && (record_words > 0U) &&
                      packed_records && ((hdr_word & ZCVID_EC_FLAG) != 0U);
    zcvid_mv_dec_job_t mv_job;
    if (overlap_mv) {
      mv_job.in = bitstream + 8U;
      mv_job.len = mv_bytes;
      mv_job.dst = p->zcvid_dec_mv_scratch;
      mv_job.tiles_x = p->zcvid_tiles_x;
      mv_job.tiles_y = p->zcvid_tiles_y;
      mv_job.qpel = false;
      mv_job.ok = false;
    }
    LARGE_INTEGER dp0 = t0, dp1 = t0, dp2 = t0, dp3 = t0, dp4 = t0;
    if (p->zcvid_prof_en) QueryPerformanceCounter(&dp0);
    if (overlap_mv) {
      mv_src = p->zcvid_dec_mv_scratch;
    } else if (mv_bytes == 0U) {
      mv_src = NULL;
      mv_qpel = false;
    } else if (ec_mv) {
      if (!zcvid_ec_unpack_mvs(bitstream + 8U, mv_bytes, p->zcvid_dec_mv_scratch,
                              p->zcvid_tiles_x, p->zcvid_tiles_y, &mv_qpel)) {
        return -1;
      }
      mv_src = p->zcvid_dec_mv_scratch;
    } else {
      /* Quarter-pel vectors are followed by one marker byte (0x51):
       * packed: runs of 3 (narrow) or 5 (wide) bytes + 1; raw: 2 or 4 bytes/tile + 1. */
      uint32_t run_step = wide_mv ? 5U : 3U;
      mv_qpel = packed ? (mv_bytes % run_step == 1U)
                       : (mv_bytes == num_tiles * (wide_mv ? 4U : 2U) + 1U);
      if (mv_qpel && bitstream[8U + mv_bytes - 1U] != 0x51U) return -1;
      if (packed) {
        uint32_t payload_len = mv_qpel ? (mv_bytes - 1U) : mv_bytes;
        if (!zcvid_unpack_mvs(bitstream + 8U, payload_len, p->zcvid_dec_mv_scratch, num_tiles, wide_mv)) {
          return -1;
        }
        mv_src = p->zcvid_dec_mv_scratch;
      } else if (wide_mv) {
        const int16_t *src_mv = (const int16_t *)(bitstream + 8U);
        for (uint32_t i = 0; i < num_tiles; ++i) {
          p->zcvid_dec_mv_scratch[i * 2U + 0U] = (int32_t)src_mv[i * 2U + 0U];
          p->zcvid_dec_mv_scratch[i * 2U + 1U] = (int32_t)src_mv[i * 2U + 1U];
        }
        mv_src = p->zcvid_dec_mv_scratch;
      } else if (mv_bytes == num_tiles * 2U || mv_qpel) {
        const int8_t *src_mv = (const int8_t *)(bitstream + 8U);
        for (uint32_t i = 0; i < num_tiles; ++i) {
          p->zcvid_dec_mv_scratch[i * 2U + 0U] = (int32_t)src_mv[i * 2U + 0U];
          p->zcvid_dec_mv_scratch[i * 2U + 1U] = (int32_t)src_mv[i * 2U + 1U];
        }
        mv_src = p->zcvid_dec_mv_scratch;
      } else {
        mv_copy_bytes = (mv_bytes <= num_tiles * 8U) ? mv_bytes : (num_tiles * 8U);
        mv_src = bitstream + 8U;
      }
    }

    if (p->zcvid_prof_en) QueryPerformanceCounter(&dp1);
    D3D11_MAPPED_SUBRESOURCE mw = {0};
    if (!overlap_mv && (mv_src || !p->zcvid_dec_mvs_zero)) {
      if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->zcvid_dec_mvs_buf,
                                         0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
        return -1;
      }
      if (!mv_src) {
        memset(mw.pData, 0, (size_t)num_tiles * 8U);
        p->zcvid_dec_mvs_zero = true;
      } else {
        memcpy(mw.pData, mv_src, mv_copy_bytes);
        p->zcvid_dec_mvs_zero = false;
      }
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->zcvid_dec_mvs_buf, 0);
    }
    if (p->zcvid_prof_en) QueryPerformanceCounter(&dp2);

    bool frame_has_tq = false;
    const void *dec_rec_src = NULL;
    dp3 = dp2;
    dp4 = dp2;
    if (record_words > 0) {
      const void *rec_src = NULL;
      uint32_t got_words = record_words;
      if (packed_records) {
        if ((size_t)record_words > p->zcvid_dec_unpack_cap) {
          if (p->zcvid_dec_unpack_cap != 0U) zcvid_note_late_alloc("decoder records", (size_t)record_words * 4U);
          uint32_t *nb = (uint32_t *)realloc(p->zcvid_dec_unpack_buf,
                                             (size_t)record_words * sizeof(uint32_t));
          if (!nb) return -1;
          p->zcvid_dec_unpack_buf = nb;
          p->zcvid_dec_unpack_cap = record_words;
        }
        if ((hdr_word & ZCVID_EC_FLAG) != 0U) {
          if (ncls >= 9U && (hdr_word & ZCVID_TQ_V3_FLAG) == 0U) return -1;
          LARGE_INTEGER q0, q1;
          QueryPerformanceCounter(&q0);
          p->zcvid_dec_has_tq = false;
          got_words = overlap_mv
                          ? zcvid_ec_unpack_records_ex(p, bitstream + 8U + mv_bytes,
                                                      len - 8U - mv_bytes, p->zcvid_dec_unpack_buf,
                                                      record_words, ncls, &mv_job)
                          : zcvid_ec_unpack_records(p, bitstream + 8U + mv_bytes,
                                                   len - 8U - mv_bytes, p->zcvid_dec_unpack_buf,
                                                   record_words, ncls);
          QueryPerformanceCounter(&q1);
          if (got_words != UINT32_MAX) {
            zcvid_ec_note_time(&p->zcvid_ec_dec_nsw_q8, q0, q1, got_words);
            frame_has_tq = p->zcvid_dec_has_tq;
            if (overlap_mv) mv_qpel = mv_job.qpel;
          }
        } else {
          got_words = zcvid_unpack_records(bitstream + 8U + mv_bytes, len - 8U - mv_bytes,
                                          p->zcvid_dec_unpack_buf, record_words, ncls);
          frame_has_tq = (ncls >= 9U && got_words > 0U);
        }
        if (got_words == UINT32_MAX) return -1;
        rec_src = p->zcvid_dec_unpack_buf;
      } else {
        rec_src = bitstream + 8U + mv_bytes;
        frame_has_tq = (got_words > 0U);
      }
      dec_rec_src = rec_src;
      if (p->zcvid_prof_en) QueryPerformanceCounter(&dp3);
      if (overlap_mv) {
        if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->zcvid_dec_mvs_buf,
                                           0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
          return -1;
        }
        memcpy(mw.pData, mv_src, mv_copy_bytes);
        p->zcvid_dec_mvs_zero = false;
        ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->zcvid_dec_mvs_buf, 0);
      }
      if (got_words > 0) {
        if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->zcvid_in_records_buf,
                                           0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
          return -1;
        }
        memcpy(mw.pData, rec_src, (size_t)got_words * sizeof(uint32_t));
        ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->zcvid_in_records_buf, 0);
      }
      if (p->zcvid_prof_en) QueryPerformanceCounter(&dp4);
      record_words = got_words;
    }

    if (frame_has_tq && record_words >= 2U && dec_rec_src != NULL) {
      uint32_t nrecs = record_words / 2U;
      uint32_t step = (nrecs > 32U) ? (nrecs / 32U) : 1U;
      uint32_t qp_sum = 0U, qp_cnt = 0U;
      const uint8_t *rb = (const uint8_t *)dec_rec_src;
      for (uint32_t ri = 0U; ri < nrecs && qp_cnt < 32U; ri += step) {
        uint32_t w0 = 0U, w1 = 0U;
        memcpy(&w0, rb + (size_t)ri * 8U, 4U);
        if ((w0 >> 28) == 0xFU) {
          memcpy(&w1, rb + (size_t)ri * 8U + 4U, 4U);
          uint32_t rqp = (w1 >> 16) & 0xFFU;
          if (rqp <= 51U) {
            qp_sum += rqp;
            qp_cnt++;
          }
        }
      }
      if (qp_cnt > 0U) {
        uint32_t mean_qp = qp_sum / qp_cnt;
        uint32_t prev = p->zcvid_dec_post_qp;
        p->zcvid_dec_post_qp = (mean_qp > prev) ? mean_qp : ((prev * 3U + mean_qp) / 4U);
      }
    }
    uint32_t post_thr = ZCVID_POST_THR;
    if (p->zcvid_dec_post_qp > 28U) {
      post_thr = ZCVID_POST_THR + (p->zcvid_dec_post_qp - 28U) / 2U;
      if (post_thr > 14U) post_thr = 14U;
    }

    uint32_t ping = p->zcvid_dec_ping & 1U;
    uint32_t pong = ping ^ 1U;
    p->zcvid_dec_ping = pong;

    zcvid_dec_cb_t dcb = {
        p->width, p->height, p->zcvid_delta_min, p->zcvid_delta_max,
        p->zcvid_gamma_q8, p->zcvid_neutral_y, record_words, p->zcvid_tiles_x,
        mv_qpel ? 1U : 0U, {post_thr, 0, 0}
    };
    ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->zcvid_dec_cb,
                                          0, NULL, &dcb, 0, 0);

    ID3D11ShaderResourceView *srvs[3] = {
        p->zcvid_dec_mvs_srv, p->zcvid_in_records_srv, p->zcvid_dec_yuvd_srv[ping]
    };
    ID3D11UnorderedAccessView *uavs[4] = {
        p->zcvid_dec_yuvd_uav[pong], p->zcvid_dec_uav, p->zcvid_dec_coef_uav, p->zcvid_dec_tflag_uav
    };
    ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->zcvid_dec_cb);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, uavs, NULL);

    /* Pass 1: CSMotionDecay */
    ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_motion_decay, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U,
                                 (p->height + 15U) / 16U, 1);

    /* Pass 2: CSApply */
    if (record_words >= 2U) {
      ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_apply, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, ((record_words / 2U) + 63U) / 64U, 1, 1);
      /* Pass 2b: CSITrans rebuilds the transform-coded blocks (skip completely
       * when this frame carried no class-8 transform records). */
      if (frame_has_tq) {
        ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_itrans, NULL, 0);
        ID3D11DeviceContext_Dispatch(p->ctx, (p->zcvid_tq_blocks + 63U) / 64U, 1, 1);
      }
    }

    /* Pass 3: CSYCbCrToRGBA
     * Unbind g_cur_yuvd UAV (u0) and bind decoded cur_yuvd as read-only SRV (t2). */
    ID3D11UnorderedAccessView *null_uav0 = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 1, &null_uav0, NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 2, 1, &p->zcvid_dec_yuvd_srv[pong]);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_ycbcr_to_rgba, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U,
                                 (p->height + 15U) / 16U, 1);

    ID3D11ShaderResourceView *null_srv[3] = {NULL, NULL, NULL};
    ID3D11UnorderedAccessView *null_uav[4] = {NULL, NULL, NULL, NULL};
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, null_uav, NULL);

    QueryPerformanceCounter(&t1);
    p->dec_frame_idx++;
    if (p->zcvid_prof_en && p->dec_frame_idx > 30U) {
      p->zcvid_dec_prof_acc[0] += qpc_delta_us(dp0, dp1);
      p->zcvid_dec_prof_acc[1] += qpc_delta_us(dp1, dp2);
      p->zcvid_dec_prof_acc[2] += qpc_delta_us(dp2, dp3);
      p->zcvid_dec_prof_acc[3] += qpc_delta_us(dp3, dp4);
      p->zcvid_dec_prof_acc[4] += qpc_delta_us(dp4, t1);
      p->zcvid_dec_prof_acc[5] += qpc_delta_us(t0, t1);
      p->zcvid_dec_prof_count++;
    }

    if (p->show_window) {
      ID3D11Texture2D *backbuf = NULL;
      if (SUCCEEDED(IDXGISwapChain1_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                              (void **)&backbuf))) {
        if (p->zcvid_dec_use_copy) {
          ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)backbuf,
                                           (ID3D11Resource *)p->zcvid_dec_tex);
        } else {
          if (vp_blit_textures(p, p->zcvid_dec_tex, 0U, backbuf) != 0) {
            static int blit_err = 0;
            if (blit_err++ < 5) gpu_log_err("[GPU][DEC] zcvid vp_blit_textures failed!\n");
          }
        }
        ID3D11Texture2D_Release(backbuf);
      }
      UINT present_flags = p->tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
      IDXGISwapChain1_Present(p->swapchain, 0, present_flags);
    } else {
      ID3D11DeviceContext_Flush(p->ctx);
    }

    QueryPerformanceCounter(&t2);
    if (out_dec_us) *out_dec_us = qpc_delta_us(t0, t1);
    if (out_present_us) *out_present_us = qpc_delta_us(t1, t2);
    return 0;
  }

  IMFMediaBuffer *in_buf = NULL;
  MFCreateMemoryBuffer((DWORD)len, &in_buf);
  BYTE *dst = NULL;
  IMFMediaBuffer_Lock(in_buf, &dst, NULL, NULL);
  memcpy(dst, bitstream, len);
  IMFMediaBuffer_Unlock(in_buf);
  IMFMediaBuffer_SetCurrentLength(in_buf, (DWORD)len);

  IMFSample *in_sample = NULL;
  MFCreateSample(&in_sample);
  IMFSample_AddBuffer(in_sample, in_buf);
  IMFMediaBuffer_Release(in_buf);
  LONGLONG dur = 10000000LL / (LONGLONG)(p->fps ? p->fps : 60U);
  IMFSample_SetSampleTime(in_sample, (LONGLONG)(p->dec_frame_idx++) * dur);
  IMFSample_SetSampleDuration(in_sample, dur);

  static unsigned dec_err_count = 0;
  HRESULT hr = IMFTransform_ProcessInput(p->dec_mft, 0, in_sample, 0);
  IMFSample_Release(in_sample);
  if (FAILED(hr)) {
    if (dec_err_count++ < 10) {
      gpu_log_err("[GPU][DEC] ProcessInput failed: hr=0x%08lx len=%zu\n",
                  (unsigned long)hr, len);
    }
    return -40;
  }

  MFT_OUTPUT_DATA_BUFFER out_data = {0};
  DWORD status = 0;
  bool decoded = false;
  ID3D11Texture2D *dec_tex = NULL;
  UINT dec_sub = 0;

  for (int tries = 0; tries < 8; ++tries) {
    hr = IMFTransform_ProcessOutput(p->dec_mft, 0, 1, &out_data, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (dec_err_count < 5) {
        gpu_log_err("[GPU][DEC] ProcessOutput STREAM_CHANGE (try=%d)\n", tries);
      }
      for (DWORD idx = 0; ; ++idx) {
        IMFMediaType *mt_avail = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(p->dec_mft, 0, idx, &mt_avail))) break;
        GUID sub = {0};
        IMFMediaType_GetGUID(mt_avail, &ZC_MF_MT_SUBTYPE, &sub);
        if (IsEqualGUID(&sub, &ZC_MFVideoFormat_NV12)) {
          HRESULT hr_set = IMFTransform_SetOutputType(p->dec_mft, 0, mt_avail, 0);
          if (FAILED(hr_set) && dec_err_count++ < 10) {
            gpu_log_err("[GPU][DEC] SetOutputType(NV12) failed: hr=0x%08lx\n",
                        (unsigned long)hr_set);
          }
          IMFMediaType_Release(mt_avail);
          break;
        }
        IMFMediaType_Release(mt_avail);
      }
      continue;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (dec_err_count < 3) {
        gpu_log_err("[GPU][DEC] ProcessOutput NEED_MORE_INPUT (len=%zu)\n", len);
      }
      break;
    }
    if (FAILED(hr)) {
      if (dec_err_count++ < 10) {
        gpu_log_err("[GPU][DEC] ProcessOutput failed: hr=0x%08lx status=0x%lx\n",
                    (unsigned long)hr, (unsigned long)status);
      }
      break;
    }
    if (SUCCEEDED(hr) && out_data.pSample) {
      IMFMediaBuffer *ob = NULL;
      HRESULT hr_buf = IMFSample_GetBufferByIndex(out_data.pSample, 0, &ob);
      if (SUCCEEDED(hr_buf)) {
        IMFDXGIBuffer *dxgi_buf = NULL;
        HRESULT hr_qi = IMFMediaBuffer_QueryInterface(ob, &IID_IMFDXGIBuffer, (void **)&dxgi_buf);
        if (SUCCEEDED(hr_qi)) {
          HRESULT hr_res = IMFDXGIBuffer_GetResource(dxgi_buf, &IID_ID3D11Texture2D, (void **)&dec_tex);
          (void)IMFDXGIBuffer_GetSubresourceIndex(dxgi_buf, &dec_sub);
          if (FAILED(hr_res) && dec_err_count++ < 10) {
            gpu_log_err("[GPU][DEC] IMFDXGIBuffer_GetResource failed: hr=0x%08lx\n",
                        (unsigned long)hr_res);
          }
          IMFDXGIBuffer_Release(dxgi_buf);
        } else if (dec_err_count++ < 10) {
          gpu_log_err("[GPU][DEC] QueryInterface(IMFDXGIBuffer) failed: hr=0x%08lx\n",
                      (unsigned long)hr_qi);
        }
        IMFMediaBuffer_Release(ob);
      } else if (dec_err_count++ < 10) {
        gpu_log_err("[GPU][DEC] GetBufferByIndex failed: hr=0x%08lx\n",
                    (unsigned long)hr_buf);
      }
      IMFSample_Release(out_data.pSample);
      decoded = (dec_tex != NULL);
      break;
    }
  }

  QueryPerformanceCounter(&t1);

  if (decoded && dec_tex) {
    ID3D11Texture2D *backbuf = NULL;
    HRESULT hr_bb = IDXGISwapChain1_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                              (void **)&backbuf);
    if (SUCCEEDED(hr_bb)) {
      if (vp_blit_textures(p, dec_tex, dec_sub, backbuf) != 0 && dec_err_count++ < 10) {
        gpu_log_err("[GPU][DEC] h264 vp_blit_textures failed!\n");
      }
      ID3D11Texture2D_Release(backbuf);
    } else if (dec_err_count++ < 10) {
      gpu_log_err("[GPU][DEC] SwapChain GetBuffer failed: hr=0x%08lx\n",
                  (unsigned long)hr_bb);
    }
    ID3D11Texture2D_Release(dec_tex);

    if (p->show_window) {
      UINT present_flags = p->tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
      HRESULT hr_prs = IDXGISwapChain1_Present(p->swapchain, 0, present_flags);
      if (FAILED(hr_prs) && dec_err_count++ < 10) {
        gpu_log_err("[GPU][DEC] SwapChain Present failed: hr=0x%08lx\n",
                    (unsigned long)hr_prs);
      }
    } else {
      ID3D11DeviceContext_Flush(p->ctx);
    }
  }

  QueryPerformanceCounter(&t2);
  if (out_dec_us) *out_dec_us = qpc_delta_us(t0, t1);
  if (out_present_us) *out_present_us = qpc_delta_us(t1, t2);
  return decoded ? 0 : 1;
}

bool gpu_pipeline_pump_events(gpu_pipeline_t *p) {
  MSG msg;
  while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      p->window_closed = true;
      return false;
    }
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return !p->window_closed;
}

static const char k_zcvid_psnr_hlsl[] =
    "cbuffer PsParams : register(b0) { uint width; uint height; uint pad0; uint pad1; };\n"
    "Texture2D<unorm float4> g_src : register(t0);\n"
    "Texture2D<unorm float4> g_dec : register(t1);\n"
    "RWByteAddressBuffer g_sum : register(u0);\n"
    "groupshared uint gs_acc[256];\n"
    "[numthreads(16, 16, 1)]\n"
    "void CSComputePSNR(uint3 dtid : SV_DispatchThreadID, uint gidx : SV_GroupIndex) {\n"
    "  uint e2 = 0u;\n"
    "  if (dtid.x < width && dtid.y < height) {\n"
    "    float3 s = g_src.Load(int3(dtid.xy, 0)).rgb * 255.0f;\n"
    "    float3 d = g_dec.Load(int3(dtid.xy, 0)).rgb * 255.0f;\n"
    "    float sy = 0.299f * s.r + 0.587f * s.g + 0.114f * s.b;\n"
    "    float dy = 0.299f * d.r + 0.587f * d.g + 0.114f * d.b;\n"
    "    float diff = sy - dy;\n"
    "    e2 = (uint)round(diff * diff * 16.0f);\n"
    "  }\n"
    "  gs_acc[gidx] = e2;\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  [unroll] for (uint off = 128u; off > 0u; off >>= 1) {\n"
    "    if (gidx < off) gs_acc[gidx] += gs_acc[gidx + off];\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "  }\n"
    "  if (gidx == 0u && gs_acc[0] != 0u) {\n"
    "    uint add_val = gs_acc[0];\n"
    "    uint prev_lo = 0u;\n"
    "    g_sum.InterlockedAdd(0, add_val, prev_lo);\n"
    "    if (prev_lo + add_val < prev_lo) {\n"
    "      uint dummy = 0u;\n"
    "      g_sum.InterlockedAdd(4, 1u, dummy);\n"
    "    }\n"
    "  }\n"
    "}\n";

static int ensure_zcvid_psnr_gpu(gpu_pipeline_t *p) {
  if (p->zcvid_psnr_stg) return 0;
  if (!p->zcvid_dec_srv) {
    if (FAILED(ID3D11Device_CreateShaderResourceView(
            p->dev, (ID3D11Resource *)p->zcvid_dec_tex, NULL, &p->zcvid_dec_srv))) {
      return -2;
    }
  }
  if (!p->zcvid_cs_psnr) {
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = D3DCompile(k_zcvid_psnr_hlsl, sizeof(k_zcvid_psnr_hlsl) - 1,
                            "zcvid_psnr.hlsl", NULL, NULL, "CSComputePSNR",
                            "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr) || !blob) {
      if (blob) ID3D10Blob_Release(blob);
      return -2;
    }
    hr = ID3D11Device_CreateComputeShader(p->dev, ID3D10Blob_GetBufferPointer(blob),
                                          ID3D10Blob_GetBufferSize(blob), NULL, &p->zcvid_cs_psnr);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) return -2;
  }

  if (!p->zcvid_psnr_cb) {
    UINT cb_init[4] = {p->width, p->height, 0U, 0U};
    D3D11_BUFFER_DESC cbd = {0};
    cbd.ByteWidth = 16U;
    cbd.Usage = D3D11_USAGE_IMMUTABLE;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {cb_init, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(p->dev, &cbd, &sd, &p->zcvid_psnr_cb))) return -2;
  }

  if (!p->zcvid_psnr_buf) {
    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = 16U;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->zcvid_psnr_buf))) return -2;
  }

  if (!p->zcvid_psnr_uav) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {0};
    uavd.Format = DXGI_FORMAT_R32_TYPELESS;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = 4U;
    uavd.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(
            p->dev, (ID3D11Resource *)p->zcvid_psnr_buf, &uavd, &p->zcvid_psnr_uav))) {
      return -2;
    }
  }

  D3D11_BUFFER_DESC sbd = {0};
  sbd.ByteWidth = 16U;
  sbd.Usage = D3D11_USAGE_STAGING;
  sbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(ID3D11Device_CreateBuffer(p->dev, &sbd, NULL, &p->zcvid_psnr_stg))) return -2;
  return 0;
}

int gpu_pipeline_zcvid_compute_psnr(gpu_pipeline_t *p, double *out_psnr_db) {
  if (!p || !p->use_zcvid || !out_psnr_db) return -1;
  if (ensure_zcvid_psnr_gpu(p) != 0) return -2;

  UINT zero4[4] = {0, 0, 0, 0};
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->zcvid_psnr_uav, zero4);
  ID3D11ShaderResourceView *srvs[2] = {p->cap_bgra_srv, p->zcvid_dec_srv};
  ID3D11UnorderedAccessView *uav0 = p->zcvid_psnr_uav;
  ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->zcvid_psnr_cb);
  ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 2, srvs);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 1, &uav0, NULL);
  ID3D11DeviceContext_CSSetShader(p->ctx, p->zcvid_cs_psnr, NULL, 0);
  ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U, (p->height + 15U) / 16U, 1);

  ID3D11ShaderResourceView *null_srvs[2] = {NULL, NULL};
  ID3D11UnorderedAccessView *null_uav = NULL;
  ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 2, null_srvs);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 1, &null_uav, NULL);

  ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->zcvid_psnr_stg,
                                   (ID3D11Resource *)p->zcvid_psnr_buf);
  D3D11_MAPPED_SUBRESOURCE ms = {0};
  if (FAILED(map_read_sync(p->ctx, (ID3D11Resource *)p->zcvid_psnr_stg, &ms))) return -3;
  const uint32_t *acc = (const uint32_t *)ms.pData;
  uint64_t sum_q4 = (uint64_t)acc[0] | ((uint64_t)acc[1] << 32);
  ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->zcvid_psnr_stg, 0);

  double mse = ((double)sum_q4 / 16.0) / (double)((size_t)p->width * (size_t)p->height);
  *out_psnr_db = (mse < 1e-9) ? 99.0 : (10.0 * log10((255.0 * 255.0) / mse));
  return 0;
}

int gpu_pipeline_zcvid_ref_mismatch(gpu_pipeline_t *p, uint64_t *out_mismatch_px) {
  if (!p || !p->use_zcvid || !out_mismatch_px) return -1;

  size_t num_px = (size_t)p->width * (size_t)p->height;
  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = (UINT)(num_px * sizeof(uint32_t));
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);

  ID3D11Buffer *stg[2] = {NULL, NULL};
  /* zcvid_*_ping index the buffers written by the last encode / decode. */
  ID3D11Buffer *src[2] = {p->zcvid_enc_yuvd_buf[p->zcvid_enc_ping & 1U],
                          p->zcvid_dec_yuvd_buf[p->zcvid_dec_ping & 1U]};
  D3D11_MAPPED_SUBRESOURCE map[2];
  int mapped = 0;
  int rc = 0;

  for (int i = 0; i < 2 && rc == 0; ++i) {
    if (FAILED(ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &stg[i]))) {
      rc = -2;
      break;
    }
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)stg[i],
                                     (ID3D11Resource *)src[i]);
    if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)stg[i], 0,
                                       D3D11_MAP_READ, 0, &map[i]))) {
      rc = -3;
      break;
    }
    mapped++;
  }

  if (rc == 0) {
    const uint32_t *enc_ref = (const uint32_t *)map[0].pData;
    const uint32_t *dec_ref = (const uint32_t *)map[1].pData;
    uint64_t diff = 0;
    for (size_t i = 0; i < num_px; ++i) diff += (enc_ref[i] != dec_ref[i]);
    *out_mismatch_px = diff;
  }

  for (int i = 0; i < 2; ++i) {
    if (i < mapped) ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)stg[i], 0);
    if (stg[i]) ID3D11Buffer_Release(stg[i]);
  }
  return rc;
}
