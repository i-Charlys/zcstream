#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
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
#include <string.h>

#include "gpu_pipeline_win32.h"
#include "ternary_entropy.h"
#include "nvEncodeAPI.h"

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

static void wait_query_event(ID3D11DeviceContext *ctx, ID3D11Query *q) {
  if (!q) return;
  ID3D11DeviceContext_Flush(ctx);
  for (uint32_t spin = 0; ; ++spin) {
    HRESULT hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q,
                                             NULL, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_OK || FAILED(hr)) break;
    if (spin >= 32U) {
      SwitchToThread();
    }
  }
}

static HRESULT map_read_nonblocking(ID3D11DeviceContext *ctx, ID3D11Resource *res,
                                    D3D11_MAPPED_SUBRESOURCE *mr) {
  for (uint32_t spin = 0; ; ++spin) {
    HRESULT hr = ID3D11DeviceContext_Map(ctx, res, 0, D3D11_MAP_READ,
                                         D3D11_MAP_FLAG_DO_NOT_WAIT, mr);
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING) return hr;
    if (spin >= 32U) {
      SwitchToThread();
    }
  }
}

/* The range coder cuts a frame into at most this many chunks, one per thread. */
#define TERN_EC_MAX_CHUNKS 16U
#define TERN_STG_RING 3U

/* Display deblocking threshold (CSYCbCrToRGBA), 0 = off. */
#ifndef TERN_POST_THR
#define TERN_POST_THR 6U
#endif

/* 1: the rate control never halves the horizontal resolution of deltas. */
#ifndef TERN_RC_NO_2X
#define TERN_RC_NO_2X 0
#endif

/* Transform intra qp = TERN_TQ_QP_BASE + 2 * threshold (clamped 12..51 in the shader). */
#ifndef TERN_TQ_QP_BASE
#define TERN_TQ_QP_BASE 16U
#endif
/* Transform inter takes a block whose mean |luma residual| after motion
 * compensation exceeds TERN_TQ_INTER_BASE + TERN_TQ_INTER_PER_THR * threshold,
 * and only if the coded residual brings it clearly closer to the source (the
 * shader checks). 0 + 0: every block where the transform pays, the hybrid
 * mode; the ternary deltas refine the rest. */
#ifndef TERN_TQ_INTER_BASE
#define TERN_TQ_INTER_BASE 0U
#endif
#ifndef TERN_TQ_INTER_PER_THR
#define TERN_TQ_INTER_PER_THR 0U
#endif

#ifndef TERN_TQ_QP_PER_THR
#define TERN_TQ_QP_PER_THR 2U
#endif
/* 1: the transform qp follows its own loop (start / floor below). */
#ifndef TERN_TQ_ADAPTIVE
#define TERN_TQ_ADAPTIVE 1
#endif
/* The transform qp loop: coarser only when a frame asks for more than
 * twice the budget, finer as soon as it fits. The delta threshold absorbs
 * smaller overshoots; the transform, being the cheaper way to fix a block,
 * keeps a fine qp as long as it can. */
#ifndef TERN_TQ_UP_PCT
#define TERN_TQ_UP_PCT 200U
#endif
#ifndef TERN_TQ_DOWN_PCT
#define TERN_TQ_DOWN_PCT 100U
#endif
#ifndef TERN_TQ_QP_START
#define TERN_TQ_QP_START 28U
#endif
#ifndef TERN_TQ_QP_MIN
#define TERN_TQ_QP_MIN 16U
#endif

/* Chroma qp of the transform intra = luma qp + TERN_TQ_CQP. */
#ifndef TERN_TQ_CQP
#define TERN_TQ_CQP 3U
#endif

/* Lowest threshold / intra trigger the rate control goes to when the frames
 * stay well under budget (encoder-only: the decoder never sees either). */
#ifndef TERN_RC_MIN_THRESHOLD
#define TERN_RC_MIN_THRESHOLD 2U
#endif
#ifndef TERN_RC_MIN_INTRA_ERR
#define TERN_RC_MIN_INTRA_ERR 16U
#endif

struct tern_pool_s;

struct gpu_pipeline_s {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  uint32_t bitrate_bps;

  HWND hwnd;
  bool window_closed;
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
  HMODULE nvenc_dll;
  NV_ENCODE_API_FUNCTION_LIST nvenc_fn;
  void *nvenc_session;
  NV_ENC_REGISTERED_PTR nvenc_reg_res;
  NV_ENC_OUTPUT_PTR nvenc_bitstream;
  NV_ENC_INITIALIZE_PARAMS nvenc_init_params;
  NV_ENC_CONFIG nvenc_config;

  uint8_t *enc_out_buf;
  size_t enc_out_cap;

  /* Ternary HLSL Compute (cs_5_0) zero-copy VRAM state */
  bool use_ternary;
  uint32_t tern_threshold;
  uint32_t tern_delta_min;
  uint32_t tern_delta_max;
  uint32_t tern_gamma_q8;
  uint32_t tern_neutral_y;
  uint32_t tern_total_blocks;
  uint32_t tern_tiles_x;
  uint32_t tern_tiles_y;
  uint32_t tern_max_record_words;
  uint32_t tern_enc_ping;
  uint32_t tern_dec_ping;
  uint32_t tern_rc_threshold;
  uint32_t tern_rc_intra_err_thr;
  uint32_t tern_kf_countdown;
  bool tern_packed;            /* emit the compact bitstream (see tern_pack_records) */
  uint32_t tern_pack_ratio_q16; /* packed bytes per raw record byte, running average */
  uint32_t tern_pack_mv_bytes;  /* packed vector bytes of the previous frame */
  uint64_t *tern_sort_a;        /* radix sort scratch, tern_sort_cap entries each */
  uint64_t *tern_sort_b;
  uint32_t tern_sort_cap;
  bool tern_ec;                 /* range-code the packed records (see tern_ec_pack_records) */
  bool tern_ec_frame;           /* rate control sized this frame for the range coder */
  uint32_t tern_ec_ratio_q16;   /* range-coded bytes per raw record byte, running average */
  uint32_t tern_ec_threads;     /* chunks coded side by side, 1..TERN_EC_MAX_CHUNKS */
  uint32_t tern_ec_enc_nsw_q8;  /* measured coding time, ns per record word (Q8) */
  uint32_t tern_ec_dec_nsw_q8;  /* measured decoding time, same unit */
  uint32_t tern_ec_peer_limit;  /* record words per frame the decoder asked for, 0 = any */
  struct tern_pool_s *tern_pool; /* their worker threads, created on first use */
  /* Per-chunk coder scratch: */
  uint8_t *tern_ec_line[TERN_EC_MAX_CHUNKS];  /* intra line buffers, 3 bytes per column */
  uint32_t *tern_ec_lrow[TERN_EC_MAX_CHUNKS]; /* row + 1 of the sample of each column */
  void *tern_ec_model[TERN_EC_MAX_CHUNKS];    /* zcrudp_tec_model_t */
  uint8_t *tern_ec_scratch;     /* chunk streams before they are laid end to end */
  size_t tern_ec_scratch_cap;
  bool tern_rc_2x;
  bool tern_intra8;             /* rolling refresh in 8-bit 2-px intra (budget to spare) */
  bool tern_tq;                 /* transform intra on 4x4 blocks (CSIntraT / CSITrans) */
  bool tern_tq_inter;           /* ... and transform-coded motion residual */
  bool tern_qpel;               /* tile vectors in quarter pels */
  bool tern_dither;             /* ordered dither on the 6-bit rolling intra */
  uint32_t tern_tq_qp;          /* transform qp, its own rate loop (TERN_TQ_ADAPTIVE) */
  uint32_t tern_ncls;           /* record classes in the packed layouts being written */
  uint32_t tern_sort_kept;      /* records the last sort kept (unused slots dropped) */
  uint32_t tern_last_raw_words; /* record words read back for the last frame, before packing */
  uint32_t tern_tq_blocks;      /* 4x4 blocks in a frame */
  ID3D11ComputeShader *tern_cs_intra_t;
  ID3D11ComputeShader *tern_cs_itrans;
  ID3D11Buffer *tern_enc_tflag_buf;
  ID3D11UnorderedAccessView *tern_enc_tflag_uav;
  ID3D11ShaderResourceView *tern_enc_tflag_srv;
  ID3D11Buffer *tern_dec_coef_buf;
  ID3D11UnorderedAccessView *tern_dec_coef_uav;
  ID3D11Buffer *tern_dec_tflag_buf;
  ID3D11UnorderedAccessView *tern_dec_tflag_uav;
  bool tern_static_scene;
  bool tern_dec_use_copy;
  ID3D11ComputeShader *tern_cs_tile_mv;
  ID3D11ComputeShader *tern_cs_encode;
  ID3D11ComputeShader *tern_cs_motion_decay;
  ID3D11ComputeShader *tern_cs_apply;
  ID3D11ComputeShader *tern_cs_ycbcr_to_rgba;
  ID3D11ShaderResourceView *cap_bgra_srv;
  ID3D11Texture2D *tern_prev_src_tex; /* previous captured frame (motion search) */
  ID3D11RenderTargetView *tern_prev_src_rtv;
  ID3D11ShaderResourceView *tern_prev_src_srv;
  bool tern_has_prev_src;
  ID3D11Texture2D *tern_half_tex[2]; /* half-res luma, current / previous frame */
  ID3D11ShaderResourceView *tern_half_srv[2];
  ID3D11UnorderedAccessView *tern_half_uav[2];
  ID3D11Texture2D *tern_quarter_tex[2]; /* quarter-res luma, same ping-pong */
  ID3D11ShaderResourceView *tern_quarter_srv[2];
  ID3D11UnorderedAccessView *tern_quarter_uav[2];
  ID3D11ComputeShader *tern_cs_luma_quarter;
  uint32_t tern_half_ping;
  ID3D11ComputeShader *tern_cs_luma_half;
  ID3D11Buffer *tern_enc_yuvd_buf[2];
  ID3D11ShaderResourceView *tern_enc_yuvd_srv[2];
  ID3D11UnorderedAccessView *tern_enc_yuvd_uav[2];
  ID3D11Buffer *tern_enc_mvs_buf[2];
  ID3D11Buffer *tern_hint_buf;        /* capture hints per tile, see build_capture_hints */
  ID3D11ShaderResourceView *tern_hint_srv;
  int32_t *tern_hint_cpu;             /* 4 ints per tile */
  uint8_t *dupl_meta;                 /* move / dirty rects of the last acquired frame */
  UINT dupl_meta_cap;
  bool tern_hints_static;             /* no new desktop frame: every tile unchanged */
  bool tern_hints_set;                /* tern_hint_cpu holds this frame's hints */
  uint64_t hint_frames, hint_moved_tiles, hint_static_tiles, hint_move_rects; /* counters */
  ID3D11ShaderResourceView *tern_enc_mvs_srv[2];
  ID3D11UnorderedAccessView *tern_enc_mvs_uav[2];
  uint32_t tern_mv_ping;
  ID3D11Buffer *tern_enc_mvs_stg[TERN_STG_RING];
  ID3D11Buffer *tern_out_records_buf;
  ID3D11UnorderedAccessView *tern_out_records_uav;
  ID3D11Buffer *tern_out_records_stg[TERN_STG_RING];
  ID3D11Buffer *tern_out_counter_buf;
  ID3D11UnorderedAccessView *tern_out_counter_uav;
  ID3D11Buffer *tern_out_counter_stg[TERN_STG_RING];
  ID3D11Query *tern_readback_query[TERN_STG_RING];
  uint32_t tern_stg_idx;
  ID3D11Buffer *tern_dec_yuvd_buf[2];
  ID3D11ShaderResourceView *tern_dec_yuvd_srv[2];
  ID3D11UnorderedAccessView *tern_dec_yuvd_uav[2];
  ID3D11Buffer *tern_dec_mvs_buf;
  ID3D11ShaderResourceView *tern_dec_mvs_srv;
  ID3D11Buffer *tern_in_records_buf;
  ID3D11ShaderResourceView *tern_in_records_srv;
  uint32_t *tern_dec_unpack_buf;
  size_t tern_dec_unpack_cap;
  int32_t *tern_dec_mv_scratch;
  uint32_t tern_dec_mv_cap;
  ID3D11Texture2D *tern_dec_tex;
  ID3D11UnorderedAccessView *tern_dec_uav;
  ID3D11Buffer *tern_enc_cb;
  ID3D11Buffer *tern_dec_cb;
};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY || msg == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
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

  /* Try Desktop Duplication on chosen adapter's first output */
  if (chosen) {
    IDXGIOutput *out = NULL;
    if (SUCCEEDED(IDXGIAdapter1_EnumOutputs(chosen, 0, &out))) {
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

  DWORD style = show_window ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE) : WS_POPUP;
  p->hwnd = CreateWindowExA(0, wc.lpszClassName,
                            "zcrudp + D3D11 Zero-Copy Video Pipeline", style,
                            100, 100, (int)p->width / 2, (int)p->height / 2,
                            NULL, NULL, wc.hInstance, NULL);

  DXGI_SWAP_CHAIN_DESC1 scd = {0};
  scd.Width = p->width;
  scd.Height = p->height;
  scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 2;
  scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  scd.Flags = p->tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

  hr = IDXGIFactory2_CreateSwapChainForHwnd(fac2, (IUnknown *)p->dev, p->hwnd,
                                            &scd, NULL, NULL, &p->swapchain);
  IDXGIFactory2_Release(fac2);
  if (FAILED(hr)) return -3;

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

  td.Format = DXGI_FORMAT_NV12;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  hr = ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->enc_nv12_tex);
  if (FAILED(hr)) return -5;

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
} tern_enc_cb_t;

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
} tern_dec_cb_t;

/* ---------------------------------------------------------------------------
 * Compact ternary bitstream (lossless repacking of what the shaders produce).
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
#define TERN_PACKED_FLAG 0x80000000U
#define TERN_RAW_RECORDS_FLAG 0x40000000U
#define TERN_RECORD_WORDS_MASK 0x0FFFFFFFU
/* Packed layouts carry 9 record classes (class 8: transform intra
 * coefficients) instead of 8. Only set when that class is in use, so a
 * stream without it stays readable by decoders that predate it. */
#define TERN_V2_FLAG 0x10000000U
#define TERN_CLASSES_MAX 9U

static uint8_t *tern_put_varint(uint8_t *o, uint32_t v) {
  while (v >= 0x80U) {
    *o++ = (uint8_t)(v | 0x80U);
    v >>= 7;
  }
  *o++ = (uint8_t)v;
  return o;
}

