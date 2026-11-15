#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_pipeline_win32.h"
#include "nvEncodeAPI.h"

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

  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
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
          p->using_desktop_dup = true;
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

void gpu_pipeline_destroy(gpu_pipeline_t *p) {
  if (!p) return;
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
  if (p->ctx) ID3D11DeviceContext_Release(p->ctx);
  if (p->dev) ID3D11Device_Release(p->dev);
  if (p->hwnd) DestroyWindow(p->hwnd);
  free(p->enc_out_buf);
  free(p);
  MFShutdown();
}

static int vp_blit_textures(gpu_pipeline_t *p, ID3D11Texture2D *src_tex,
                            ID3D11Texture2D *dst_tex) {
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {0};
  ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  ID3D11VideoProcessorInputView *iv = NULL;
  HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorInputView(
      p->vdev, (ID3D11Resource *)src_tex, p->vp_enum, &ivd, &iv);
  if (FAILED(hr)) return -1;

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {0};
  ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  ID3D11VideoProcessorOutputView *ov = NULL;
  hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(
      p->vdev, (ID3D11Resource *)dst_tex, p->vp_enum, &ovd, &ov);
  if (FAILED(hr)) {
    ID3D11VideoProcessorInputView_Release(iv);
    return -1;
  }

  D3D11_VIDEO_PROCESSOR_STREAM stream = {0};
  stream.Enable = TRUE;
  stream.pInputSurface = iv;
  hr = ID3D11VideoContext_VideoProcessorBlt(p->vctx, p->vp, ov, 0, 1, &stream);

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
      }
      acquired_dupl = true;
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
      /* Fallback to GPU-rendered pattern */
    }
  }

  if (!acquired_dupl) {
    float c = (float)(frame_idx % 120U) / 120.0f;
    float rgba[4] = {0.1f + 0.7f * c, 0.25f, 0.85f - 0.6f * c, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(p->ctx, p->cap_rtv, rgba);
  }

  if (!p->use_direct_nvenc) {
    /* Zero-copy GPU BGRA -> NV12 conversion in VRAM (MFT fallback only) */
    vp_blit_textures(p, src_bgra, p->enc_nv12_tex);
  }

  if (acquired_dupl) {
    IDXGIResource_Release(dupl_res);
    IDXGIOutputDuplication_ReleaseFrame(p->dupl);
  }

  QueryPerformanceCounter(&t1);

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

  HRESULT hr = IMFTransform_ProcessInput(p->dec_mft, 0, in_sample, 0);
  IMFSample_Release(in_sample);
  if (FAILED(hr)) return -40;

  MFT_OUTPUT_DATA_BUFFER out_data = {0};
  DWORD status = 0;
  bool decoded = false;
  ID3D11Texture2D *dec_tex = NULL;

  for (int tries = 0; tries < 8; ++tries) {
    hr = IMFTransform_ProcessOutput(p->dec_mft, 0, 1, &out_data, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      for (DWORD idx = 0; ; ++idx) {
        IMFMediaType *mt_avail = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(p->dec_mft, 0, idx, &mt_avail))) break;
        GUID sub = {0};
        IMFMediaType_GetGUID(mt_avail, &ZC_MF_MT_SUBTYPE, &sub);
        if (IsEqualGUID(&sub, &ZC_MFVideoFormat_NV12)) {
          IMFTransform_SetOutputType(p->dec_mft, 0, mt_avail, 0);
          IMFMediaType_Release(mt_avail);
          break;
        }
        IMFMediaType_Release(mt_avail);
      }
      continue;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
    if (SUCCEEDED(hr) && out_data.pSample) {
      IMFMediaBuffer *ob = NULL;
      if (SUCCEEDED(IMFSample_GetBufferByIndex(out_data.pSample, 0, &ob))) {
        IMFDXGIBuffer *dxgi_buf = NULL;
        if (SUCCEEDED(IMFMediaBuffer_QueryInterface(ob, &IID_IMFDXGIBuffer, (void **)&dxgi_buf))) {
          IMFDXGIBuffer_GetResource(dxgi_buf, &IID_ID3D11Texture2D, (void **)&dec_tex);
          IMFDXGIBuffer_Release(dxgi_buf);
        }
        IMFMediaBuffer_Release(ob);
      }
      IMFSample_Release(out_data.pSample);
      decoded = (dec_tex != NULL);
      break;
    }
  }

  QueryPerformanceCounter(&t1);

  if (decoded && dec_tex) {
    ID3D11Texture2D *backbuf = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(p->swapchain, 0, &IID_ID3D11Texture2D,
                                            (void **)&backbuf))) {
      vp_blit_textures(p, dec_tex, backbuf);
      ID3D11Texture2D_Release(backbuf);
    }
    ID3D11Texture2D_Release(dec_tex);

    UINT present_flags = p->tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    IDXGISwapChain1_Present(p->swapchain, 0, present_flags);
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
