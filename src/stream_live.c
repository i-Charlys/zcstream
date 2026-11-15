#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "gpu_pipeline_win32.h"
#include "zcrudp_video_bridge.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WSA_FLAG_REGISTERED_IO
#define WSA_FLAG_REGISTERED_IO 0x100
#endif

static volatile LONG g_running = 1;
static FILE *g_log_file = NULL;

static void log_init(const char *role_name) {
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    char *slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (slash) {
      *(slash + 1) = '\0';
    } else {
      path[0] = '\0';
    }
  } else {
    path[0] = '\0';
  }
  char log_path[MAX_PATH];
  snprintf(log_path, sizeof(log_path), "%szcrudp_stream_%s.log", path, role_name);
  g_log_file = fopen(log_path, "w");
  if (g_log_file) {
    gpu_pipeline_set_log_file(g_log_file);
    fprintf(g_log_file, "=== log started (%s) ===\n", role_name);
    fflush(g_log_file);
  }
}

static void log_out(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
  if (g_log_file) {
    va_list ap2;
    va_start(ap2, fmt);
    vfprintf(g_log_file, fmt, ap2);
    va_end(ap2);
    fflush(g_log_file);
  }
}

static void log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
  if (g_log_file) {
    va_list ap2;
    va_start(ap2, fmt);
    vfprintf(g_log_file, fmt, ap2);
    va_end(ap2);
    fflush(g_log_file);
  }
}

static BOOL WINAPI on_console_ctrl(DWORD ctrl_type) {
  (void)ctrl_type;
  InterlockedExchange(&g_running, 0);
  return TRUE;
}

typedef enum {
  ROLE_NONE = 0,
  ROLE_SENDER = 1,
  ROLE_RECEIVER = 2
} stream_role_t;

typedef enum {
  CODEC_NVENC = 0,
  CODEC_TERNARY = 1
} stream_codec_t;

static void print_usage(const char *exe) {
  printf("Usage: %s --role <sender|receiver> [options]\n"
         "  --role <sender|receiver>  Required role\n"
         "  --codec <nvenc|ternary>   Video codec (default: nvenc)\n"
         "  --host <IPv4>             Receiver IP for sender (default: 127.0.0.1)\n"
         "  --port <port>             UDP port (default: 9900)\n"
         "  --width <px>              Frame width (default: 1920)\n"
         "  --height <px>             Frame height (default: 1080)\n"
         "  --fps <hz>                Target frame rate (default: 120)\n"
         "  --bitrate <Mbps>          Target bitrate / pacing ceiling (default: 25 nvenc, 400 ternary)\n"
         "  --fec <0..4>              Cauchy GF(2^8) parity shards (default: 1)\n"
         "  --frames <N>              Stop after N frames (default: 0 = continuous)\n"
         "  --no-rio                  Disable Winsock RIO on sender\n"
         "  --help                    Show this help\n",
         exe);
}

static gpu_pipeline_t *create_pipeline(stream_codec_t codec, uint32_t width,
                                       uint32_t height, uint32_t fps,
                                       uint32_t bitrate_bps, bool show_window,
                                       gpu_pipeline_info_t *info) {
  gpu_pipeline_t *gpu = NULL;
  int rc = 0;
  if (codec == CODEC_TERNARY) {
    rc = gpu_pipeline_create_ternary(&gpu, width, height, fps, 6U, 2U, 256U,
                                     128U, show_window, info);
    if (rc == 0 && gpu) gpu_pipeline_set_bitrate(gpu, bitrate_bps);
  } else {
    rc = gpu_pipeline_create(&gpu, width, height, fps, bitrate_bps, show_window,
                             info);
  }
  if (rc != 0) {
    fprintf(stderr, "gpu_pipeline_create failed (%d)\n", rc);
    return NULL;
  }
  return gpu;
}