static const uint8_t *tern_get_varint(const uint8_t *i, const uint8_t *end, uint32_t *v) {
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

static uint32_t tern_record_class(uint32_t w0, uint32_t *id) {
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

/* Worst case: 1 count byte per class, 5 gap bytes + 5 payload bytes per record. */
static size_t tern_pack_bound(uint32_t num_records) {
  return 8U * 5U + (size_t)num_records * 10U;
}

/* Sorts records by (class, position). Returns the sorted keys, each
 * (class << 60 | position << 32 | source index), or NULL without memory. */
/* A record word pair the encoder left unused (refused transform block). */
#define TERN_NO_RECORD 0xFFFFFFFFU

static const uint64_t *tern_sort_records(gpu_pipeline_t *p, const uint32_t *rec,
                                         uint32_t *num_io, uint32_t counts[TERN_CLASSES_MAX]) {
  uint32_t num_records = *num_io;
  if (num_records > p->tern_sort_cap) {
    uint32_t cap = num_records + num_records / 2U + 1024U;
    uint64_t *a = (uint64_t *)realloc(p->tern_sort_a, (size_t)cap * sizeof(uint64_t));
    if (a) p->tern_sort_a = a;
    uint64_t *b = (uint64_t *)realloc(p->tern_sort_b, (size_t)cap * sizeof(uint64_t));
    if (b) p->tern_sort_b = b;
    if (!a || !b) return NULL;
    p->tern_sort_cap = cap;
  }
  uint64_t *a = p->tern_sort_a, *b = p->tern_sort_b;
  memset(counts, 0, TERN_CLASSES_MAX * sizeof(uint32_t));
  uint32_t kept = 0;
  for (uint32_t i = 0; i < num_records; ++i) {
    if (rec[i * 2U] == TERN_NO_RECORD) continue;
    uint32_t id = 0;
    uint32_t cls = tern_record_class(rec[i * 2U], &id);
    counts[cls]++;
    /* Positions stay below 2^28 for every class up to 4K. */
    a[kept++] = ((uint64_t)((cls << 28) | (id & 0x0FFFFFFFU)) << 32) | i;
  }
  num_records = kept;
  *num_io = kept;
  p->tern_sort_kept = kept;
  /* LSD radix sort on the 32-bit key held in the top half. */
  for (unsigned pass = 0; pass < 3U; ++pass) {
    unsigned shift = 32U + pass * 11U;
    uint32_t hist[2048] = {0};
    for (uint32_t i = 0; i < num_records; ++i) hist[(a[i] >> shift) & 2047U]++;
    uint32_t sum = 0;
    for (unsigned h = 0; h < 2048U; ++h) {
      uint32_t c = hist[h];
      hist[h] = sum;
      sum += c;
    }
    for (uint32_t i = 0; i < num_records; ++i) b[hist[(a[i] >> shift) & 2047U]++] = a[i];
    uint64_t *t = a;
    a = b;
    b = t;
  }
  return a;
}

/* Packs the records of sorted keys a[0 .. num). */
static size_t tern_pack_sorted(const uint32_t *rec, const uint64_t *a, uint32_t num,
                               uint32_t ncls, uint8_t *out) {
  uint32_t counts[TERN_CLASSES_MAX] = {0};
  for (uint32_t n = 0; n < num; ++n) counts[a[n] >> 60]++;

  uint8_t *o = out;
  uint32_t i = 0;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    o = tern_put_varint(o, counts[cls]);
    uint32_t prev = 0;
    for (uint32_t n = 0; n < counts[cls]; ++n, ++i) {
      uint32_t src = (uint32_t)a[i];
      uint32_t id = (uint32_t)(a[i] >> 32) & 0x0FFFFFFFU;
      uint32_t w0 = rec[src * 2U], w1 = rec[src * 2U + 1U];
      o = tern_put_varint(o, id - prev);
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
      } else { /* level (16 bits), qp (6 bits) and pred (bit 7) */
        *o++ = (uint8_t)w1;
        *o++ = (uint8_t)(w1 >> 8);
        *o++ = (uint8_t)(((w1 >> 16) & 0x3FU) | (((w1 >> 24) & 1U) << 7));
      }
    }
  }
  return (size_t)(o - out);
}

static size_t tern_pack_records(gpu_pipeline_t *p, const uint32_t *rec,
                                uint32_t num_records, uint8_t *out) {
  uint32_t counts[TERN_CLASSES_MAX];
  const uint64_t *a = tern_sort_records(p, rec, &num_records, counts);
  if (!a) return 0;
  return tern_pack_sorted(rec, a, num_records, p->tern_ncls, out);
}

/* Expands a packed record stream back to 2 words per record. Returns the word
 * count, or UINT32_MAX on a malformed stream. */
static uint32_t tern_unpack_records(const uint8_t *in, size_t in_len, uint32_t *out,
                                    uint32_t max_words, uint32_t ncls) {
  const uint8_t *i = in, *end = in + in_len;
  uint32_t words = 0;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    uint32_t count = 0;
    i = tern_get_varint(i, end, &count);
    if (!i) return UINT32_MAX;
    uint32_t id = 0;
    for (uint32_t n = 0; n < count; ++n) {
      uint32_t gap = 0;
      i = tern_get_varint(i, end, &gap);
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
             ((uint32_t)(i[2] >> 7) << 24);
        i += 3;
      }
      out[words++] = w0;
      out[words++] = w1;
    }
  }
  return words;
}

/* ---------------------------------------------------------------------------
 * Range-coded records (TERN_EC_FLAG). Same sorted classes as the packed
 * layout, but every field goes through the adaptive binary range coder of
 * ternary_entropy.h, one stream for the whole frame:
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
#define TERN_EC_FLAG 0x20000000U
#define TERN_EC_COUNT_BITS 24U

/* Coding time grows with the record count, so each side measures what a
 * record word costs it and the coder takes TERN_EC_TIME_PCT percent of the
 * frame period at most. Records beyond that budget are not given up on: the
 * frame carries them in the plain packed layout after the coded ones. */
#ifndef TERN_EC_TIME_PCT
#define TERN_EC_TIME_PCT 30U
#endif

typedef struct {
  zcrudp_iec_model_t luma;
  zcrudp_iec_model_t chroma;
  uint8_t *line;       /* [y, cb, cr] per column: the sample last coded there */
  uint32_t *lrow;      /* its row + 1, 0 when the column holds nothing */
  uint32_t width;
  uint32_t last_idx;   /* pixel index of the last sample stored */
  uint8_t last_up[3];  /* what its column held before, when that was the row above */
  bool last_up_ok;
  uint32_t act_y;      /* magnitude of the last luma / chroma residual */
  uint32_t act_c;
} tern_ec_intra_t;

static void tern_ec_intra_reset(tern_ec_intra_t *s, uint8_t *line, uint32_t *lrow,
                                uint32_t width) {
  zcrudp_iec_model_reset(&s->luma);
  zcrudp_iec_model_reset(&s->chroma);
  s->line = line;
  s->lrow = lrow;
  s->width = width;
  s->last_idx = UINT32_MAX;
  s->last_up_ok = false;
  s->act_y = 0;
  s->act_c = 0;
  memset(lrow, 0, (size_t)width * sizeof(uint32_t));
}

/* Neighbours of pixel idx among the samples of this frame coded so far. */
typedef struct {
  bool has_a, has_b, has_c;
  uint8_t a[3], b[3], c[3]; /* left, above, above-left */
} tern_ec_nb_t;

static void tern_ec_neighbours(const tern_ec_intra_t *s, uint32_t idx, tern_ec_nb_t *nb) {
  uint32_t row = idx / s->width, x = idx - row * s->width;
  nb->has_a = (x > 0U) && s->lrow[x - 1U] == row + 1U;
  nb->has_b = (row > 0U) && s->lrow[x] == row;
  /* The column to the left now holds this row: its former content was kept
   * aside when the previous sample was stored. */
  nb->has_c = nb->has_a && s->last_up_ok && s->last_idx + 1U == idx;
  if (nb->has_a) memcpy(nb->a, s->line + (size_t)(x - 1U) * 3U, 3);
  if (nb->has_b) memcpy(nb->b, s->line + (size_t)x * 3U, 3);
  if (nb->has_c) memcpy(nb->c, s->last_up, 3);
}

