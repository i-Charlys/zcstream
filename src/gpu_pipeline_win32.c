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
  bool tern_rc_2x;
  bool tern_dec_use_copy;
  ID3D11ComputeShader *tern_cs_tile_mv;
  ID3D11ComputeShader *tern_cs_encode;
  ID3D11ComputeShader *tern_cs_motion_decay;
  ID3D11ComputeShader *tern_cs_apply;
  ID3D11ComputeShader *tern_cs_ycbcr_to_rgba;
  ID3D11ShaderResourceView *cap_bgra_srv;
  ID3D11Buffer *tern_enc_yuvd_buf[2];
  ID3D11ShaderResourceView *tern_enc_yuvd_srv[2];
  ID3D11UnorderedAccessView *tern_enc_yuvd_uav[2];
  ID3D11Buffer *tern_enc_mvs_buf;
  ID3D11UnorderedAccessView *tern_enc_mvs_uav;
  ID3D11Buffer *tern_enc_mvs_stg;
  ID3D11Buffer *tern_out_records_buf;
  ID3D11UnorderedAccessView *tern_out_records_uav;
  ID3D11Buffer *tern_out_records_stg;
  ID3D11Buffer *tern_out_counter_buf;
  ID3D11UnorderedAccessView *tern_out_counter_uav;
  ID3D11Buffer *tern_out_counter_stg;
  ID3D11Buffer *tern_dec_yuvd_buf[2];
  ID3D11ShaderResourceView *tern_dec_yuvd_srv[2];
  ID3D11UnorderedAccessView *tern_dec_yuvd_uav[2];
  ID3D11Buffer *tern_dec_mvs_buf;
  ID3D11ShaderResourceView *tern_dec_mvs_srv;
  ID3D11Buffer *tern_in_records_buf;
  ID3D11ShaderResourceView *tern_in_records_srv;
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
      IDXGIOutput1 *out1 = NULL;
      if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&out1))) {
        if (SUCCEEDED(IDXGIOutput1_DuplicateOutput(out1, (IUnknown *)p->dev, &p->dupl))) {
          /* Frames are copied 1:1 into cap_bgra_tex: only usable when the
           * desktop has exactly the requested size and a BGRA8 format. */
          DXGI_OUTDUPL_DESC dd;
          memset(&dd, 0, sizeof(dd));
          IDXGIOutputDuplication_GetDesc(p->dupl, &dd);
          if (dd.ModeDesc.Width == p->width && dd.ModeDesc.Height == p->height &&
              dd.ModeDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            p->using_desktop_dup = true;
          } else {
            IDXGIOutputDuplication_Release(p->dupl);
            p->dupl = NULL;
          }
        }
        IDXGIOutput1_Release(out1);
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
} tern_dec_cb_t;

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
  ID3DBlob *code = NULL;
  ID3DBlob *err = NULL;
  HRESULT hr = E_FAIL;

  for (int i = 0; i < 4; ++i) {
    char path[MAX_PATH];
    wchar_t wpath[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", prefixes[i], filename);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    hr = D3DCompileFromFile(wpath, NULL, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                            entry, "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (SUCCEEDED(hr)) break;
    if (err) {
      fprintf(stderr, "HLSL compile error (%s:%s): %s\n",
              path, entry, (const char *)ID3D10Blob_GetBufferPointer(err));
      ID3D10Blob_Release(err);
      err = NULL;
    }
  }
  if (err) ID3D10Blob_Release(err);
  if (FAILED(hr) || !code) return -1;

  hr = ID3D11Device_CreateComputeShader(
      dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
      NULL, out_cs);
  ID3D10Blob_Release(code);
  return SUCCEEDED(hr) ? 0 : -2;
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

static int setup_ternary_hlsl(gpu_pipeline_t *p, gpu_pipeline_info_t *info) {
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

  HRESULT hr = ID3D11Device_CreateShaderResourceView(
      p->dev, (ID3D11Resource *)p->cap_bgra_tex, NULL, &p->cap_bgra_srv);
  if (FAILED(hr)) return -23;

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

  /* Tile MV buffers (enc UAV + staging, dec DYNAMIC SRV) */
  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = 8U;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_enc_mvs_buf);
  if (FAILED(hr)) return -24;

  D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {0};
  uavd.Format = DXGI_FORMAT_UNKNOWN;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = num_tiles;
  hr = ID3D11Device_CreateUnorderedAccessView(
      p->dev, (ID3D11Resource *)p->tern_enc_mvs_buf, &uavd, &p->tern_enc_mvs_uav);
  if (FAILED(hr)) return -24;

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_enc_mvs_stg);
  if (FAILED(hr)) return -24;

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = num_tiles * 8U;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = 8U;
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_dec_mvs_buf);
  if (FAILED(hr)) return -24;

  D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {0};
  srvd.Format = DXGI_FORMAT_UNKNOWN;
  srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srvd.Buffer.NumElements = num_tiles;
  hr = ID3D11Device_CreateShaderResourceView(
      p->dev, (ID3D11Resource *)p->tern_dec_mvs_buf, &srvd, &p->tern_dec_mvs_srv);
  if (FAILED(hr)) return -24;

  /* Output active records buffer + staging (worst-case Intra + Delta) */
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
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_out_records_stg);
  if (FAILED(hr)) return -25;

  /* Atomic counter buffer (4 bytes) + staging */
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
  hr = ID3D11Device_CreateBuffer(p->dev, &bd, NULL, &p->tern_out_counter_stg);
  if (FAILED(hr)) return -26;

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
  p->enc_out_cap = 8U + (size_t)num_tiles * 8U + (size_t)rec_bytes;
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