static int run_sender(stream_codec_t codec, const char *host, uint16_t port,
                      uint32_t width, uint32_t height, uint32_t fps,
                      uint32_t bitrate_bps, uint8_t fec_shards,
                      uint64_t max_frames, bool use_rio) {
  gpu_pipeline_info_t info;
  gpu_pipeline_t *gpu =
      create_pipeline(codec, width, height, fps, bitrate_bps, false, &info);
  if (!gpu) return 1;

  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &dst_addr.sin_addr) != 1) {
    fprintf(stderr, "Invalid --host IPv4 address: %s\n", host);
    gpu_pipeline_destroy(gpu);
    return 1;
  }

  SOCKET tx_sock = INVALID_SOCKET;
  bool rio_enabled = false;
  if (use_rio) {
    tx_sock = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0,
                         WSA_FLAG_REGISTERED_IO);
    if (tx_sock != INVALID_SOCKET) {
      int sndbuf = 8 * 1024 * 1024;
      setsockopt(tx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf,
                 sizeof(sndbuf));
      if (connect(tx_sock, (const struct sockaddr *)&dst_addr,
                  sizeof(dst_addr)) == 0) {
        rio_enabled = true;
      } else {
        closesocket(tx_sock);
        tx_sock = INVALID_SOCKET;
      }
    }
  }
  if (tx_sock == INVALID_SOCKET) {
    tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx_sock == INVALID_SOCKET) {
      fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
      gpu_pipeline_destroy(gpu);
      return 1;
    }
    int sndbuf = 8 * 1024 * 1024;
    setsockopt(tx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf,
               sizeof(sndbuf));
  }

  zc_video_tx_t *tx = (zc_video_tx_t *)calloc(1, sizeof(*tx));
  zc_net_impair_t impair = {0};
  if (!tx ||
      zc_video_tx_init(tx, RUDP_PAGE_POOL_PAGES, bitrate_bps, bitrate_bps / 4U,
                       bitrate_bps * 2U, fec_shards, &impair,
                       qpc_now_us()) != 0) {
    fprintf(stderr, "zc_video_tx_init failed\n");
    free(tx);
    closesocket(tx_sock);
    gpu_pipeline_destroy(gpu);
    return 1;
  }

  if (rio_enabled) {
    if (zc_video_tx_enable_rio(tx, (uintptr_t)tx_sock) != 0) {
      rio_enabled = false;
      closesocket(tx_sock);
      tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (tx_sock == INVALID_SOCKET) {
        fprintf(stderr, "fallback socket() failed\n");
        zc_video_tx_destroy(tx);
        free(tx);
        gpu_pipeline_destroy(gpu);
        return 1;
      }
    }
  }

  printf("=== zcrudp_stream SENDER ===\n");
  printf("Target   : %s:%u | RIO=%s | FEC=%u shards\n", host, (unsigned)port,
         rio_enabled ? "ON" : "OFF", (unsigned)fec_shards);
  printf("Adapter  : %s\n", info.adapter_name);
  printf("Encoder  : %s (%ux%u @ %u fps, %.1f Mbps)\n", info.encoder_name,
         width, height, fps, (double)bitrate_bps / 1e6);
  printf("Capture  : %s\n",
         info.using_desktop_dup ? "DXGI Desktop Duplication" : "Synthetic fallback");
  fflush(stdout);

  const uint64_t frame_period_us = 1000000ULL / (fps ? fps : 60U);
  uint64_t next_tick_us = qpc_now_us();
  uint64_t last_report_us = next_tick_us;
  uint64_t frames_sent = 0;
  uint64_t bytes_period = 0;
  uint64_t frames_period = 0;

  for (uint32_t f = 0; InterlockedCompareExchange(&g_running, 1, 1) == 1; ++f) {
    if (max_frames > 0 && frames_sent >= max_frames) break;

    const uint8_t *bs = NULL;
    size_t bs_len = 0;
    bool keyframe = false;
    double cap_us = 0.0, enc_us = 0.0;
    int erc = gpu_pipeline_capture_and_encode(gpu, f, &bs, &bs_len, &keyframe,
                                              &cap_us, &enc_us);
    if (erc == 0 && bs && bs_len > 0) {
      uint64_t now_us = qpc_now_us();
      zc_video_tx_send_frame(tx, (uintptr_t)tx_sock,
                             (const struct sockaddr *)&dst_addr,
                             (int)sizeof(dst_addr), f, bs, bs_len, keyframe,
                             now_us, false);
      frames_sent++;
      frames_period++;
      bytes_period += bs_len;
    }

    uint64_t now_us = qpc_now_us();
    if (now_us - last_report_us >= 1000000ULL) {
      double dt_s = (double)(now_us - last_report_us) / 1e6;
      double cur_fps = (double)frames_period / dt_s;
      double cur_mbps = ((double)bytes_period * 8.0) / (dt_s * 1e6);
      printf("[SENDER] %.1f fps | %.2f Mbps | total=%llu frames | pkts=%llu | errs=%llu\n",
             cur_fps, cur_mbps, (unsigned long long)frames_sent,
             (unsigned long long)tx->packets_sent,
             (unsigned long long)tx->send_errors);
      fflush(stdout);
      last_report_us = now_us;
      frames_period = 0;
      bytes_period = 0;
    }

    next_tick_us += frame_period_us;
    now_us = qpc_now_us();
    if (next_tick_us > now_us) {
      uint64_t wait_us = next_tick_us - now_us;
      if (wait_us > 2000ULL) {
        Sleep((DWORD)((wait_us - 1000ULL) / 1000ULL));
      }
      while (qpc_now_us() < next_tick_us) {
        YieldProcessor();
      }
    } else if (now_us - next_tick_us > frame_period_us * 4ULL) {
      next_tick_us = now_us;
    }
  }

  printf("Sender stopped: %llu frames sent, %llu packets, %llu send errors.\n",
         (unsigned long long)frames_sent, (unsigned long long)tx->packets_sent,
         (unsigned long long)tx->send_errors);

  zc_video_tx_destroy(tx);
  free(tx);
  closesocket(tx_sock);
  gpu_pipeline_destroy(gpu);
  return 0;
}