static uint32_t tern_ec_predict(const tern_ec_nb_t *nb, unsigned ch, uint32_t *grad) {
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

static void tern_ec_store(tern_ec_intra_t *s, uint32_t idx, uint8_t y, uint8_t cb, uint8_t cr) {
  uint32_t row = idx / s->width, x = idx - row * s->width;
  uint8_t *l = s->line + (size_t)x * 3U;
  s->last_up_ok = (row > 0U) && s->lrow[x] == row;
  memcpy(s->last_up, l, 3);
  s->last_idx = idx;
  l[0] = y;
  l[1] = cb;
  l[2] = cr;
  s->lrow[x] = row + 1U;
}

/* Samples travel as 8-bit values (shift 0) or 6-bit ones (shift 2); the
 * predictors always work on 8 bits. */
static void tern_ec_put_sample(zcrudp_rc_enc_t *rc, zcrudp_iec_model_t *m, uint32_t *act,
                               uint32_t pred8, uint32_t grad, unsigned shift, uint32_t v) {
  uint32_t a = (grad != UINT32_MAX) ? (grad >> shift) : (*act * 2U);
  int32_t res = (int32_t)v - (int32_t)(pred8 >> shift);
  zcrudp_iec_enc_residual(rc, m, zcrudp_iec_bucket(a), 0U, res);
  *act = (uint32_t)abs(res);
}

static uint32_t tern_ec_get_sample(zcrudp_rc_dec_t *rc, zcrudp_iec_model_t *m, uint32_t *act,
                                   uint32_t pred8, uint32_t grad, unsigned shift) {
  uint32_t a = (grad != UINT32_MAX) ? (grad >> shift) : (*act * 2U);
  int32_t res = zcrudp_iec_dec_residual(rc, m, zcrudp_iec_bucket(a), 0U);
  *act = (uint32_t)abs(res);
  return (uint32_t)((int32_t)(pred8 >> shift) + res) & (0xFFU >> shift);
}

/* A 6-bit sample as the 8-bit value the shaders rebuild from it. */
static uint8_t tern_ec_expand6(uint32_t q6) { return (uint8_t)((q6 << 2) | (q6 >> 4)); }

/* Trits are indexed block * 10 + j; returns the distance to the trit above
 * (0 when rows do not line up). In 2x mode a trit covers two pixels. */
static size_t tern_ec_delta_stride(uint32_t width, uint32_t cls) {
  if ((cls & 1U) == 0U) return width;
  return (width & 1U) ? 0U : width / 2U;
}

/* Positions a record of this class may take; anything else is corrupt. */
static uint32_t tern_ec_id_limit(uint32_t num_px, uint32_t cls) {
  if (cls < 6U) return num_px / 10U + 1U;
  return (cls == 6U) ? num_px : (num_px + 1U) / 2U;
}

/* Class 8 positions: 48 coefficients per 4x4 block. */
static uint32_t tern_ec_tq_limit(const gpu_pipeline_t *p) {
  return ((p->width + 3U) / 4U) * ((p->height + 3U) / 4U) * 48U;
}

static uint32_t tern_ec_trit_ctx(const zcrudp_tec_model_t *m, size_t idx, uint32_t *sctx) {
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

static void tern_ec_put_trits(zcrudp_rc_enc_t *rc, zcrudp_tec_model_t *m, size_t base,
                              uint32_t w1) {
  for (size_t j = 0; j < 10U; ++j) {
    size_t idx = base + j;
    uint32_t code = w1 % 3U; /* 0 = minus, 1 = zero, 2 = plus */
    w1 /= 3U;
    uint32_t sctx = 0;
    uint32_t zctx = tern_ec_trit_ctx(m, idx, &sctx);
    uint8_t h = 0;
    zcrudp_rc_enc_bit(rc, &m->zero[zctx], (code != 1U) ? 1U : 0U);
    if (code != 1U) {
      zcrudp_rc_enc_bit(rc, &m->sign[sctx], (code == 0U) ? 1U : 0U);
      h = (code == 0U) ? 2U : 1U;
    }
    m->hist[idx & ZCRUDP_TEC_HIST_MASK] = h;
  }
}

static uint32_t tern_ec_get_trits(zcrudp_rc_dec_t *rc, zcrudp_tec_model_t *m, size_t base) {
  static const uint32_t pow3[10] = {1, 3, 9, 27, 81, 243, 729, 2187, 6561, 19683};
  uint32_t w1 = 0;
  for (size_t j = 0; j < 10U; ++j) {
    size_t idx = base + j;
    uint32_t sctx = 0;
    uint32_t zctx = tern_ec_trit_ctx(m, idx, &sctx);
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
 * ------------------------------------------------------------------------- */
typedef void (*tern_job_fn)(void *ctx, uint32_t index);

struct tern_pool_s {
  HANDLE threads[TERN_EC_MAX_CHUNKS];
  HANDLE sem_start;
  HANDLE sem_done;
  uint32_t num_workers;
  volatile LONG next;
  LONG num_jobs;
  volatile LONG quit;
  tern_job_fn fn;
  void *ctx;
};

static void tern_pool_drain(struct tern_pool_s *pool) {
  for (;;) {
    LONG i = InterlockedIncrement(&pool->next) - 1;
    if (i >= pool->num_jobs) break;
    pool->fn(pool->ctx, (uint32_t)i);
  }
}

static DWORD WINAPI tern_pool_worker(LPVOID arg) {
  struct tern_pool_s *pool = (struct tern_pool_s *)arg;
  for (;;) {
    WaitForSingleObject(pool->sem_start, INFINITE);
    if (InterlockedCompareExchange(&pool->quit, 1, 1) == 1) break;
    tern_pool_drain(pool);
    ReleaseSemaphore(pool->sem_done, 1, NULL);
  }
  return 0;
}

static struct tern_pool_s *tern_pool_create(uint32_t num_workers) {
  struct tern_pool_s *pool = (struct tern_pool_s *)calloc(1, sizeof(*pool));
  if (!pool) return NULL;
  if (num_workers > TERN_EC_MAX_CHUNKS) num_workers = TERN_EC_MAX_CHUNKS;
  pool->sem_start = CreateSemaphoreA(NULL, 0, (LONG)TERN_EC_MAX_CHUNKS, NULL);
  pool->sem_done = CreateSemaphoreA(NULL, 0, (LONG)TERN_EC_MAX_CHUNKS, NULL);
  if (pool->sem_start && pool->sem_done) {
    for (uint32_t i = 0; i < num_workers; ++i) {
      HANDLE h = CreateThread(NULL, 0, tern_pool_worker, pool, 0, NULL);
      if (!h) break;
      pool->threads[pool->num_workers++] = h;
    }
  }
  return pool; /* with 0 workers every job runs on the caller */
}

static void tern_pool_destroy(struct tern_pool_s *pool) {
  if (!pool) return;
  InterlockedExchange(&pool->quit, 1);
  if (pool->num_workers > 0U) {
    ReleaseSemaphore(pool->sem_start, (LONG)pool->num_workers, NULL);
    WaitForMultipleObjects(pool->num_workers, pool->threads, TRUE, INFINITE);
  }
  for (uint32_t i = 0; i < pool->num_workers; ++i) CloseHandle(pool->threads[i]);
  if (pool->sem_start) CloseHandle(pool->sem_start);
  if (pool->sem_done) CloseHandle(pool->sem_done);
  free(pool);
}

/* Runs fn(ctx, 0 .. num_jobs - 1) and returns when all are done. */
static void tern_pool_run(struct tern_pool_s *pool, tern_job_fn fn, void *ctx,
                          uint32_t num_jobs) {
  if (!pool || pool->num_workers == 0U || num_jobs <= 1U) {
    for (uint32_t i = 0; i < num_jobs; ++i) fn(ctx, i);
    return;
  }
  uint32_t helpers = (num_jobs - 1U < pool->num_workers) ? (num_jobs - 1U) : pool->num_workers;
  pool->fn = fn;
  pool->ctx = ctx;
  pool->num_jobs = (LONG)num_jobs;
  InterlockedExchange(&pool->next, 0);
  ReleaseSemaphore(pool->sem_start, (LONG)helpers, NULL);
  tern_pool_drain(pool);
  for (uint32_t i = 0; i < helpers; ++i) WaitForSingleObject(pool->sem_done, INFINITE);
}

/* Scratch of one chunk coder: trit model and intra line buffers. */
static bool tern_ec_alloc(gpu_pipeline_t *p, uint32_t slot) {
  if (!p->tern_ec_line[slot]) p->tern_ec_line[slot] = (uint8_t *)malloc((size_t)p->width * 3U);
  if (!p->tern_ec_lrow[slot]) {
    p->tern_ec_lrow[slot] = (uint32_t *)malloc((size_t)p->width * sizeof(uint32_t));
  }
  if (!p->tern_ec_model[slot]) p->tern_ec_model[slot] = malloc(sizeof(zcrudp_tec_model_t));
  return p->tern_ec_line[slot] && p->tern_ec_lrow[slot] && p->tern_ec_model[slot];
}

/* Chunks a frame of num_records is cut into: one per 8192 records (smaller
 * chunks would spend more on restarting the models than threads save). */
static uint32_t tern_ec_chunks(const gpu_pipeline_t *p, uint32_t num_records) {
  uint32_t k = num_records / 8192U;
  if (k > p->tern_ec_threads) k = p->tern_ec_threads;
  return (k < 1U) ? 1U : k;
}

/* Worst-case size of a coded chunk the encoder is willing to produce. Real
 * streams are far smaller; one that is not is sent in the plain layout. */
static size_t tern_ec_chunk_bound(uint32_t num_records) {
  return 64U + (size_t)num_records * 12U;
}

/* Range-codes sorted records keys[0 .. num) into out. Returns the stream
 * size, or 0 when they cannot be coded (output full, or a record the layout
 * does not cover). */

/* ---------------------------------------------------------------------------
 * Transform coefficients (class 8), coded block by block. A record holds one
 * coefficient; within a chunk the records of a 4x4 block are consecutive
 * (sorted by block, plane, zigzag index). Per block: distance to the previous
 * coded block in blocks, its prediction bit; per plane: present, qp (when it
 * changed), coefficient
 * count, then per coefficient the zigzag step and the level. Coding positions
 * as plain gaps in coefficient units cost about 19 bits per scattered block.
 * ------------------------------------------------------------------------- */
#define TERN_TQ_DK_CTX 4U
#define TERN_TQ_LEN_CTX 6U

typedef struct {
  zcrudp_rc_prob_t pred;
  zcrudp_rc_prob_t qp_same[2];
  zcrudp_rc_prob_t present[3];
  zcrudp_rc_prob_t count[3][TERN_TQ_LEN_CTX];
  zcrudp_rc_prob_t dk[2][TERN_TQ_DK_CTX][TERN_TQ_LEN_CTX];
} tern_tq_model_t;

static void tern_tq_model_reset(tern_tq_model_t *m) {
  zcrudp_rc_prob_init(&m->pred, 16384U);
  for (int i = 0; i < 2; ++i) zcrudp_rc_prob_init(&m->qp_same[i], 30000U);
  zcrudp_rc_prob_init(&m->present[0], 30000U);
  zcrudp_rc_prob_init(&m->present[1], 16384U);
  zcrudp_rc_prob_init(&m->present[2], 16384U);
  for (int pl = 0; pl < 3; ++pl)
    for (int c = 0; c < (int)TERN_TQ_LEN_CTX; ++c) zcrudp_rc_prob_init(&m->count[pl][c], 16384U);
  for (int ch = 0; ch < 2; ++ch)
    for (int b = 0; b < (int)TERN_TQ_DK_CTX; ++b)
      for (int c = 0; c < (int)TERN_TQ_LEN_CTX; ++c) zcrudp_rc_prob_init(&m->dk[ch][b][c], 20000U);
}

static uint32_t tern_tq_dk_bucket(uint32_t k_next) {
  return (k_next < 2U) ? 0U : (k_next < 5U) ? 1U : (k_next < 9U) ? 2U : 3U;
}

static uint32_t tern_tq_level_bucket(uint32_t k) {
  return (k == 0U) ? 5U : (k < 3U) ? 4U : (k < 6U) ? 3U : (k < 10U) ? 2U : 1U;
}

/* Codes keys[0 .. num) (all class 8, sorted). Returns false when the records
 * break the layout (the caller then falls back on the plain packing). */
static bool tern_tq_put(zcrudp_rc_enc_t *rc, zcrudp_tec_model_t *tm, tern_ec_intra_t *im,
                        const uint32_t *rec, const uint64_t *keys, uint32_t num,
                        uint32_t nblocks) {
  tern_tq_model_t m;
  tern_tq_model_reset(&m);
  uint32_t last_qp[2] = {64U, 64U};
  uint32_t prev_b = UINT32_MAX;
  uint32_t i = 0;
  while (i < num) {
    uint32_t id0 = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
    uint32_t b = id0 / 48U;
    if (b >= nblocks || (prev_b != UINT32_MAX && b <= prev_b)) return false;
    if (prev_b == UINT32_MAX) {
      zcrudp_rc_enc_direct(rc, b, 24U);
    } else {
      zcrudp_tec_enc_run(rc, tm, (uint64_t)(b - prev_b - 1U));
    }
    prev_b = b;
    /* this block's records: [i, e) */
    uint32_t e = i;
    while (e < num && ((uint32_t)(keys[e] >> 32) & 0x0FFFFFFFU) / 48U == b) e++;
    uint32_t pred = (rec[(uint32_t)keys[i] * 2U + 1U] >> 24) & 0xFFU;
    if (pred > 1U) return false;
    zcrudp_rc_enc_bit(rc, &m.pred, pred);
    uint32_t j = i;
    for (uint32_t pl = 0; pl < 3U; ++pl) {
      uint32_t pe = j;
      while (pe < e && (((uint32_t)(keys[pe] >> 32) & 0x0FFFFFFFU) % 48U) / 16U == pl) pe++;
      uint32_t n = pe - j;
      uint32_t chroma = (pl > 0U) ? 1U : 0U;
      /* Every plane has a presence bit: a chunk may start in the middle of a
       * block, so even the luma (whose DC always goes out) can be absent. */
      zcrudp_rc_enc_bit(rc, &m.present[pl], n ? 1U : 0U);
      if (n == 0U) continue;
      uint32_t qp = (rec[(uint32_t)keys[j] * 2U + 1U] >> 16) & 0xFFU;
      if (qp > 51U) return false;
      zcrudp_rc_enc_bit(rc, &m.qp_same[chroma], (qp != last_qp[chroma]) ? 1U : 0U);
      if (qp != last_qp[chroma]) zcrudp_rc_enc_direct(rc, qp, 6U);
      last_qp[chroma] = qp;
      zcrudp_rc_enc_egk(rc, m.count[pl], TERN_TQ_LEN_CTX, 4U, n - 1U);
      int32_t prev_k = -1;
      for (uint32_t t = j; t < pe; ++t) {
        uint32_t src = (uint32_t)keys[t];
        uint32_t id = (uint32_t)(keys[t] >> 32) & 0x0FFFFFFFU;
        uint32_t w1 = rec[src * 2U + 1U];
        uint32_t k = id % 16U;
        int32_t level = (int32_t)(int16_t)(w1 & 0xFFFFU);
        if (((w1 >> 16) & 0xFFU) != qp || ((w1 >> 24) & 0xFFU) != pred) return false;
        if ((int32_t)k <= prev_k || level < -255 || level > 255) return false;
        /* the last coefficient of a plane needs no step past 15 - n left */
        uint32_t dk = k - (uint32_t)(prev_k + 1);
        zcrudp_rc_enc_egk(rc, m.dk[chroma][tern_tq_dk_bucket((uint32_t)(prev_k + 1))],
                          TERN_TQ_LEN_CTX, 4U, dk);
        zcrudp_iec_enc_residual(rc, chroma ? &im->chroma : &im->luma, tern_tq_level_bucket(k),
                                (k == 0U) ? 1U : 0U, level);
        prev_k = (int32_t)k;
      }
      j = pe;
    }
    if (j != e) return false;
    if (rc->overflow) return false;
    i = e;
  }
  return true;
}

/* Decodes count records into out (2 words each). Returns false on a
 * malformed stream. */
static bool tern_tq_get(zcrudp_rc_dec_t *rc, zcrudp_tec_model_t *tm, tern_ec_intra_t *im,
                        uint32_t count, uint32_t *out, uint32_t nblocks) {
  tern_tq_model_t m;
  tern_tq_model_reset(&m);
  uint32_t last_qp[2] = {64U, 64U};
  uint32_t prev_b = UINT32_MAX;
  uint32_t got = 0;
  while (got < count) {
    uint32_t b;
    if (prev_b == UINT32_MAX) {
      b = (uint32_t)zcrudp_rc_dec_direct(rc, 24U);
    } else {
      uint64_t gap = zcrudp_tec_dec_run(rc, tm);
      if (gap >= nblocks) return false;
      b = prev_b + 1U + (uint32_t)gap;
    }
    if (b >= nblocks) return false;
    prev_b = b;
    uint32_t pred = zcrudp_rc_dec_bit(rc, &m.pred);
    for (uint32_t pl = 0; pl < 3U; ++pl) {
      uint32_t chroma = (pl > 0U) ? 1U : 0U;
      if (zcrudp_rc_dec_bit(rc, &m.present[pl]) == 0U) continue;
      if (zcrudp_rc_dec_bit(rc, &m.qp_same[chroma]) != 0U) {
        last_qp[chroma] = (uint32_t)zcrudp_rc_dec_direct(rc, 6U);
      }
      uint32_t qp = last_qp[chroma];
      if (qp > 51U) return false;
      uint32_t n = (uint32_t)zcrudp_rc_dec_egk(rc, m.count[pl], TERN_TQ_LEN_CTX, 4U) + 1U;
      if (n > 16U || n > count - got) return false;
      int32_t prev_k = -1;
      for (uint32_t t = 0; t < n; ++t) {
        uint32_t dk = (uint32_t)zcrudp_rc_dec_egk(rc, m.dk[chroma][tern_tq_dk_bucket((uint32_t)(prev_k + 1))],
                                                  TERN_TQ_LEN_CTX, 4U);
        uint32_t k = (uint32_t)(prev_k + 1) + dk;
        if (k > 15U) return false;
        int32_t level = zcrudp_iec_dec_residual(rc, chroma ? &im->chroma : &im->luma,
                                                tern_tq_level_bucket(k), (k == 0U) ? 1U : 0U);
        out[got * 2U] = 0xF0000000U | (b * 48U + pl * 16U + k);
        out[got * 2U + 1U] = ((uint32_t)level & 0xFFFFU) | (qp << 16) | (pred << 24);
        got++;
        prev_k = (int32_t)k;
      }
    }
  }
  return true;
}

static size_t tern_ec_put_chunk(gpu_pipeline_t *p, uint32_t slot, const uint32_t *rec,
                                const uint64_t *keys, uint32_t num, uint32_t ncls,
                                uint8_t *out, size_t out_cap) {
  zcrudp_tec_model_t *tm = (zcrudp_tec_model_t *)p->tern_ec_model[slot];
  const uint32_t num_px = p->width * p->height;
  uint32_t counts[TERN_CLASSES_MAX] = {0};
  for (uint32_t n = 0; n < num; ++n) counts[keys[n] >> 60]++;

  zcrudp_rc_enc_t rc;
  zcrudp_rc_enc_init(&rc, out, out_cap);
  tern_ec_intra_t im;
  uint32_t i = 0;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    if (counts[cls] >> TERN_EC_COUNT_BITS) return 0;
    zcrudp_rc_enc_direct(&rc, counts[cls], TERN_EC_COUNT_BITS);
    if (counts[cls] == 0U) continue;
    zcrudp_tec_model_reset(tm, (cls < 6U) ? tern_ec_delta_stride(p->width, cls) : 0U, 1U);
    if (cls >= 6U) {
      tern_ec_intra_reset(&im, p->tern_ec_line[slot], p->tern_ec_lrow[slot], p->width);
    }
    if (cls == 8U) {
      if (!tern_tq_put(&rc, tm, &im, rec, keys + i, counts[cls], tern_ec_tq_limit(p) / 48U)) return 0;
      i += counts[cls];
      continue;
    }
    /* Positions count blocks (delta), pixels (2-px intra) or pixel pairs
     * (4-px intra). span is a record's extent in those units, px in pixels. */
    const uint32_t unit = (cls == 7U) ? 2U : 1U;
    const uint32_t px = (cls < 6U) ? 0U : (cls == 6U) ? 2U : 4U;
    const uint32_t span = (cls < 6U) ? 1U : px / unit;
    const uint32_t id_limit = tern_ec_id_limit(num_px, cls);
    uint32_t next = 0; /* first position the next record may take */
    for (uint32_t n = 0; n < counts[cls]; ++n, ++i) {
      uint32_t src = (uint32_t)keys[i];
      uint32_t id = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
      uint32_t w0 = rec[src * 2U], w1 = rec[src * 2U + 1U];
      /* Duplicated, overlapping or out-of-frame record: not representable. */
      if (id < next || id >= id_limit) return 0;
      zcrudp_tec_enc_run(&rc, tm, id - next);
      if (cls < 6U) {
        if (w1 >= 59049U) return 0;
        zcrudp_tec_hist_clear(tm, (size_t)next * 10U, (size_t)id * 10U);
        tern_ec_put_trits(&rc, tm, (size_t)id * 10U, w1);
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
        uint8_t cb8 = (cls == 6U) ? (uint8_t)cb : tern_ec_expand6(cb);
        uint8_t cr8 = (cls == 6U) ? (uint8_t)cr : tern_ec_expand6(cr);
        tern_ec_nb_t nb;
        uint32_t grad = 0, pred;
        tern_ec_neighbours(&im, id * unit, &nb);
        pred = tern_ec_predict(&nb, 1U, &grad);
        tern_ec_put_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift, cb);
        pred = tern_ec_predict(&nb, 2U, &grad);
        tern_ec_put_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift, cr);
        for (unsigned k = 0; k < px; ++k) {
          if (k > 0U) tern_ec_neighbours(&im, id * unit + k, &nb);
          pred = tern_ec_predict(&nb, 0U, &grad);
          tern_ec_put_sample(&rc, &im.luma, &im.act_y, pred, grad, shift, y[k]);
          tern_ec_store(&im, id * unit + k, (cls == 6U) ? (uint8_t)y[k] : tern_ec_expand6(y[k]),
                        cb8, cr8);
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
static uint32_t tern_ec_get_chunk(gpu_pipeline_t *p, uint32_t slot, const uint8_t *in,
                                  size_t in_len, uint32_t *out, uint32_t max_words,
                                  uint32_t ncls) {
  zcrudp_tec_model_t *tm = (zcrudp_tec_model_t *)p->tern_ec_model[slot];
  const uint32_t num_px = p->width * p->height;
  zcrudp_rc_dec_t rc;
  zcrudp_rc_dec_init(&rc, in, in_len);
  tern_ec_intra_t im;
  uint32_t words = 0;
  for (uint32_t cls = 0; cls < ncls; ++cls) {
    uint32_t count = (uint32_t)zcrudp_rc_dec_direct(&rc, TERN_EC_COUNT_BITS);
    if (count == 0U) continue;
    if (count > (max_words - words) / 2U) return UINT32_MAX;
    zcrudp_tec_model_reset(tm, (cls < 6U) ? tern_ec_delta_stride(p->width, cls) : 0U, 1U);
    if (cls >= 6U) {
      tern_ec_intra_reset(&im, p->tern_ec_line[slot], p->tern_ec_lrow[slot], p->width);
    }
    if (cls == 8U) {
      if (!tern_tq_get(&rc, tm, &im, count, out + words, tern_ec_tq_limit(p) / 48U)) return UINT32_MAX;
      words += count * 2U;
      continue;
    }
    /* Positions count blocks (delta), pixels (2-px intra) or pixel pairs
     * (4-px intra). span is a record's extent in those units, px in pixels. */
    const uint32_t unit = (cls == 7U) ? 2U : 1U;
    const uint32_t px = (cls < 6U) ? 0U : (cls == 6U) ? 2U : 4U;
    const uint32_t span = (cls < 6U) ? 1U : px / unit;
    const uint32_t id_limit = tern_ec_id_limit(num_px, cls);
    uint32_t next = 0;
    for (uint32_t n = 0; n < count; ++n) {
      uint64_t run = zcrudp_tec_dec_run(&rc, tm);
      if (run >= id_limit || next + (uint32_t)run >= id_limit) return UINT32_MAX;
      uint32_t id = next + (uint32_t)run;
      uint32_t w0, w1;
      if (cls < 6U) {
        zcrudp_tec_hist_clear(tm, (size_t)next * 10U, (size_t)id * 10U);
        w1 = tern_ec_get_trits(&rc, tm, (size_t)id * 10U);
        w0 = ((cls >> 1) << 30) | ((cls & 1U) << 29) | id;
      } else {
        const unsigned shift = (cls == 6U) ? 0U : 2U;
        uint32_t y[4] = {0, 0, 0, 0}, cb, cr;
        tern_ec_nb_t nb;
        uint32_t grad = 0, pred;
        tern_ec_neighbours(&im, id * unit, &nb);
        pred = tern_ec_predict(&nb, 1U, &grad);
        cb = tern_ec_get_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift);
        pred = tern_ec_predict(&nb, 2U, &grad);
        cr = tern_ec_get_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift);
        uint8_t cb8 = (cls == 6U) ? (uint8_t)cb : tern_ec_expand6(cb);
        uint8_t cr8 = (cls == 6U) ? (uint8_t)cr : tern_ec_expand6(cr);
        for (unsigned k = 0; k < px; ++k) {
          if (k > 0U) tern_ec_neighbours(&im, id * unit + k, &nb);
          pred = tern_ec_predict(&nb, 0U, &grad);
          y[k] = tern_ec_get_sample(&rc, &im.luma, &im.act_y, pred, grad, shift);
          tern_ec_store(&im, id * unit + k, (cls == 6U) ? (uint8_t)y[k] : tern_ec_expand6(y[k]),
                        cb8, cr8);
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
  return words;
}

typedef struct {
  gpu_pipeline_t *p;
  const uint32_t *rec;      /* encoder: raw records */
  const uint64_t *keys;     /* encoder: sorted keys */
  uint32_t first[TERN_EC_MAX_CHUNKS + 1U]; /* encoder: first record of each chunk */
  uint8_t *buf[TERN_EC_MAX_CHUNKS];        /* coded stream of each chunk */
  size_t cap[TERN_EC_MAX_CHUNKS];          /* encoder: room; decoder: stream size */
  size_t len[TERN_EC_MAX_CHUNKS];          /* encoder: coded size, 0 = failed */
  uint32_t *out[TERN_EC_MAX_CHUNKS];       /* decoder: where the chunk's words go */
  uint32_t words[TERN_EC_MAX_CHUNKS];      /* decoder: words announced */
  bool ok[TERN_EC_MAX_CHUNKS];             /* decoder: chunk decoded as announced */
  uint32_t ncls;                           /* record classes in the layout */
} tern_ec_job_t;

static void tern_ec_put_job(void *ctx, uint32_t i) {
  tern_ec_job_t *j = (tern_ec_job_t *)ctx;
  j->len[i] = tern_ec_put_chunk(j->p, i, j->rec, j->keys + j->first[i],
                                j->first[i + 1U] - j->first[i], j->ncls, j->buf[i], j->cap[i]);
}

static void tern_ec_get_job(void *ctx, uint32_t i) {
  tern_ec_job_t *j = (tern_ec_job_t *)ctx;
  j->ok[i] = tern_ec_get_chunk(j->p, i, j->buf[i], j->cap[i], j->out[i], j->words[i],
                               j->ncls) == j->words[i];
}

/* Range-coded payload: [u8 chunks][per chunk: u32 record words, u32 bytes]
 * [chunk streams][plain packed records, when the chunks do not hold them all].
 * At most ec_records are range-coded, taken from the end of the sorted order
 * (intra first: that is where a coded record saves the most); the others
 * follow in the plain packed layout. Returns the payload size, or 0 when the
 * records cannot be range-coded (no memory, output full, or a record the
 * layout does not cover): the caller then falls back on tern_pack_records. */
static size_t tern_ec_pack_records(gpu_pipeline_t *p, const uint32_t *rec,
                                   uint32_t num_records, uint32_t ec_records,
                                   uint8_t *out, size_t out_cap) {
  uint32_t counts[TERN_CLASSES_MAX];
  const uint64_t *all_keys = tern_sort_records(p, rec, &num_records, counts);
  if (!all_keys) return 0;
  if (ec_records > num_records) ec_records = num_records;
  if (ec_records == 0U) return 0;
  const uint32_t plain_records = num_records - ec_records;
  const uint64_t *keys = all_keys + plain_records;
  num_records = ec_records;

  tern_ec_job_t job;
  job.p = p;
  job.rec = rec;
  job.keys = keys;
  job.ncls = p->tern_ncls;
  const uint32_t chunks = tern_ec_chunks(p, num_records);
  size_t need = 0;
  for (uint32_t c = 0; c <= chunks; ++c) {
    job.first[c] = (uint32_t)(((uint64_t)num_records * c) / chunks);
  }
  for (uint32_t c = 0; c < chunks; ++c) {
    if (!tern_ec_alloc(p, c)) return 0;
    job.cap[c] = tern_ec_chunk_bound(job.first[c + 1U] - job.first[c]);
    need += job.cap[c];
  }
  if (need > p->tern_ec_scratch_cap) {
    uint8_t *s = (uint8_t *)realloc(p->tern_ec_scratch, need + need / 2U);
    if (!s) return 0;
    p->tern_ec_scratch = s;
    p->tern_ec_scratch_cap = need + need / 2U;
  }
  size_t off = 0;
  for (uint32_t c = 0; c < chunks; ++c) {
    job.buf[c] = p->tern_ec_scratch + off;
    off += job.cap[c];
  }
  if (chunks > 1U && !p->tern_pool) p->tern_pool = tern_pool_create(p->tern_ec_threads - 1U);
  tern_pool_run(p->tern_pool, tern_ec_put_job, &job, chunks);

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
    if (total + tern_pack_bound(plain_records) > out_cap) return 0;
    total += tern_pack_sorted(rec, all_keys, plain_records, p->tern_ncls, o);
  }
  return total;
}

/* Expands a range-coded payload back to 2 words per record. Returns the word
 * count, or UINT32_MAX on a malformed stream. */
static uint32_t tern_ec_unpack_records(gpu_pipeline_t *p, const uint8_t *in, size_t in_len,
                                       uint32_t *out, uint32_t max_words, uint32_t ncls) {
  if (in_len < 1U) return UINT32_MAX;
  const uint32_t chunks = in[0];
  if (chunks < 1U || chunks > TERN_EC_MAX_CHUNKS || in_len < 1U + (size_t)chunks * 8U) {
    return UINT32_MAX;
  }
  tern_ec_job_t job;
  job.p = p;
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
    if (!tern_ec_alloc(p, c)) return UINT32_MAX;
    job.buf[c] = (uint8_t *)data; /* read only */
    job.cap[c] = bytes;
    job.out[c] = out + total_words;
    job.words[c] = words;
    job.ok[c] = false;
    data += bytes;
    left -= bytes;
    total_words += words;
  }
  if (chunks > 1U && !p->tern_pool) p->tern_pool = tern_pool_create(p->tern_ec_threads - 1U);
  tern_pool_run(p->tern_pool, tern_ec_get_job, &job, chunks);
  for (uint32_t c = 0; c < chunks; ++c) {
    if (!job.ok[c]) return UINT32_MAX;
  }
  if (total_words < max_words) {
    /* The records the encoder had no time to range-code. */
    uint32_t plain = tern_unpack_records(data, left, out + total_words,
                                         max_words - total_words, ncls);
    if (plain == UINT32_MAX) return UINT32_MAX;
    total_words += plain;
  }
  return total_words;
}

/* Record words per frame a side can afford to range-code or decode. */
static uint32_t tern_ec_budget_words(const gpu_pipeline_t *p, uint32_t nsw_q8) {
  uint32_t fps = (p->fps > 0U) ? p->fps : 60U;
  uint64_t budget_ns = (1000000000ULL / fps) * TERN_EC_TIME_PCT / 100U;
  uint64_t words = (budget_ns << 8) / (nsw_q8 ? nsw_q8 : 1U);
  if (words < 16384U) words = 16384U;
  if (words > 0x1FFFFFFEU) words = 0x1FFFFFFEU;
  return (uint32_t)words & ~1U;
}

/* Folds one timing into a ns-per-word estimate. Small frames say little. */
static void tern_ec_note_time(uint32_t *nsw_q8, LARGE_INTEGER t0, LARGE_INTEGER t1,
                              uint32_t words) {
  if (words < 8192U) return;
  double ns = qpc_delta_us(t0, t1) * 1000.0;
  uint64_t sample = (uint64_t)(ns * 256.0 / (double)words);
  if (sample < 1U) sample = 1U;
  if (sample > (64U << 8)) sample = 64U << 8;
  *nsw_q8 = (uint32_t)(((uint64_t)*nsw_q8 * 7U + sample) / 8U);
}

/* Vectors as runs over the tile grid; returns 0 bytes when all are zero. */
static uint32_t tern_pack_mvs(const int32_t *mvs, uint32_t num_tiles, uint8_t *out) {
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
    *o++ = (uint8_t)(int8_t)dx;
    *o++ = (uint8_t)(int8_t)dy;
    i += run;
  }
  return (uint32_t)(o - out);
}

static bool tern_unpack_mvs(const uint8_t *in, uint32_t in_len, int32_t *dst,
                            uint32_t num_tiles) {
  uint32_t t = 0;
  for (uint32_t i = 0; i + 3U <= in_len; i += 3U) {
    uint32_t run = (uint32_t)in[i] + 1U;
    if (t + run > num_tiles) return false;
    for (uint32_t k = 0; k < run; ++k, ++t) {
      dst[t * 2U] = (int32_t)(int8_t)in[i + 1U];
      dst[t * 2U + 1U] = (int32_t)(int8_t)in[i + 2U];
    }
  }
  return t == num_tiles;
}

/* Tile motion search radius in pixels per frame (3840 px/s at 120 fps). The
 * shader handles up to 60; vectors travel as int8. */
#ifndef TERN_SEARCH_R
#define TERN_SEARCH_R 32U
#endif

/* ---------------------------------------------------------------------------
 * Compiled shader cache. D3DCompile takes seconds for these shaders; its
 * output only depends on the source text, the entry point, the profile and
 * the flags, so the bytecode is kept on disk under a hash of all four:
 * %LOCALAPPDATA%\zcrudp\shaders\<entry>-<hash>.dxbc (or the temp directory).
 * A changed shader gets a new hash, hence a fresh compile; stale files are
 * simply never read again. Any cache failure falls back to compiling.
 * ------------------------------------------------------------------------- */
#define TERN_HLSL_FLAGS D3DCOMPILE_OPTIMIZATION_LEVEL3
#define TERN_HLSL_PROFILE "cs_5_0"

static uint64_t tern_fnv1a(uint64_t h, const void *data, size_t len) {
  const uint8_t *d = (const uint8_t *)data;
  for (size_t i = 0; i < len; ++i) {
    h ^= d[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static bool tern_shader_cache_path(const char *entry, uint64_t hash, char *out, size_t cap) {
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

static uint8_t *tern_read_file(const char *path, size_t *len) {
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
  const char *prefixes[4] = {
#ifdef TERN_VID_SHADERS_DIR
    TERN_VID_SHADERS_DIR "/",
#else
    "../tern_vid/shaders/",
#endif
    "../tern_vid/shaders/",
    "../../tern_vid/shaders/",
    "shaders/"
  };
  for (int i = 0; i < 4; ++i) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", prefixes[i], filename);
    size_t src_len = 0;
    uint8_t *src = tern_read_file(path, &src_len);
    if (!src) continue;

    uint64_t h = 14695981039346656037ULL;
    h = tern_fnv1a(h, src, src_len);
    h = tern_fnv1a(h, entry, strlen(entry) + 1U);
    h = tern_fnv1a(h, TERN_HLSL_PROFILE, sizeof(TERN_HLSL_PROFILE));
    uint32_t flags = (uint32_t)TERN_HLSL_FLAGS;
    h = tern_fnv1a(h, &flags, sizeof(flags));

    char cache[MAX_PATH];
    bool have_cache_path = tern_shader_cache_path(entry, h, cache, sizeof(cache));
    if (have_cache_path) {
      size_t bc_len = 0;
      uint8_t *bc = tern_read_file(cache, &bc_len);
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
                            entry, TERN_HLSL_PROFILE, TERN_HLSL_FLAGS, 0, &code, &err);
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
static int tern_create_uint_buffer(ID3D11Device *dev, uint32_t count, ID3D11Buffer **buf,
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

static int setup_ternary_hlsl(gpu_pipeline_t *p, gpu_pipeline_info_t *info) {
  if (compile_hlsl_cs(p->dev, "ternary_encode.hlsl", "CSLumaHalf", &p->tern_cs_luma_half) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "ternary_encode.hlsl", "CSLumaQuarter", &p->tern_cs_luma_quarter) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "ternary_encode.hlsl", "CSTileMV", &p->tern_cs_tile_mv) != 0)
    return -19;
  if (compile_hlsl_cs(p->dev, "ternary_encode.hlsl", "CSEncode", &p->tern_cs_encode) != 0)
    return -20;
  if (compile_hlsl_cs(p->dev, "ternary_decode.hlsl", "CSMotionDecay", &p->tern_cs_motion_decay) != 0)
    return -21;
  if (compile_hlsl_cs(p->dev, "ternary_decode.hlsl", "CSApply", &p->tern_cs_apply) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "ternary_decode.hlsl", "CSYCbCrToRGBA", &p->tern_cs_ycbcr_to_rgba) != 0)
    return -22;
  if (compile_hlsl_cs(p->dev, "ternary_encode.hlsl", "CSIntraT", &p->tern_cs_intra_t) != 0)
    return -20;
  if (compile_hlsl_cs(p->dev, "ternary_decode.hlsl", "CSITrans", &p->tern_cs_itrans) != 0)
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
    hr = ID3D11Device_CreateTexture2D(p->dev, &ptd, NULL, &p->tern_prev_src_tex);
    if (FAILED(hr)) return -23;
    hr = ID3D11Device_CreateRenderTargetView(
        p->dev, (ID3D11Resource *)p->tern_prev_src_tex, NULL, &p->tern_prev_src_rtv);
    if (FAILED(hr)) return -23;
    hr = ID3D11Device_CreateShaderResourceView(
        p->dev, (ID3D11Resource *)p->tern_prev_src_tex, NULL, &p->tern_prev_src_srv);
    if (FAILED(hr)) return -23;
    ID3D11DeviceContext_ClearRenderTargetView(p->ctx, p->tern_prev_src_rtv, zero_rgba);

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
      hr = ID3D11Device_CreateTexture2D(p->dev, &htd, NULL, &p->tern_half_tex[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateShaderResourceView(
          p->dev, (ID3D11Resource *)p->tern_half_tex[i], NULL, &p->tern_half_srv[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateUnorderedAccessView(
          p->dev, (ID3D11Resource *)p->tern_half_tex[i], NULL, &p->tern_half_uav[i]);
      if (FAILED(hr)) return -23;
      ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->tern_half_uav[i], zero4);
    }
    htd.Width = (p->width + 3U) / 4U;
    htd.Height = (p->height + 3U) / 4U;
    for (int i = 0; i < 2; ++i) {
      hr = ID3D11Device_CreateTexture2D(p->dev, &htd, NULL, &p->tern_quarter_tex[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateShaderResourceView(
          p->dev, (ID3D11Resource *)p->tern_quarter_tex[i], NULL, &p->tern_quarter_srv[i]);
      if (FAILED(hr)) return -23;
      hr = ID3D11Device_CreateUnorderedAccessView(
          p->dev, (ID3D11Resource *)p->tern_quarter_tex[i], NULL, &p->tern_quarter_uav[i]);
      if (FAILED(hr)) return -23;
      ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->tern_quarter_uav[i], zero4);
    }
  }

  size_t num_px = (size_t)p->width * (size_t)p->height;
  p->tern_total_blocks = (uint32_t)((num_px + 9U) / 10U);
  p->tern_tiles_x = (p->width + 31U) / 32U;
  p->tern_tiles_y = (p->height + 31U) / 32U;
  uint32_t num_tiles = p->tern_tiles_x * p->tern_tiles_y;
  p->tern_max_record_words = (uint32_t)(2U * num_px + (size_t)p->tern_total_blocks * 6U);

  if (create_yuvd_pingpong(p->dev, num_px, p->tern_neutral_y, p->tern_delta_min,
                           p->tern_enc_yuvd_buf, p->tern_enc_yuvd_srv,
                           p->tern_enc_yuvd_uav) != 0) {
    return -24;
  }
  if (create_yuvd_pingpong(p->dev, num_px, p->tern_neutral_y, p->tern_delta_min,
                           p->tern_dec_yuvd_buf, p->tern_dec_yuvd_srv,
                           p->tern_dec_yuvd_uav) != 0) {
    return -24;
  }
  {
    /* Capture hints for CSTileMV: one int4 per tile, all "no hint" to start. */
    uint32_t nt = ((p->width + 31U) / 32U) * ((p->height + 31U) / 32U);
    p->tern_hint_cpu = (int32_t *)calloc((size_t)nt * 4U, sizeof(int32_t));
    D3D11_BUFFER_DESC hbd = {0};
    hbd.ByteWidth = nt * 16U;
    hbd.Usage = D3D11_USAGE_DEFAULT;
    hbd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    hbd.StructureByteStride = 16U;
    D3D11_SUBRESOURCE_DATA hinit = {0};
    hinit.pSysMem = p->tern_hint_cpu;
    if (!p->tern_hint_cpu ||
        FAILED(ID3D11Device_CreateBuffer(p->dev, &hbd, &hinit, &p->tern_hint_buf))) {
      return -24;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC hsd = {0};
    hsd.Format = DXGI_FORMAT_UNKNOWN;
    hsd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    hsd.Buffer.NumElements = nt;
    if (FAILED(ID3D11Device_CreateShaderResourceView(p->dev, (ID3D11Resource *)p->tern_hint_buf,
                                                     &hsd, &p->tern_hint_srv))) {
      return -24;
    }
  }
  p->tern_tq_blocks = ((p->width + 3U) / 4U) * ((p->height + 3U) / 4U);
  if (tern_create_uint_buffer(p->dev, p->tern_tq_blocks, &p->tern_enc_tflag_buf,
                              &p->tern_enc_tflag_uav, &p->tern_enc_tflag_srv) != 0 ||
      tern_create_uint_buffer(p->dev, p->tern_tq_blocks * 48U, &p->tern_dec_coef_buf,
                              &p->tern_dec_coef_uav, NULL) != 0 ||
      tern_create_uint_buffer(p->dev, p->tern_tq_blocks, &p->tern_dec_tflag_buf,
                              &p->tern_dec_tflag_uav, NULL) != 0) {
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
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_enc_mvs_buf[i]);
    if (FAILED(hr)) return -24;
    hr = ID3D11Device_CreateShaderResourceView(
        p->dev, (ID3D11Resource *)p->tern_enc_mvs_buf[i], &srvd, &p->tern_enc_mvs_srv[i]);
    if (FAILED(hr)) return -24;
    hr = ID3D11Device_CreateUnorderedAccessView(
        p->dev, (ID3D11Resource *)p->tern_enc_mvs_buf[i], &uavd, &p->tern_enc_mvs_uav[i]);
    if (FAILED(hr)) return -24;
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->tern_enc_mvs_uav[i], zero4);
  }

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (uint32_t s = 0; s < TERN_STG_RING; ++s) {
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_enc_mvs_stg[s]);
    if (FAILED(hr)) return -24;
  }

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = 8U;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_dec_mvs_buf);
  if (FAILED(hr)) return -24;

  memset(&srvd, 0, sizeof(srvd));
  srvd.Format = DXGI_FORMAT_UNKNOWN;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srvd.Buffer.NumElements = num_tiles;
  hr = ID3D11Device_CreateShaderResourceView(
      p->dev, (ID3D11Resource *)p->tern_dec_mvs_buf, &srvd, &p->tern_dec_mvs_srv);
  if (FAILED(hr)) return -24;

  /* Output active records buffer + triple-buffered staging (worst-case Intra + Delta) */
  UINT rec_bytes = p->tern_max_record_words * (UINT)sizeof(uint32_t);
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = rec_bytes;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_out_records_buf);
  if (FAILED(hr)) return -25;

  memset(&uavd, 0, sizeof(uavd));
  uavd.Format = DXGI_FORMAT_UNKNOWN;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = p->tern_max_record_words;
  hr = ID3D11Device_CreateUnorderedAccessView(
      p->dev, (ID3D11Resource *)p->tern_out_records_buf, &uavd, &p->tern_out_records_uav);
  if (FAILED(hr)) return -25;

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = rec_bytes;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (uint32_t s = 0; s < TERN_STG_RING; ++s) {
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_out_records_stg[s]);
    if (FAILED(hr)) return -25;
  }

  /* Atomic counter buffer (4 bytes) + triple-buffered staging + event queries */
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = 4U;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_out_counter_buf);
  if (FAILED(hr)) return -26;

  memset(&uavd, 0, sizeof(uavd));
  uavd.Format = DXGI_FORMAT_R32_TYPELESS;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = 1U;
  uavd.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  hr = ID3D11Device_CreateUnorderedAccessView(
      p->dev, (ID3D11Resource *)p->tern_out_counter_buf, &uavd, &p->tern_out_counter_uav);
  if (FAILED(hr)) return -26;

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = 4U;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  for (uint32_t s = 0; s < TERN_STG_RING; ++s) {
    hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_out_counter_stg[s]);
    if (FAILED(hr)) return -26;
    hr = ID3D11Device_CreateQuery(p->dev, &qd, &p->tern_readback_query[s]);
    if (FAILED(hr)) return -26;
  }

  /* Decoder input records (DYNAMIC) + SRV */
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = rec_bytes;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_in_records_buf);
  if (FAILED(hr)) return -27;

  memset(&srvd, 0, sizeof(srvd));
  srvd.Format = DXGI_FORMAT_UNKNOWN;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srvd.Buffer.NumElements = p->tern_max_record_words;
  hr = ID3D11Device_CreateShaderResourceView(
      p->dev, (ID3D11Resource *)p->tern_in_records_buf, &srvd, &p->tern_in_records_srv);
  if (FAILED(hr)) return -27;

  /* Decoder output texture in VRAM */
  UINT fmt_sup = 0;
  ID3D11Device_CheckFormatSupport(p->dev, DXGI_FORMAT_B8G8R8A8_UNORM, &fmt_sup);
  DXGI_FORMAT dec_fmt = (fmt_sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
      ? DXGI_FORMAT_B8G8R8A8_UNORM
      : DXGI_FORMAT_R8G8B8A8_UNORM;
  p->tern_dec_use_copy = (dec_fmt == DXGI_FORMAT_B8G8R8A8_UNORM);

  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = p->width;
  td.Height = p->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = dec_fmt;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  hr = ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &p->tern_dec_tex);
  if (FAILED(hr)) return -28;

  hr = ID3D11Device_CreateUnorderedAccessView(
      p->dev, (ID3D11Resource *)p->tern_dec_tex, NULL, &p->tern_dec_uav);
  if (FAILED(hr)) return -28;

  /* Constant buffers */
  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = sizeof(tern_enc_cb_t);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_enc_cb);
  if (FAILED(hr)) return -29;

  bd.ByteWidth = sizeof(tern_dec_cb_t);
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_dec_cb);
  if (FAILED(hr)) return -29;

  free(p->enc_out_buf);
  p->enc_out_cap = 8U + (size_t)num_tiles * 8U + (size_t)rec_bytes + (size_t)rec_bytes / 4U + 64U;
  p->enc_out_buf = (uint8_t *)malloc(p->enc_out_cap);
  if (!p->enc_out_buf) return -30;

  snprintf(info->encoder_name, sizeof(info->encoder_name),
           "Ternary YCbCr+TileMV HLSL (cs_5_0)");
  snprintf(info->decoder_name, sizeof(info->decoder_name),
           "Ternary YCbCr+TileMV HLSL (cs_5_0)");
  return 0;
}