void gpu_pipeline_force_synth(gpu_pipeline_t *p, bool enable) {
  if (!p) return;
  if (enable && p->dupl) {
    IDXGIOutputDuplication_Release(p->dupl);
    p->dupl = NULL;
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
  if (p->tern_out_counter_stg) ID3D11Buffer_Release(p->tern_out_counter_stg);
  if (p->tern_out_counter_uav) ID3D11UnorderedAccessView_Release(p->tern_out_counter_uav);
  if (p->tern_out_counter_buf) ID3D11Buffer_Release(p->tern_out_counter_buf);
  if (p->tern_out_records_stg) ID3D11Buffer_Release(p->tern_out_records_stg);
  if (p->tern_out_records_uav) ID3D11UnorderedAccessView_Release(p->tern_out_records_uav);
  if (p->tern_out_records_buf) ID3D11Buffer_Release(p->tern_out_records_buf);
  if (p->tern_enc_mvs_stg) ID3D11Buffer_Release(p->tern_enc_mvs_stg);
  if (p->tern_enc_mvs_uav) ID3D11UnorderedAccessView_Release(p->tern_enc_mvs_uav);
  if (p->tern_enc_mvs_buf) ID3D11Buffer_Release(p->tern_enc_mvs_buf);
  if (p->cap_bgra_srv) ID3D11ShaderResourceView_Release(p->cap_bgra_srv);
  if (p->tern_cs_ycbcr_to_rgba) ID3D11ComputeShader_Release(p->tern_cs_ycbcr_to_rgba);
  if (p->tern_cs_apply) ID3D11ComputeShader_Release(p->tern_cs_apply);
  if (p->tern_cs_motion_decay) ID3D11ComputeShader_Release(p->tern_cs_motion_decay);
  if (p->tern_cs_encode) ID3D11ComputeShader_Release(p->tern_cs_encode);
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
  if (p->enc_nv12_tex) ID3D11Texture2D_Release(p->enc_nv12_tex);
  if (p->cap_rtv) ID3D11RenderTargetView_Release(p->cap_rtv);
  if (p->cap_bgra_tex) ID3D11Texture2D_Release(p->cap_bgra_tex);
  if (p->swapchain) IDXGISwapChain1_Release(p->swapchain);
  if (p->ctx1) ID3D11DeviceContext1_Release(p->ctx1);
  if (p->ctx) ID3D11DeviceContext_Release(p->ctx);
  if (p->dev) ID3D11Device_Release(p->dev);
  if (p->hwnd) DestroyWindow(p->hwnd);
  free(p->enc_out_buf);
  free(p);
  MFShutdown();
}

static int vp_blit_textures(gpu_pipeline_t *p, ID3D11Texture2D *src_tex,
                            UINT src_slice, ID3D11Texture2D *dst_tex) {
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

  RECT r = {0, 0, (LONG)p->width, (LONG)p->height};
  ID3D11VideoContext_VideoProcessorSetStreamSourceRect(p->vctx, p->vp, 0, TRUE, &r);
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

int gpu_pipeline_capture_and_encode(gpu_pipeline_t *p, uint32_t frame_idx,
                                    const uint8_t **out_bitstream,
                                    size_t *out_len, bool *out_keyframe,
                                    double *out_cap_us, double *out_enc_us) {
  LARGE_INTEGER t0, t1, t2;
  QueryPerformanceCounter(&t0);

  ID3D11Texture2D *src_bgra = p->cap_bgra_tex;
  IDXGIResource *dupl_res = NULL;
  bool acquired_dupl = false;

  if (p->dupl) {
    DXGI_OUTDUPL_FRAME_INFO fi;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(p->dupl, 0, &fi, &dupl_res);
    if (SUCCEEDED(hr) && dupl_res) {
      ID3D11Texture2D *dupl_tex = NULL;
      if (SUCCEEDED(IDXGIResource_QueryInterface(dupl_res, &IID_ID3D11Texture2D,
                                                 (void **)&dupl_tex))) {
        ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->cap_bgra_tex,
                                         (ID3D11Resource *)dupl_tex);
        ID3D11Texture2D_Release(dupl_tex);
        p->dupl_has_frame = true;
      }
      acquired_dupl = true;
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
      /* Access lost (mode switch, secure desktop...): stop duplicating and
       * fall back to the GPU-rendered pattern from now on. */
      IDXGIOutputDuplication_Release(p->dupl);
      p->dupl = NULL;
      p->dupl_has_frame = false;
      p->fallback_bg_init = false;
    }
  }

  /* A timeout only means the desktop did not change: the previous capture is
   * still the current picture, so do not paint the test pattern over it. */
  if (!acquired_dupl && !p->dupl_has_frame) {
    if (p->ctx1) {
      float bg[4] = {0.10f, 0.12f, 0.16f, 1.0f};
      float hud[4] = {0.88f, 0.90f, 0.94f, 1.0f};
      float box_col[4] = {0.25f, 0.65f, 0.92f, 1.0f};
      if (!p->fallback_bg_init) {
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
    uint32_t mv_reserve = 8U + num_tiles * 2U;
    uint32_t target_words = (target_bytes > mv_reserve + 4096U)
                                ? ((target_bytes - mv_reserve) / 4U)
                                : 1024U;
    uint32_t hard_cap_words = (target_words + (target_words >> 2)) & ~1U;
    if (hard_cap_words < 1024U) hard_cap_words = 1024U;
    if (hard_cap_words > p->tern_max_record_words) hard_cap_words = p->tern_max_record_words & ~1U;

    /* Scale rolling intra-refresh cycles so it uses ~25% of target_words */
    uint32_t intra_budget = target_words / 4U;
    uint32_t words_per_row = (p->width * 3U) / 5U;
    uint32_t rows_per_frame = (words_per_row > 0U && intra_budget >= words_per_row)
                                  ? (intra_budget / words_per_row)
                                  : 1U;
    uint32_t intra_cycles = (p->height + rows_per_frame - 1U) / rows_per_frame;
    if (intra_cycles < 30U) intra_cycles = 30U;
    if (intra_cycles > 480U) intra_cycles = 480U;

    uint32_t mode_flags = (frame_idx < 16U) ? 1U : (p->tern_rc_2x ? 1U : 0U);
    uint32_t eff_blocks = (mode_flags != 0U) ? ((p->tern_total_blocks + 1U) >> 1)
                                             : p->tern_total_blocks;
    uint32_t enc_cap_words = (frame_idx < 16U) ? 140000U : hard_cap_words;
    if (enc_cap_words > (p->tern_max_record_words & ~1U)) {
      enc_cap_words = p->tern_max_record_words & ~1U;
    }
    uint32_t enc_thr = (frame_idx < 16U) ? 8U : p->tern_rc_threshold;

    tern_enc_cb_t ecb = {
        p->width, p->height, enc_thr, p->tern_delta_min,
        p->tern_delta_max, p->tern_gamma_q8, p->tern_neutral_y, p->tern_total_blocks,
        frame_idx, intra_cycles, (frame_idx < 16U) ? 255U : p->tern_rc_intra_err_thr,
        p->tern_tiles_x, p->tern_tiles_y, 4U, enc_cap_words, mode_flags
    };
    ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->tern_enc_cb,
                                          0, NULL, &ecb, 0, 0);

    ID3D11ShaderResourceView *srvs[2] = {p->cap_bgra_srv, p->tern_enc_yuvd_srv[ping]};
    ID3D11UnorderedAccessView *uavs[4] = {
        p->tern_enc_yuvd_uav[pong], p->tern_enc_mvs_uav,
        p->tern_out_records_uav, p->tern_out_counter_uav
    };
    ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->tern_enc_cb);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 2, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, uavs, NULL);

    /* Pass 1: CSTileMV (32x32 tiles) */
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_tile_mv, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, p->tern_tiles_x, p->tern_tiles_y, 1);

    /* Pass 2: CSEncode (YCbCr + ADM + Rolling Intra-Refresh) */
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_encode, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (eff_blocks + 63U) / 64U, 1, 1);

    ID3D11ShaderResourceView *null_srv[2] = {NULL, NULL};
    ID3D11UnorderedAccessView *null_uav[4] = {NULL, NULL, NULL, NULL};
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 2, null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 4, null_uav, NULL);

    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->tern_out_counter_stg,
                                     (ID3D11Resource *)p->tern_out_counter_buf);
    ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)p->tern_enc_mvs_stg,
                                     (ID3D11Resource *)p->tern_enc_mvs_buf);

    D3D11_MAPPED_SUBRESOURCE mr = {0};
    uint32_t raw_words = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->tern_out_counter_stg,
                                          0, D3D11_MAP_READ, 0, &mr))) {
      raw_words = *(const uint32_t *)mr.pData;
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_out_counter_stg, 0);
    }
    uint32_t record_words = (raw_words > enc_cap_words) ? enc_cap_words : raw_words;
    record_words &= ~1U;

    /* Compact MVs: 0 bytes if all (0,0), else 2 bytes/tile (int8 dx, int8 dy) */
    uint32_t mv_bytes = 0U;
    if (SUCCEEDED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->tern_enc_mvs_stg,
                                          0, D3D11_MAP_READ, 0, &mr))) {
      const int32_t *mvs = (const int32_t *)mr.pData;
      bool any_nz = false;
      for (uint32_t i = 0; i < num_tiles; ++i) {
        if ((mvs[i * 2U] | mvs[i * 2U + 1U]) != 0) {
          any_nz = true;
          break;
        }
      }
      if (any_nz) {
        mv_bytes = num_tiles * 2U;
        int8_t *dst_mv = (int8_t *)(p->enc_out_buf + 8U);
        for (uint32_t i = 0; i < num_tiles; ++i) {
          dst_mv[i * 2U + 0U] = (int8_t)mvs[i * 2U + 0U];
          dst_mv[i * 2U + 1U] = (int8_t)mvs[i * 2U + 1U];
        }
      }
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_enc_mvs_stg, 0);
    }
    memcpy(p->enc_out_buf, &record_words, sizeof(uint32_t));
    memcpy(p->enc_out_buf + sizeof(uint32_t), &mv_bytes, sizeof(uint32_t));

    /* Closed-loop frame rate controller (skip initial 16-frame convergence burst) */
    if (frame_idx >= 16U) {
      if (raw_words > target_words + (target_words >> 3)) {
        if (!p->tern_rc_2x && raw_words > target_words + (target_words >> 1)) {
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
      if (p->tern_rc_threshold > p->tern_threshold) {
        p->tern_rc_threshold--;
      }
      if (p->tern_rc_intra_err_thr > 32U) {
        p->tern_rc_intra_err_thr -= 4U;
      }
      if (p->tern_rc_2x && raw_words < (target_words >> 1)) {
        p->tern_rc_2x = false;
      }
    }
    }

    size_t out_bytes = 8U + mv_bytes;
    if (record_words > 0) {
      D3D11_BOX box = {0, 0, 0, record_words * (UINT)sizeof(uint32_t), 1, 1};
      ID3D11DeviceContext_CopySubresourceRegion(
          p->ctx, (ID3D11Resource *)p->tern_out_records_stg, 0, 0, 0, 0,
          (ID3D11Resource *)p->tern_out_records_buf, 0, &box);
      if (SUCCEEDED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->tern_out_records_stg,
                                            0, D3D11_MAP_READ, 0, &mr))) {
        size_t rec_bytes = (size_t)record_words * sizeof(uint32_t);
        memcpy(p->enc_out_buf + out_bytes, mr.pData, rec_bytes);
        out_bytes += rec_bytes;
        ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_out_records_stg, 0);
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
    uint32_t record_words = 0, mv_bytes = 0;
    memcpy(&record_words, bitstream, sizeof(uint32_t));
    memcpy(&mv_bytes, bitstream + sizeof(uint32_t), sizeof(uint32_t));
    if (record_words > p->tern_max_record_words) record_words = p->tern_max_record_words;
    if (len < 8U + (size_t)mv_bytes + (size_t)record_words * sizeof(uint32_t)) return -1;

    uint32_t num_tiles = p->tern_tiles_x * p->tern_tiles_y;
    D3D11_MAPPED_SUBRESOURCE mw = {0};
    if (SUCCEEDED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->tern_dec_mvs_buf,
                                          0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
      if (mv_bytes == 0U) {
        memset(mw.pData, 0, (size_t)num_tiles * 8U);
      } else if (mv_bytes == num_tiles * 2U) {
        const int8_t *src_mv = (const int8_t *)(bitstream + 8U);
        int32_t *dst_mv = (int32_t *)mw.pData;
        for (uint32_t i = 0; i < num_tiles; ++i) {
          dst_mv[i * 2U + 0U] = (int32_t)src_mv[i * 2U + 0U];
          dst_mv[i * 2U + 1U] = (int32_t)src_mv[i * 2U + 1U];
        }
      } else {
        uint32_t copy_mv = (mv_bytes <= num_tiles * 8U) ? mv_bytes : (num_tiles * 8U);
        memcpy(mw.pData, bitstream + 8U, copy_mv);
      }
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_dec_mvs_buf, 0);
    }
    if (record_words > 0 &&
        SUCCEEDED(ID3D11DeviceContext_Map(p->ctx, (ID3D11Resource *)p->tern_in_records_buf,
                                          0, D3D11_MAP_WRITE_DISCARD, 0, &mw))) {
      memcpy(mw.pData, bitstream + 8U + mv_bytes,
             (size_t)record_words * sizeof(uint32_t));
      ID3D11DeviceContext_Unmap(p->ctx, (ID3D11Resource *)p->tern_in_records_buf, 0);
    }

    uint32_t ping = p->tern_dec_ping & 1U;
    uint32_t pong = ping ^ 1U;
    p->tern_dec_ping = pong;

    tern_dec_cb_t dcb = {
        p->width, p->height, p->tern_delta_min, p->tern_delta_max,
        p->tern_gamma_q8, p->tern_neutral_y, record_words, p->tern_tiles_x
    };
    ID3D11DeviceContext_UpdateSubresource(p->ctx, (ID3D11Resource *)p->tern_dec_cb,
                                          0, NULL, &dcb, 0, 0);

    ID3D11ShaderResourceView *srvs[3] = {
        p->tern_dec_mvs_srv, p->tern_in_records_srv, p->tern_dec_yuvd_srv[ping]
    };
    ID3D11UnorderedAccessView *uavs[2] = {
        p->tern_dec_yuvd_uav[pong], p->tern_dec_uav
    };
    ID3D11DeviceContext_CSSetConstantBuffers(p->ctx, 0, 1, &p->tern_dec_cb);
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 2, uavs, NULL);

    /* Pass 1: CSMotionDecay */
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_motion_decay, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U,
                                 (p->height + 15U) / 16U, 1);

    /* Pass 2: CSApply */
    if (record_words >= 2U) {
      ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_apply, NULL, 0);
      ID3D11DeviceContext_Dispatch(p->ctx, ((record_words / 2U) + 63U) / 64U, 1, 1);
    }

    /* Pass 3: CSYCbCrToRGBA */
    ID3D11DeviceContext_CSSetShader(p->ctx, p->tern_cs_ycbcr_to_rgba, NULL, 0);
    ID3D11DeviceContext_Dispatch(p->ctx, (p->width + 15U) / 16U,
                                 (p->height + 15U) / 16U, 1);

    ID3D11ShaderResourceView *null_srv[3] = {NULL, NULL, NULL};
    ID3D11UnorderedAccessView *null_uav[2] = {NULL, NULL};
    ID3D11DeviceContext_CSSetShaderResources(p->ctx, 0, 3, null_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(p->ctx, 0, 2, null_uav, NULL);

    QueryPerformanceCounter(&t1);

    ID3D11Texture2D *backbuf = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                            (void **)&backbuf))) {
      if (p->tern_dec_use_copy) {
        ID3D11DeviceContext_CopyResource(p->ctx, (ID3D11Resource *)backbuf,
                                         (ID3D11Resource *)p->tern_dec_tex);
      } else {
        vp_blit_textures(p, p->tern_dec_tex, 0U, backbuf);
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
      vp_blit_textures(p, dec_tex, dec_sub, backbuf);
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