static int run_receiver(stream_codec_t codec, uint16_t port, uint32_t width,
                        uint32_t height, uint32_t fps, uint32_t bitrate_bps,
                        uint64_t max_frames) {
  gpu_pipeline_info_t info;
  gpu_pipeline_t *gpu =
      create_pipeline(codec, width, height, fps, bitrate_bps, true, &info);
  if (!gpu) return 1;

  SOCKET rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (rx_sock == INVALID_SOCKET) {
    fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
    gpu_pipeline_destroy(gpu);
    return 1;
  }

  int rcvbuf = 8 * 1024 * 1024;
  setsockopt(rx_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf,
             sizeof(rcvbuf));

  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(port);
  if (bind(rx_sock, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) !=
      0) {
    fprintf(stderr, "bind(0.0.0.0:%u) failed: %d\n", (unsigned)port,
            WSAGetLastError());
    closesocket(rx_sock);
    gpu_pipeline_destroy(gpu);
    return 1;
  }

  u_long nonblock = 1;
  ioctlsocket(rx_sock, FIONBIO, &nonblock);

  zc_video_rx_t *rx = (zc_video_rx_t *)calloc(1, sizeof(*rx));
  if (!rx || zc_video_rx_init(rx, 8 * 1024 * 1024) != 0) {
    fprintf(stderr, "zc_video_rx_init failed\n");
    free(rx);
    closesocket(rx_sock);
    gpu_pipeline_destroy(gpu);
    return 1;
  }

  log_out("=== zcrudp_stream RECEIVER ===\n");
  log_out("Listening: 0.0.0.0:%u\n", (unsigned)port);
  log_out("Adapter  : %s\n", info.adapter_name);
  log_out("Decoder  : %s (%ux%u)\n", info.decoder_name, width, height);

  uint8_t pkt[2048];
  uint64_t frames_decoded = 0;
  uint64_t frames_period = 0;
  uint64_t bytes_period = 0;
  uint64_t pkts_period = 0;
  uint64_t rx_err_period = 0;
  uint64_t dec_fail_period = 0;
  bool seen_first_pkt = false;
  bool seen_first_frame = false;
  bool seen_first_dec = false;
  unsigned dec_fail_logs = 0;
  uint64_t last_report_us = qpc_now_us();

  while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
    if (!gpu_pipeline_pump_events(gpu)) break;
    if (max_frames > 0 && frames_decoded >= max_frames) break;

    WSAPOLLFD pfd = {0};
    pfd.fd = rx_sock;
    pfd.events = POLLRDNORM;
    int prc = WSAPoll(&pfd, 1, 5);
    if (prc > 0 && (pfd.revents & POLLRDNORM)) {
      for (;;) {
        int rlen = recvfrom(rx_sock, (char *)pkt, sizeof(pkt), 0, NULL, NULL);
        if (rlen <= 0) break;
        pkts_period++;
        if (!seen_first_pkt) {
          seen_first_pkt = true;
          log_out("[RECEIVER] first UDP packet received (%d bytes)\n", rlen);
        }

        size_t rx_len = 0;
        uint32_t rx_fid = 0;
        bool rx_kf = false, rx_fec = false;
        int feed_rc = zc_video_rx_feed(rx, pkt, (size_t)rlen, &rx_len, &rx_fid,
                                       &rx_kf, &rx_fec);
        if (feed_rc < 0) {
          rx_err_period++;
        } else if (feed_rc == 1) {
          if (!seen_first_frame) {
            seen_first_frame = true;
            log_out("[RECEIVER] first frame assembled: id=%u len=%zu kf=%d fec=%d\n",
                    rx_fid, rx_len, rx_kf ? 1 : 0, rx_fec ? 1 : 0);
          }
          double dec_us = 0.0, prs_us = 0.0;
          int dec_rc = gpu_pipeline_decode_and_present(gpu, rx->frame_buf,
                                                       rx_len, &dec_us, &prs_us);
          if (dec_rc == 0) {
            if (!seen_first_dec) {
              seen_first_dec = true;
              log_out("[RECEIVER] first frame decoded & presented (dec=%.0fus present=%.0fus)\n",
                      dec_us, prs_us);
            }
            frames_decoded++;
            frames_period++;
            bytes_period += rx_len;
            if (max_frames > 0 && frames_decoded >= max_frames) break;
          } else {
            dec_fail_period++;
            if (dec_fail_logs++ < 10) {
              log_err("[RECEIVER] decode_and_present rc=%d (id=%u len=%zu kf=%d)\n",
                      dec_rc, rx_fid, rx_len, rx_kf ? 1 : 0);
            }
          }
        }
      }
    }

    uint64_t now_us = qpc_now_us();
    if (now_us - last_report_us >= 1000000ULL) {
      double dt_s = (double)(now_us - last_report_us) / 1e6;
      double cur_fps = (double)frames_period / dt_s;
      double cur_mbps = ((double)bytes_period * 8.0) / (dt_s * 1e6);
      log_out("[RECEIVER] %.1f fps | %.2f Mbps | decoded=%llu | fec_rec=%llu | dropped=%llu | pkts=%llu | rx_err=%llu | dec_fail=%llu\n",
              cur_fps, cur_mbps, (unsigned long long)frames_decoded,
              (unsigned long long)rx->frames_fec_recovered,
              (unsigned long long)rx->frames_dropped,
              (unsigned long long)pkts_period,
              (unsigned long long)rx_err_period,
              (unsigned long long)dec_fail_period);
      last_report_us = now_us;
      frames_period = 0;
      bytes_period = 0;
      pkts_period = 0;
      rx_err_period = 0;
      dec_fail_period = 0;
    }
  }

  log_out("Receiver stopped: %llu frames decoded, %llu completed, %llu FEC recovered, %llu dropped.\n",
          (unsigned long long)frames_decoded,
          (unsigned long long)rx->frames_completed,
          (unsigned long long)rx->frames_fec_recovered,
          (unsigned long long)rx->frames_dropped);

  zc_video_rx_destroy(rx);
  free(rx);
  closesocket(rx_sock);
  gpu_pipeline_destroy(gpu);
  return 0;
}