int gpu_pipeline_create_ternary(gpu_pipeline_t **out_pipe, uint32_t width,
                                uint32_t height, uint32_t fps,
                                uint32_t threshold, uint32_t delta_step,
                                uint32_t gamma_q8, uint32_t neutral_y,
                                bool show_window, gpu_pipeline_info_t *out_info) {
  if (!out_pipe || !out_info || width == 0U || height == 0U) return -1;
  gpu_pipeline_t *p = (gpu_pipeline_t *)calloc(1, sizeof(*p));
  if (!p) return -1;

  p->width = width;
  p->height = height;
  p->fps = fps;
  p->bitrate_bps = 50000000U;
  p->use_ternary = true;
  p->tern_threshold = threshold;
  p->tern_delta_min = delta_step;
  p->tern_delta_max = 32U;
  p->tern_gamma_q8 = gamma_q8;
  p->tern_neutral_y = neutral_y;
  p->tern_rc_threshold = threshold;
  p->tern_rc_intra_err_thr = 32U;
  p->tern_rc_2x = false;
  p->tern_pack_ratio_q16 = 32768U;
  p->tern_ec_ratio_q16 = 16384U;
  p->tern_tq_qp = TERN_TQ_QP_START;
  {
    /* Every logical processor but one. What the coder may take is bounded
     * in time, not in threads: see tern_ec_budget_words. */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint32_t t = (si.dwNumberOfProcessors > 1U) ? (uint32_t)si.dwNumberOfProcessors - 1U : 1U;
    if (t > TERN_EC_MAX_CHUNKS) t = TERN_EC_MAX_CHUNKS;
    p->tern_ec_threads = t;
    /* First guess, replaced by measurements: 16 ns per word on one thread. */
    p->tern_ec_enc_nsw_q8 = (16U << 8) / t;
    p->tern_ec_dec_nsw_q8 = (16U << 8) / t;
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
  rc = setup_ternary_hlsl(p, &info);
  if (rc != 0) {
    gpu_pipeline_destroy(p);
    return rc;
  }
  *out_pipe = p;
  if (out_info) *out_info = info;
  return 0;
}

void gpu_pipeline_set_bitrate(gpu_pipeline_t *p, uint32_t bitrate_bps) {
  if (!p) return;
  p->bitrate_bps = bitrate_bps;
}

void gpu_pipeline_ternary_request_keyframe(gpu_pipeline_t *p) {
  if (p && p->use_ternary) p->tern_kf_countdown = 16U;
}

void gpu_pipeline_ternary_set_packed(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_ternary) p->tern_packed = enable;
}

void gpu_pipeline_ternary_set_transform(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_ternary) p->tern_tq = enable;
}

void gpu_pipeline_ternary_hint_stats(gpu_pipeline_t *p, uint64_t *frames, uint64_t *move_rects,
                                     uint64_t *moved_tiles, uint64_t *static_tiles) {
  if (!p) return;
  if (frames) *frames = p->hint_frames;
  if (move_rects) *move_rects = p->hint_move_rects;
  if (moved_tiles) *moved_tiles = p->hint_moved_tiles;
  if (static_tiles) *static_tiles = p->hint_static_tiles;
}

void gpu_pipeline_ternary_set_dither(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_ternary) p->tern_dither = enable;
}

void gpu_pipeline_ternary_set_qpel(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_ternary) p->tern_qpel = enable;
}

void gpu_pipeline_ternary_set_transform_inter(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_ternary) p->tern_tq_inter = enable;
}

void gpu_pipeline_ternary_set_entropy(gpu_pipeline_t *p, bool enable) {
  if (p && p->use_ternary) p->tern_ec = enable;
}

uint32_t gpu_pipeline_ternary_entropy_budget(gpu_pipeline_t *p) {
  if (!p || !p->use_ternary) return 0;
  return tern_ec_budget_words(p, p->tern_ec_dec_nsw_q8);
}