int main(int argc, char **argv) {
  stream_role_t role = ROLE_NONE;
  stream_codec_t codec = CODEC_NVENC;
  const char *host = "127.0.0.1";
  uint16_t port = 9900;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t fps = 120;
  uint32_t bitrate_mbps = 0;
  uint8_t fec_shards = 1;
  uint64_t max_frames = 0;
  bool use_rio = true;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      if (strcmp(v, "sender") == 0) role = ROLE_SENDER;
      else if (strcmp(v, "receiver") == 0) role = ROLE_RECEIVER;
      else {
        fprintf(stderr, "Unknown --role '%s'\n", v);
        return 1;
      }
    } else if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
      const char *v = argv[++i];
      if (strcmp(v, "nvenc") == 0) codec = CODEC_NVENC;
      else if (strcmp(v, "ternary") == 0) codec = CODEC_TERNARY;
      else {
        fprintf(stderr, "Unknown --codec '%s'\n", v);
        return 1;
      }
    } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
      width = (uint32_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
      height = (uint32_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
      fps = (uint32_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
      bitrate_mbps = (uint32_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--fec") == 0 && i + 1 < argc) {
      int f = atoi(argv[++i]);
      if (f < 0) f = 0;
      if (f > (int)ZC_MAX_PARITY_SHARDS) f = (int)ZC_MAX_PARITY_SHARDS;
      fec_shards = (uint8_t)f;
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      max_frames = (uint64_t)strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--no-rio") == 0) {
      use_rio = false;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  if (role == ROLE_NONE) {
    print_usage(argv[0]);
    return 1;
  }

  if (bitrate_mbps == 0) {
    bitrate_mbps = (codec == CODEC_TERNARY) ? 400U : 25U;
  }

  SetConsoleCtrlHandler(on_console_ctrl, TRUE);
  timeBeginPeriod(1);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    timeEndPeriod(1);
    return 1;
  }

  log_init(role == ROLE_SENDER ? "sender" : "receiver");

  int rc = 0;
  if (role == ROLE_SENDER) {
    rc = run_sender(codec, host, port, width, height, fps,
                    bitrate_mbps * 1000000U, fec_shards, max_frames, use_rio);
  } else {
    rc = run_receiver(codec, port, width, height, fps, bitrate_mbps * 1000000U,
                      max_frames);
  }

  gpu_pipeline_set_log_file(NULL);
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }

  WSACleanup();
  timeEndPeriod(1);
  return rc;
}