void gpu_pipeline_ternary_set_entropy_limit(gpu_pipeline_t *p, uint32_t record_words) {
  if (!p || !p->use_ternary) return;
  /* Whole records only, and never so low that the coder is turned off. */
  if (record_words != 0U && record_words < 16384U) record_words = 16384U;
  p->tern_ec_peer_limit = record_words & ~1U;
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
  if (p->tern_dec_cb) ID3D11Buffer_Release(p->tern_dec_cb);
  if (p->tern_enc_cb) ID3D11Buffer_Release(p->tern_enc_cb);
  if (p->tern_dec_uav) ID3D11UnorderedAccessView_Release(p->tern_dec_uav);
  if (p->tern_dec_tex) ID3D11Texture2D_Release(p->tern_dec_tex);
  if (p->tern_in_records_srv) ID3D11ShaderResourceView_Release(p->tern_in_records_srv);
  if (p->tern_in_records_buf) ID3D11Buffer_Release(p->tern_in_records_buf);
  if (p->tern_dec_mvs_srv) ID3D11ShaderResourceView_Release(p->tern_dec_mvs_srv);
  if (p->tern_dec_mvs_buf) ID3D11Buffer_Release(p->tern_dec_mvs_buf);
  for (int i = 0; i < 2; ++i) {
    if (p->tern_dec_yuvd_uav[i]) ID3D11UnorderedAccessView_Release(p->tern_dec_yuvd_uav[i]);
    if (p->tern_dec_yuvd_srv[i]) ID3D11ShaderResourceView_Release(p->tern_dec_yuvd_srv[i]);
    if (p->tern_dec_yuvd_buf[i]) ID3D11Buffer_Release(p->tern_dec_yuvd_buf[i]);
    if (p->tern_enc_yuvd_uav[i]) ID3D11UnorderedAccessView_Release(p->tern_enc_yuvd_uav[i]);
    if (p->tern_enc_yuvd_srv[i]) ID3D11ShaderResourceView_Release(p->tern_enc_yuvd_srv[i]);
    if (p->tern_enc_yuvd_buf[i]) ID3D11Buffer_Release(p->tern_enc_yuvd_buf[i]);
  }
  for (uint32_t s = 0; s < TERN_STG_RING; ++s) {
    if (p->tern_readback_query[s]) ID3D11Query_Release(p->tern_readback_query[s]);
    if (p->tern_out_counter_stg[s]) ID3D11Buffer_Release(p->tern_out_counter_stg[s]);
    if (p->tern_out_records_stg[s]) ID3D11Buffer_Release(p->tern_out_records_stg[s]);
    if (p->tern_enc_mvs_stg[s]) ID3D11Buffer_Release(p->tern_enc_mvs_stg[s]);
  }
  if (p->tern_out_counter_uav) ID3D11UnorderedAccessView_Release(p->tern_out_counter_uav);
  if (p->tern_out_counter_buf) ID3D11Buffer_Release(p->tern_out_counter_buf);
  if (p->tern_out_records_uav) ID3D11UnorderedAccessView_Release(p->tern_out_records_uav);
  if (p->tern_out_records_buf) ID3D11Buffer_Release(p->tern_out_records_buf);
  for (int i = 0; i < 2; ++i) {
    if (p->tern_enc_mvs_uav[i]) ID3D11UnorderedAccessView_Release(p->tern_enc_mvs_uav[i]);
    if (p->tern_enc_mvs_srv[i]) ID3D11ShaderResourceView_Release(p->tern_enc_mvs_srv[i]);
    if (p->tern_enc_mvs_buf[i]) ID3D11Buffer_Release(p->tern_enc_mvs_buf[i]);
  }
  for (int i = 0; i < 2; ++i) {
    if (p->tern_half_uav[i]) ID3D11UnorderedAccessView_Release(p->tern_half_uav[i]);
    if (p->tern_half_srv[i]) ID3D11ShaderResourceView_Release(p->tern_half_srv[i]);
    if (p->tern_half_tex[i]) ID3D11Texture2D_Release(p->tern_half_tex[i]);
    if (p->tern_quarter_uav[i]) ID3D11UnorderedAccessView_Release(p->tern_quarter_uav[i]);
    if (p->tern_quarter_srv[i]) ID3D11ShaderResourceView_Release(p->tern_quarter_srv[i]);
    if (p->tern_quarter_tex[i]) ID3D11Texture2D_Release(p->tern_quarter_tex[i]);
  }
  if (p->tern_cs_luma_quarter) ID3D11ComputeShader_Release(p->tern_cs_luma_quarter);
  if (p->tern_cs_luma_half) ID3D11ComputeShader_Release(p->tern_cs_luma_half);
  if (p->tern_prev_src_srv) ID3D11ShaderResourceView_Release(p->tern_prev_src_srv);
  if (p->tern_prev_src_rtv) ID3D11RenderTargetView_Release(p->tern_prev_src_rtv);
  if (p->tern_prev_src_tex) ID3D11Texture2D_Release(p->tern_prev_src_tex);
  free(p->tern_dec_unpack_buf);
  free(p->tern_dec_mv_scratch);
  if (p->cap_bgra_srv) ID3D11ShaderResourceView_Release(p->cap_bgra_srv);
  if (p->tern_cs_ycbcr_to_rgba) ID3D11ComputeShader_Release(p->tern_cs_ycbcr_to_rgba);
  if (p->tern_cs_apply) ID3D11ComputeShader_Release(p->tern_cs_apply);
  if (p->tern_cs_motion_decay) ID3D11ComputeShader_Release(p->tern_cs_motion_decay);
  if (p->tern_cs_encode) ID3D11ComputeShader_Release(p->tern_cs_encode);
  if (p->tern_cs_intra_t) ID3D11ComputeShader_Release(p->tern_cs_intra_t);
  if (p->tern_hint_srv) ID3D11ShaderResourceView_Release(p->tern_hint_srv);
  if (p->tern_hint_buf) ID3D11Buffer_Release(p->tern_hint_buf);
  free(p->tern_hint_cpu);
  free(p->dupl_meta);
  if (p->tern_cs_itrans) ID3D11ComputeShader_Release(p->tern_cs_itrans);
  if (p->tern_enc_tflag_srv) ID3D11ShaderResourceView_Release(p->tern_enc_tflag_srv);
  if (p->tern_enc_tflag_uav) ID3D11UnorderedAccessView_Release(p->tern_enc_tflag_uav);
  if (p->tern_enc_tflag_buf) ID3D11Buffer_Release(p->tern_enc_tflag_buf);
  if (p->tern_dec_coef_uav) ID3D11UnorderedAccessView_Release(p->tern_dec_coef_uav);
  if (p->tern_dec_coef_buf) ID3D11Buffer_Release(p->tern_dec_coef_buf);
  if (p->tern_dec_tflag_uav) ID3D11UnorderedAccessView_Release(p->tern_dec_tflag_uav);
  if (p->tern_dec_tflag_buf) ID3D11Buffer_Release(p->tern_dec_tflag_buf);
  if (p->tern_cs_tile_mv) ID3D11ComputeShader_Release(p->tern_cs_tile_mv);
  if (p->use_direct_nvenc && p->nvenc_session) {
    if (p->nvenc_bitstream) p->nvenc_fn.nvEncDestroyBitstreamBuffer(p->nvenc_session, p->nvenc_bitstream);
    if (p->nvenc_reg_res) p->nvenc_fn.nvEncUnregisterResource(p->nvenc_session, p->nvenc_reg_res);
    p->nvenc_fn.nvEncDestroyEncoder(p->nvenc_session);
  }
  if (p->nvenc_dll) FreeLibrary(p->nvenc_dll);
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
  free(p->tern_sort_a);
  free(p->tern_sort_b);
  tern_pool_destroy(p->tern_pool);
  for (uint32_t i = 0; i < TERN_EC_MAX_CHUNKS; ++i) {
    free(p->tern_ec_line[i]);
    free(p->tern_ec_lrow[i]);
    free(p->tern_ec_model[i]);
  }
  free(p->tern_ec_scratch);
  free(p->enc_out_buf);
  const bool mf_started = !p->use_ternary; /* only gpu_pipeline_create starts it */
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
  if (!p->use_ternary || !p->tern_hint_cpu || fi->TotalMetadataBufferSize == 0U) return false;
  if (fi->TotalMetadataBufferSize > p->dupl_meta_cap) {
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
  const uint32_t tx_n = p->tern_tiles_x, ty_n = p->tern_tiles_y;
  /* stream pixels per desktop pixel, Q16 */
  const uint64_t sx = ((uint64_t)p->width << 16) / (desk_w ? desk_w : 1U);
  const uint64_t sy = ((uint64_t)p->height << 16) / (desk_h ? desk_h : 1U);
  int32_t *h = p->tern_hint_cpu;
  for (uint32_t t = 0; t < tx_n * ty_n; ++t) {
    h[t * 4U] = 2; /* unchanged unless a rectangle says otherwise */
    h[t * 4U + 1U] = h[t * 4U + 2U] = h[t * 4U + 3U] = 0;
  }
  /* Tiles a rectangle touches, in stream tiles, clamped to the frame. */
#define TERN_RECT_TILES(L, T, Rt, B, X0, Y0, X1, Y1)                                   \
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
    TERN_RECT_TILES(dr[i].left, dr[i].top, dr[i].right, dr[i].bottom, x0, y0, x1, y1);
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
    TERN_RECT_TILES(d->left, d->top, d->right, d->bottom, x0, y0, x1, y1);
    for (uint32_t ty = y0; ty < y1; ++ty) {
      for (uint32_t tx = x0; tx < x1; ++tx) {
        int32_t *e = &h[(ty * tx_n + tx) * 4U];
        e[0] = 1;
        e[1] = vx;
        e[2] = vy;
      }
    }
  }
#undef TERN_RECT_TILES
  p->hint_frames++;
  p->hint_move_rects += moves;
  for (uint32_t t = 0; t < tx_n * ty_n; ++t) {
    if (h[t * 4U] == 1) p->hint_moved_tiles++;
    else if (h[t * 4U] == 2) p->hint_static_tiles++;
  }
  return true;
}

int gpu_pipeline_capture_and_encode(gpu_pipeline_t *p, uint32_t frame_idx,
                                    const uint8_t **out_bitstream,
                                    size_t *out_len, bool *out_keyframe,
                                    double *out_cap_us, double *out_enc_us) {
  LARGE_INTEGER t0, t1, t2;
  QueryPerformanceCounter(&t0);

  if (p->use_ternary && p->tern_has_prev_src) {
    ID3D11Texture2D *tmp_tex = p->cap_bgra_tex;
    p->cap_bgra_tex = p->tern_prev_src_tex;
    p->tern_prev_src_tex = tmp_tex;
    ID3D11RenderTargetView *tmp_rtv = p->cap_rtv;
    p->cap_rtv = p->tern_prev_src_rtv;
    p->tern_prev_src_rtv = tmp_rtv;
    ID3D11ShaderResourceView *tmp_srv = p->cap_bgra_srv;
    p->cap_bgra_srv = p->tern_prev_src_srv;
    p->tern_prev_src_srv = tmp_srv;
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
          p->tern_hints_set = build_capture_hints(p, &fi, dd.Width, dd.Height);
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
  if (!acquired_dupl && p->dupl_has_frame && p->use_ternary && p->tern_has_prev_src) {
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->cap_bgra_tex,
                                     (ID3D11Resource *)p->tern_prev_src_tex);
  } else if (!acquired_dupl && !p->dupl_has_frame) {
    if (p->ctx1) {
      float bg[4] = {0.10f, 0.12f, 0.16f, 1.0f};
      float hud[4] = {0.88f, 0.90f, 0.94f, 1.0f};
      float box_col[4] = {0.25f, 0.65f, 0.92f, 1.0f};
      if (p->use_ternary || !p->fallback_bg_init) {
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

  if (!p->use_direct_nvenc && !p->use_ternary) {
    /* Zero-copy GPU BGRA -> NV12 conversion in VRAM (MFT fallback only) */
    vp_blit_textures(p, src_bgra, 0U, p->enc_nv12_tex);
  }

  p->tern_hints_static = (p->dupl != NULL) && p->dupl_has_frame && !acquired_dupl;
  if (acquired_dupl) {
    IDXGIResource_Release(dupl_res);
    IDXGIOutputDuplication_ReleaseFrame(p->dupl);
  }

  QueryPerformanceCounter(&t1);

  if (p->use_ternary) {
    uint32_t ping = p->tern_enc_ping & 1U;
    uint32_t pong = ping ^ 1U;
    p->tern_enc_ping = pong;

    UINT zero4[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->tern_out_counter_uav, zero4);

    uint32_t fps = (p->fps > 0U) ? p->fps : 60U;
    uint32_t target_bytes = (p->bitrate_bps > 0U) ? (p->bitrate_bps / (8U * fps)) : 52083U;
    uint32_t num_tiles = p->tern_tiles_x * p->tern_tiles_y;
    /* Packed, a record byte costs tern_pack_ratio_q16 / 65536 wire bytes and
     * the vectors what they cost last frame: the same wire budget buys that
     * many more records. */
    uint32_t mv_reserve = p->tern_packed ? (8U + p->tern_pack_mv_bytes) : (8U + num_tiles * 2U);
    uint32_t target_words = (target_bytes > mv_reserve + 4096U)
                                ? ((target_bytes - mv_reserve) / 4U)
                                : 1024U;
    p->tern_ec_frame = false;
    if (p->tern_packed) {
      /* tern_ec_ratio_q16 is what a record byte really cost on the wire,
       * range-coded part and plain part together, so a frame larger than the
       * coder's time budget just sees its ratio drift toward the plain one. */
      uint32_t ratio = p->tern_ec ? p->tern_ec_ratio_q16 : p->tern_pack_ratio_q16;
      target_words = (uint32_t)(((uint64_t)target_words << 16) / ratio);
      p->tern_ec_frame = p->tern_ec;
    }
    /* 25 % of slack lets the controller absorb a frame of extra motion. A
     * scene it cannot bring under the budget even at its highest threshold
     * would otherwise sit on that cap for good, 25 % over the target. */
    uint32_t hard_cap_words = (p->tern_rc_threshold >= 14U)
                                  ? (target_words & ~1U)
                                  : ((target_words + (target_words >> 2)) & ~1U);
    if (hard_cap_words < 1024U) hard_cap_words = 1024U;
    if (hard_cap_words > p->tern_max_record_words) hard_cap_words = p->tern_max_record_words & ~1U;

    /* Scale rolling intra-refresh cycles so it uses ~25% of target_words */
    uint32_t intra_budget = target_words / 4U;
    uint32_t words_per_row = (p->width * 3U) / 5U;
    uint32_t rows_per_frame = (words_per_row > 0U && intra_budget >= words_per_row)
                                  ? (intra_budget / words_per_row)
                                  : 1U;
    /* A requested burst resynchronises a decoder whose reference differs from
     * the encoder's. Only intra records repair it, so for those 16 frames:
     *  - no motion search: a non-zero vector makes the decoder copy pixels
     *    that are still stale over rows intra has already repaired;
     *  - no delta records: on a moving picture they fill the record cap and
     *    the intra rows get dropped. */
    bool kf_resync = (p->tern_kf_countdown > 0U);
    bool kf_burst = (frame_idx < 16U) || kf_resync;
    uint32_t ecb_frame_id = frame_idx;
    if (p->tern_kf_countdown > 0U) {
      ecb_frame_id = 16U - p->tern_kf_countdown;
      p->tern_kf_countdown--;
    }

    uint32_t intra_cycles = (p->height + rows_per_frame - 1U) / rows_per_frame;
    if (intra_cycles < 30U) intra_cycles = 30U;
    if (intra_cycles > 480U) intra_cycles = 480U;
    if (kf_burst) {
      intra_cycles = 16U;
    } else if (p->tern_static_scene) {
      uint32_t resync_mod = (fps > 0U) ? (fps * 3U) : 360U;
      if ((frame_idx % resync_mod) < 30U) {
        intra_cycles = 30U;
      } else {
        intra_cycles = (p->height > 720U) ? p->height : 720U;
      }
    }

    uint32_t mode_flags = kf_burst ? 1U : (p->tern_rc_2x ? 1U : 0U);
    /* bit 1: full-precision rolling refresh, decided on the previous frame's
     * size so that it is the same for every block of this one. */
    if (!kf_burst && p->tern_intra8) mode_flags |= 2U;
    if (p->tern_tq) mode_flags |= 4U;
    if (p->tern_tq && p->tern_tq_inter) mode_flags |= 8U;
    if (p->tern_qpel) mode_flags |= 16U;
    if (p->tern_dither) mode_flags |= 64U;
    uint32_t eff_blocks = ((mode_flags & 1U) != 0U) ? ((p->tern_total_blocks + 1U) >> 1)
                                             : p->tern_total_blocks;
    /* A burst frame refreshes 1/16 of the rows with 4-px intra records
     * (pixels / 32 words) and needs room beyond that for the 2-px ones. */
    uint32_t burst_cap_words = (p->width * p->height) / 14U;
    if (burst_cap_words < 140000U) burst_cap_words = 140000U;
    uint32_t enc_cap_words = kf_burst ? (burst_cap_words & ~1U) : hard_cap_words;
    if (enc_cap_words > (p->tern_max_record_words & ~1U)) {
      enc_cap_words = p->tern_max_record_words & ~1U;
    }
    uint32_t enc_thr = kf_resync ? 255U : (kf_burst ? 8U : p->tern_rc_threshold);

    tern_enc_cb_t ecb = {
        p->width, p->height, enc_thr, p->tern_delta_min,
        p->tern_delta_max, p->tern_gamma_q8, p->tern_neutral_y, p->tern_total_blocks,
        ecb_frame_id, intra_cycles, kf_burst ? 255U : p->tern_rc_intra_err_thr,
        p->tern_tiles_x, p->tern_tiles_y,
        /* Frame 0 has no previous picture to search (those textures are not
         * even initialised yet). */
        (kf_resync || frame_idx == 0U) ? 0U : TERN_SEARCH_R, enc_cap_words, mode_flags,
        /* Transform intra qp follows the rate control: finer while it has
         * room (threshold near its floor), coarser under pressure. */
        TERN_TQ_ADAPTIVE ? p->tern_tq_qp : (TERN_TQ_QP_BASE + TERN_TQ_QP_PER_THR * enc_thr),
        {TERN_TQ_CQP, TERN_TQ_INTER_BASE + TERN_TQ_INTER_PER_THR * enc_thr, 0}
    };
    ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->tern_enc_cb,
                                          0, NULL, &ecb, 0, 0);

    uint32_t half_cur = p->tern_half_ping & 1U;
    uint32_t half_prev = half_cur ^ 1U;
    p->tern_half_ping = half_prev;
    uint32_t mv_cur = p->tern_mv_ping & 1U;
    uint32_t mv_prev = mv_cur ^ 1U;
    p->tern_mv_ping = mv_prev;

    ID3D11ShaderResourceView *srvs[8] = {p->cap_bgra_srv, p->tern_enc_yuvd_srv[ping],
                                         p->tern_prev_src_srv, p->tern_half_srv[half_cur],
                                         p->tern_half_srv[half_prev], p->tern_enc_mvs_srv[mv_prev],
                                         p->tern_quarter_srv[half_cur],
                                         p->tern_quarter_srv[half_prev]};
    ID3D11UnorderedAccessView *uavs[4] = {
        p->tern_enc_yuvd_uav[pong], p->tern_enc_mvs_uav[mv_cur],
        p->tern_out_records_uav, p->tern_out_counter_uav
    };
    ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->tern_enc_cb);
    /* Pass 0: CSLumaHalf + CSLumaQuarter (reduced luma of this frame for the motion search) */
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 1, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 4, 1, &p->tern_half_uav[half_cur], NULL);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_luma_half, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, ((p->width + 1U) / 2U + 15U) / 16U,
                                 ((p->height + 1U) / 2U + 15U) / 16U, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 4, 1, &p->tern_quarter_uav[half_cur], NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 3, 1, &srvs[3]);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_luma_quarter, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, ((p->width + 3U) / 4U + 15U) / 16U,
                                 ((p->height + 3U) / 4U + 15U) / 16U, 1);
    ID3D11UnorderedAccessView *no_uav = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 4, 1, &no_uav, NULL);

    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 8, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, uavs, NULL);

    /* Capture hints: this frame's move / dirty rectangles, every tile
     * unchanged when no new desktop frame came, nothing otherwise. */
    if (p->tern_hint_buf) {
      uint32_t nt = p->tern_tiles_x * p->tern_tiles_y;
      bool desk_static = !p->tern_hints_set && p->tern_hints_static;
      if (!p->tern_hints_set) {
        for (uint32_t t = 0; t < nt; ++t) {
          p->tern_hint_cpu[t * 4U] = desk_static ? 2 : 0;
          p->tern_hint_cpu[t * 4U + 1U] = p->tern_hint_cpu[t * 4U + 2U] = 0;
          p->tern_hint_cpu[t * 4U + 3U] = 0;
        }
      }
      ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->tern_hint_buf, 0, NULL,
                                            p->tern_hint_cpu, 0, 0);
      ID3D11DeviceContext_CSSetShaderResources(p->ctx, 9, 1, &p->tern_hint_srv);
      p->tern_hints_set = false;
    }
    /* Pass 1: CSTileMV (32x32 tiles) - skip cleanly when search_r == 0 */
    if (ecb.search_r == 0U) {
      ID3D11DeviceContext_ClearUnorderedAccessViewUint(p->ctx, p->tern_enc_mvs_uav[mv_cur], zero4);
    } else {
      ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_tile_mv, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, p->tern_tiles_x, p->tern_tiles_y, 1);
    }

    /* Pass 2: CSEncode (YCbCr + ADM + Rolling Intra-Refresh)
     * Unbind g_tile_mvs UAV (u1) and bind current frame's MVs as read-only SRV (t5). */
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 1, 1, &no_uav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 5, 1, &p->tern_enc_mvs_srv[mv_cur]);
    /* Pass 1b: CSIntraT (transform intra on 4x4 blocks; writes 0 flags when off) */
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 5, 1, &p->tern_enc_tflag_uav, NULL);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_intra_t, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->tern_tq_blocks + 63U) / 64U, 1, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 5, 1, &no_uav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 8, 1, &p->tern_enc_tflag_srv);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_encode, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (eff_blocks + 63U) / 64U, 1, 1);
    {
      ID3D11ShaderResourceView *null_t89[2] = {NULL, NULL};
      ID3D11DeviceContext_CSSetShaderResources(p->ctx, 8, 2, null_t89);
    }

    ID3D11ShaderResourceView *null_srv[8] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    ID3D11UnorderedAccessView *null_uav[4] = {NULL, NULL, NULL, NULL};
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 8, null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, null_uav, NULL);

    p->tern_has_prev_src = true;

    uint32_t stg_idx = p->tern_stg_idx % TERN_STG_RING;
    p->tern_stg_idx++;
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->tern_out_counter_stg[stg_idx],
                                     (ID3D11Resource *)p->tern_out_counter_buf);
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->tern_enc_mvs_stg[stg_idx],
                                     (ID3D11Resource *)p->tern_enc_mvs_buf[mv_cur]);
    /* The records go back in the same round trip, up to this frame's cap:
     * waiting for the count first and then for the records cost a second
     * GPU synchronisation (0.35 to 0.5 ms). Copying words past the count is
     * a few tens of microseconds of PCIe traffic. */
    if (enc_cap_words > 0U) {
      D3D11_BOX box = {0, 0, 0, enc_cap_words * (UINT)sizeof(uint32_t), 1, 1};
      ID3D11DeviceContext_CopySubresourceRegion(
          p->ctx, (ID3D11Resource *)p->tern_out_records_stg[stg_idx], 0, 0, 0, 0,
          (ID3D11Resource *)p->tern_out_records_buf, 0, &box);
    }
    ID3D11DeviceContext_End(p->ctx, (ID3D11Asynchronous *)p->tern_readback_query[stg_idx]);
    wait_query_event(p->ctx, p->tern_readback_query[stg_idx]);

    D3D11_MAPPED_SUBRESOURCE mr = {0};
    uint32_t raw_words = 0;
    if (SUCCEEDED(map_read_nonblocking(p->ctx, (ID3D11Resource *)p->tern_out_counter_stg[stg_idx], &mr))) {
      raw_words = *(const uint32_t *)mr.pData;
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_out_counter_stg[stg_idx], 0);
    }
    uint32_t record_words = (raw_words > enc_cap_words) ? enc_cap_words : raw_words;
    record_words &= ~1U;
    p->tern_last_raw_words = record_words;


    /* Compact MVs: 0 bytes if all (0,0), else 2 bytes/tile (int8 dx, int8 dy) */
    uint32_t mv_bytes = 0U;
    if (SUCCEEDED(map_read_nonblocking(p->ctx, (ID3D11Resource *)p->tern_enc_mvs_stg[stg_idx], &mr))) {
      const int32_t *mvs = (const int32_t *)mr.pData;
      bool any_nz = false;
      for (uint32_t i = 0; i < num_tiles; ++i) {
        if ((mvs[i * 2U] | mvs[i * 2U + 1U]) != 0) {
          any_nz = true;
          break;
        }
      }
      if (p->tern_packed) {
        mv_bytes = tern_pack_mvs(mvs, num_tiles, p->enc_out_buf + 8U);
        if (p->tern_qpel && mv_bytes > 0U) p->enc_out_buf[8U + mv_bytes++] = 0x51U;
        p->tern_pack_mv_bytes = mv_bytes;
      } else if (any_nz) {
        mv_bytes = num_tiles * 2U;
        int8_t *dst_mv = (int8_t *)(p->enc_out_buf + 8U);
        for (uint32_t i = 0; i < num_tiles; ++i) {
          dst_mv[i * 2U + 0U] = (int8_t)mvs[i * 2U + 0U];
          dst_mv[i * 2U + 1U] = (int8_t)mvs[i * 2U + 1U];
        }
        if (p->tern_qpel) p->enc_out_buf[8U + mv_bytes++] = 0x51U;
      }
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_enc_mvs_stg[stg_idx], 0);
    }
    uint32_t hdr_word = p->tern_packed ? (TERN_PACKED_FLAG | record_words) : record_words;
    p->tern_ncls = p->tern_tq ? 9U : 8U;
    if (p->tern_packed && p->tern_tq) hdr_word |= TERN_V2_FLAG;
    memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
    memcpy(p->enc_out_buf + sizeof(uint32_t), &mv_bytes, sizeof(uint32_t));

    /* Closed-loop frame rate controller (skip initial 16-frame convergence burst) */
    p->tern_static_scene = (mv_bytes == 0U && raw_words < (target_words / 2U));
    if (!kf_burst) {
      /* The transform has its own qp loop: coarser while frames exceed the
       * budget, finer while they leave room. Tying it to the delta threshold
       * made it as coarse as the deltas whenever those saturated, and the
       * transform then gave up the blocks it codes best. */
      if (raw_words > (target_words * TERN_TQ_UP_PCT) / 100U) {
        if (p->tern_tq_qp < 51U) p->tern_tq_qp++;
      } else if (raw_words < (target_words * TERN_TQ_DOWN_PCT) / 100U) {
        if (p->tern_tq_qp > TERN_TQ_QP_MIN) p->tern_tq_qp--;
      }
    }
    if (!kf_burst) {
      /* The 8-bit refresh doubles what a refreshed row costs: only with a
       * wide margin, and dropped again well before the budget is reached. */
      if (raw_words < (target_words * 2U) / 5U) {
        p->tern_intra8 = true;
      } else if (raw_words > (target_words * 3U) / 4U) {
        p->tern_intra8 = false;
      }
      if (raw_words > target_words + (target_words >> 3)) {
        if (!p->tern_rc_2x && !TERN_RC_NO_2X && raw_words > target_words + (target_words >> 1)) {
          p->tern_rc_2x = true;
        }
        if (p->tern_rc_threshold < 14U) {
        uint32_t step = (raw_words > target_words * 2U) ? 2U : 1U;
        p->tern_rc_threshold += step;
        if (p->tern_rc_threshold > 14U) p->tern_rc_threshold = 14U;
      }
      if (p->tern_rc_intra_err_thr < 96U) {
        p->tern_rc_intra_err_thr += 8U;
      }
    } else if (raw_words < (target_words * 3U) / 4U) {
      /* With bandwidth left over, go below the configured floor: the
       * threshold and the intra trigger are encoder-only decisions, so the
       * spare bits turn into quality at no cost to the decoder. */
      /* Graded: the lowest floor only with ample room, so that a frame or
       * two of extra motion cannot push the controller from far under the
       * budget straight into the hard cap (and back). */
      uint32_t thr_floor = (raw_words < (target_words * 2U) / 5U) ? TERN_RC_MIN_THRESHOLD
                         : (raw_words < (target_words * 3U) / 5U) ? TERN_RC_MIN_THRESHOLD + 1U
                                                                   : p->tern_threshold;
      if (thr_floor > p->tern_threshold) thr_floor = p->tern_threshold;
      uint32_t ierr_floor = (raw_words < (target_words * 3U) / 5U) ? TERN_RC_MIN_INTRA_ERR : 32U;
      if (p->tern_rc_threshold > thr_floor) {
        p->tern_rc_threshold--;
      }
      if (p->tern_rc_intra_err_thr > ierr_floor) {
        p->tern_rc_intra_err_thr -= 4U;
        if (p->tern_rc_intra_err_thr < ierr_floor) p->tern_rc_intra_err_thr = ierr_floor;
      }
      if (p->tern_rc_2x && raw_words < (target_words >> 1)) {
        p->tern_rc_2x = false;
      }
    }
    }

    size_t out_bytes = 8U + mv_bytes;
    if (record_words > 0) {
      if (SUCCEEDED(map_read_nonblocking(p->ctx, (ID3D11Resource *)p->tern_out_records_stg[stg_idx], &mr))) {
        size_t rec_bytes = (size_t)record_words * sizeof(uint32_t);
        size_t packed = 0;
        bool coded = false;
        bool fits = p->tern_packed &&
                    out_bytes + tern_pack_bound(record_words / 2U) <= p->enc_out_cap;
        if (fits && p->tern_ec) {
          /* As many records as this side and the decoder have time for. */
          uint32_t budget = tern_ec_budget_words(p, p->tern_ec_enc_nsw_q8);
          if (p->tern_ec_peer_limit != 0U && budget > p->tern_ec_peer_limit) {
            budget = p->tern_ec_peer_limit;
          }
          uint32_t ec_words = (record_words < budget) ? record_words : budget;
          LARGE_INTEGER q0, q1;
          QueryPerformanceCounter(&q0);
          packed = tern_ec_pack_records(p, (const uint32_t *)mr.pData, record_words / 2U,
                                        ec_words / 2U, p->enc_out_buf + out_bytes,
                                        p->enc_out_cap - out_bytes);
          QueryPerformanceCounter(&q1);
          coded = (packed > 0);
          if (coded) tern_ec_note_time(&p->tern_ec_enc_nsw_q8, q0, q1, ec_words);
        }
        if (fits && !coded) {
          packed = tern_pack_records(p, (const uint32_t *)mr.pData, record_words / 2U,
                                     p->enc_out_buf + out_bytes);
        }
        if (packed > 0) {
          out_bytes += packed;
          /* The header announces the records actually packed: slots the
           * encoder left unused are not part of the stream. */
          hdr_word = (hdr_word & ~TERN_RECORD_WORDS_MASK) | (p->tern_sort_kept * 2U);
          memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
          uint32_t ratio = (uint32_t)(((uint64_t)packed << 16) / rec_bytes);
          if (ratio > 65536U) ratio = 65536U;
          if (coded) {
            hdr_word |= TERN_EC_FLAG;
            memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
            if (ratio < 2048U) ratio = 2048U;
            /* Burst frames are not what the controller is sizing. */
            if (!kf_burst) p->tern_ec_ratio_q16 = (p->tern_ec_ratio_q16 * 7U + ratio) / 8U;
          } else {
            if (ratio < 8192U) ratio = 8192U;
            p->tern_pack_ratio_q16 = (p->tern_pack_ratio_q16 * 7U + ratio) / 8U;
          }
        } else {
          if (p->tern_packed) { /* sort scratch allocation failed: raw records */
            hdr_word |= TERN_RAW_RECORDS_FLAG;
            memcpy(p->enc_out_buf, &hdr_word, sizeof(uint32_t));
          }
          memcpy(p->enc_out_buf + out_bytes, mr.pData, rec_bytes);
          out_bytes += rec_bytes;
        }
        ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_out_records_stg[stg_idx], 0);
      }
    }

    QueryPerformanceCounter(&t2);
    if (out_cap_us) *out_cap_us = qpc_delta_us(t0, t1);
    if (out_enc_us) *out_enc_us = qpc_delta_us(t1, t2);
    if (out_bitstream) *out_bitstream = p->enc_out_buf;
    if (out_len) *out_len = out_bytes;
    if (out_keyframe) *out_keyframe = (frame_idx == 0) || (fps > 0U && (frame_idx % fps) == 0U);
    return 0;
  }

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

  if (p->use_ternary) {
    if (len < 8U) return -1;
    uint32_t hdr_word = 0, record_words = 0, mv_bytes = 0;
    memcpy(&hdr_word, bitstream, sizeof(uint32_t));
    memcpy(&mv_bytes, bitstream + sizeof(uint32_t), sizeof(uint32_t));
    bool packed = (hdr_word & TERN_PACKED_FLAG) != 0U;
    bool packed_records = packed && (hdr_word & TERN_RAW_RECORDS_FLAG) == 0U;
    record_words = packed ? (hdr_word & TERN_RECORD_WORDS_MASK) : hdr_word;
    const uint32_t ncls = (packed && (hdr_word & TERN_V2_FLAG) != 0U) ? 9U : 8U;
    bool mv_qpel = false;
    if (record_words > p->tern_max_record_words) record_words = p->tern_max_record_words;
    if (mv_bytes > len - 8U) return -1;
    if (!packed_records &&
        len < 8U + (size_t)mv_bytes + (size_t)record_words * sizeof(uint32_t)) return -1;

    uint32_t num_tiles = p->tern_tiles_x * p->tern_tiles_y;
    if (num_tiles > p->tern_dec_mv_cap) {
      int32_t *nb = (int32_t *)realloc(p->tern_dec_mv_scratch,
                                       (size_t)num_tiles * 2U * sizeof(int32_t));
      if (!nb) return -1;
      p->tern_dec_mv_scratch = nb;
      p->tern_dec_mv_cap = num_tiles;
    }
    const void *mv_src = NULL;
    uint32_t mv_copy_bytes = num_tiles * 8U;
    /* Quarter-pel vectors are followed by one marker byte: runs of 3 bytes
     * plus 1 when packed, 2 bytes per tile plus 1 when raw. */
    mv_qpel = (mv_bytes != 0U) && (packed ? (mv_bytes % 3U == 1U) : (mv_bytes == num_tiles * 2U + 1U));
    if (mv_bytes == 0U) {
      mv_src = NULL;
    } else if (packed) {
      if (!tern_unpack_mvs(bitstream + 8U, mv_bytes, p->tern_dec_mv_scratch, num_tiles)) {
        return -1;
      }
      mv_src = p->tern_dec_mv_scratch;
    } else if (mv_bytes == num_tiles * 2U || mv_qpel) {
      const int8_t *src_mv = (const int8_t *)(bitstream + 8U);
      for (uint32_t i = 0; i < num_tiles; ++i) {
        p->tern_dec_mv_scratch[i * 2U + 0U] = (int32_t)src_mv[i * 2U + 0U];
        p->tern_dec_mv_scratch[i * 2U + 1U] = (int32_t)src_mv[i * 2U + 1U];
      }
      mv_src = p->tern_dec_mv_scratch;
    } else {
      mv_copy_bytes = (mv_bytes <= num_tiles * 8U) ? mv_bytes : (num_tiles * 8U);
      mv_src = bitstream + 8U;
    }

    D3D11_MAPPED_SUBRESOURCE mw = {0};
    if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->tern_dec_mvs_buf,
                                       0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
      return -1;
    }
    if (!mv_src) {
      memset(mw.pData, 0, (size_t)num_tiles * 8U);
    } else {
      memcpy(mw.pData, mv_src, mv_copy_bytes);
    }
    ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_dec_mvs_buf, 0);

    if (record_words > 0) {
      const void *rec_src = NULL;
      uint32_t got_words = record_words;
      if (packed_records) {
        if ((size_t)record_words > p->tern_dec_unpack_cap) {
          uint32_t *nb = (uint32_t *)realloc(p->tern_dec_unpack_buf,
                                             (size_t)record_words * sizeof(uint32_t));
          if (!nb) return -1;
          p->tern_dec_unpack_buf = nb;
          p->tern_dec_unpack_cap = record_words;
        }
        if ((hdr_word & TERN_EC_FLAG) != 0U) {
          LARGE_INTEGER q0, q1;
          QueryPerformanceCounter(&q0);
          got_words = tern_ec_unpack_records(p, bitstream + 8U + mv_bytes, len - 8U - mv_bytes,
                                             p->tern_dec_unpack_buf, record_words, ncls);
          QueryPerformanceCounter(&q1);
          if (got_words != UINT32_MAX) {
            tern_ec_note_time(&p->tern_ec_dec_nsw_q8, q0, q1, got_words);
          }
        } else {
          got_words = tern_unpack_records(bitstream + 8U + mv_bytes, len - 8U - mv_bytes,
                                          p->tern_dec_unpack_buf, record_words, ncls);
        }
        if (got_words == UINT32_MAX) return -1;
        rec_src = p->tern_dec_unpack_buf;
      } else {
        rec_src = bitstream + 8U + mv_bytes;
      }
      if (got_words > 0) {
        if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->tern_in_records_buf,
                                           0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
          return -1;
        }
        memcpy(mw.pData, rec_src, (size_t)got_words * sizeof(uint32_t));
        ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_in_records_buf, 0);
      }
      record_words = got_words;
    }

    uint32_t ping = p->tern_dec_ping & 1U;
    uint32_t pong = ping ^ 1U;
    p->tern_dec_ping = pong;

    tern_dec_cb_t dcb = {
        p->width, p->height, p->tern_delta_min, p->tern_delta_max,
        p->tern_gamma_q8, p->tern_neutral_y, record_words, p->tern_tiles_x,
        mv_qpel ? 1U : 0U, {TERN_POST_THR, 0, 0}
    };
    ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->tern_dec_cb,
                                          0, NULL, &dcb, 0, 0);

    ID3D11ShaderResourceView *srvs[3] = {
        p->tern_dec_mvs_srv, p->tern_in_records_srv, p->tern_dec_yuvd_srv[ping]
    };
    ID3D11UnorderedAccessView *uavs[4] = {
        p->tern_dec_yuvd_uav[pong], p->tern_dec_uav, p->tern_dec_coef_uav, p->tern_dec_tflag_uav
    };
    ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->tern_dec_cb);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, uavs, NULL);

    /* Pass 1: CSMotionDecay */
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_motion_decay, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U,
                                 (p->height + 15U) / 16U, 1);

    /* Pass 2: CSApply */
    if (record_words >= 2U) {
      ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_apply, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, ((record_words / 2U) + 63U) / 64U, 1, 1);
      /* Pass 2b: CSITrans rebuilds the transform-coded blocks (only flagged
       * ones do any work). */
      ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_itrans, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, (p->tern_tq_blocks + 63U) / 64U, 1, 1);
    }

    /* Pass 3: CSYCbCrToRGBA
     * Unbind g_cur_yuvd UAV (u0) and bind decoded cur_yuvd as read-only SRV (t2). */
    ID3D11UnorderedAccessView *null_uav0 = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 1, &null_uav0, NULL);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 2, 1, &p->tern_dec_yuvd_srv[pong]);
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_ycbcr_to_rgba, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U,
                                 (p->height + 15U) / 16U, 1);

    ID3D11ShaderResourceView *null_srv[3] = {NULL, NULL, NULL};
    ID3D11UnorderedAccessView *null_uav[4] = {NULL, NULL, NULL, NULL};
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, null_uav, NULL);

    QueryPerformanceCounter(&t1);

    ID3D11Texture2D *backbuf = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                            (void **)&backbuf))) {
      if (p->tern_dec_use_copy) {
        ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)backbuf,
                                         (ID3D11Resource *)p->tern_dec_tex);
      } else {
        if (vp_blit_textures(p, p->tern_dec_tex, 0U, backbuf) != 0) {
          static int blit_err = 0;
          if (blit_err++ < 5) gpu_log_err("[GPU][DEC] ternary vp_blit_textures failed!\n");
        }
      }
      ID3D11Texture2D_Release(backbuf);
    }
    UINT present_flags = p->tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    IDXGISwapChain1_Present(p->swapchain, 0, present_flags);

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

    UINT present_flags = p->tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr_prs = IDXGISwapChain1_Present(p->swapchain, 0, present_flags);
    if (FAILED(hr_prs) && dec_err_count++ < 10) {
      gpu_log_err("[GPU][DEC] SwapChain Present failed: hr=0x%08lx\n",
                  (unsigned long)hr_prs);
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

int gpu_pipeline_ternary_compute_psnr(gpu_pipeline_t *p, double *out_psnr_db) {
  if (!p || !p->use_ternary || !out_psnr_db) return -1;

  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = p->width;
  td.Height = p->height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  ID3D11Texture2D *stg_src = NULL;
  if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &stg_src))) return -2;

  td.Format = p->tern_dec_use_copy ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  ID3D11Texture2D *stg_dec = NULL;
  if (FAILED(ID3D11Device_CreateTexture2D(p->dev, &td, NULL, &stg_dec))) {
    ID3D11Texture2D_Release(stg_src);
    return -2;
  }

  ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)stg_src,
                                   (ID3D11Resource *)p->cap_bgra_tex);
  ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)stg_dec,
                                   (ID3D11Resource *)p->tern_dec_tex);

  D3D11_MAPPED_SUBRESOURCE ms = {0}, md = {0};
  if (FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)stg_src, 0, D3D11_MAP_READ, 0, &ms)) ||
      FAILED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)stg_dec, 0, D3D11_MAP_READ, 0, &md))) {
    ID3D11Texture2D_Release(stg_src);
    ID3D11Texture2D_Release(stg_dec);
    return -3;
  }

  double mse = 0.0;
  for (uint32_t y = 0; y < p->height; ++y) {
    const uint8_t *srow = (const uint8_t *)ms.pData + (size_t)y * ms.RowPitch;
    const uint8_t *drow = (const uint8_t *)md.pData + (size_t)y * md.RowPitch;
    for (uint32_t x = 0; x < p->width; ++x) {
      /* cap_bgra_tex is BGRA */
      double b = (double)srow[x * 4U + 0U];
      double g = (double)srow[x * 4U + 1U];
      double r = (double)srow[x * 4U + 2U];
      double sy = 0.299 * r + 0.587 * g + 0.114 * b;
      double db = (double)drow[x * 4U + (p->tern_dec_use_copy ? 0U : 2U)];
      double dg = (double)drow[x * 4U + 1U];
      double dr = (double)drow[x * 4U + (p->tern_dec_use_copy ? 2U : 0U)];
      double dy = 0.299 * dr + 0.587 * dg + 0.114 * db;
      double diff = sy - dy;
      mse += diff * diff;
    }
  }

  ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)stg_src, 0);
  ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)stg_dec, 0);
  ID3D11Texture2D_Release(stg_src);
  ID3D11Texture2D_Release(stg_dec);

  mse /= (double)((size_t)p->width * (size_t)p->height);
  *out_psnr_db = (mse < 1e-9) ? 99.0 : (10.0 * log10((255.0 * 255.0) / mse));
  return 0;
}

int gpu_pipeline_ternary_ref_mismatch(gpu_pipeline_t *p, uint64_t *out_mismatch_px) {
  if (!p || !p->use_ternary || !out_mismatch_px) return -1;

  size_t num_px = (size_t)p->width * (size_t)p->height;
  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = (UINT)(num_px * sizeof(uint32_t));
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);

  ID3D11Buffer *stg[2] = {NULL, NULL};
  /* tern_*_ping index the buffers written by the last encode / decode. */
  ID3D11Buffer *src[2] = {p->tern_enc_yuvd_buf[p->tern_enc_ping & 1U],
                          p->tern_dec_yuvd_buf[p->tern_dec_ping & 1U]};
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
