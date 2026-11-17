#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "gpu_pipeline_win32.h"
#include "zcrudp_video_bridge.h"
#include "zpsync.h"

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
         "  --codec <nvenc|ternary>   Video codec (default: ternary)\n"
         "  --host <IPv4>             Receiver IP for sender (default: 127.0.0.1)\n"
         "  --port <port>             UDP port (default: 9900)\n"
         "  --width <px>              Frame width (default: 1920)\n"
         "  --height <px>             Frame height (default: 1080)\n"
         "  --fps <hz>                Target frame rate (default: 120)\n"
         "  --bitrate <Mbps>          Target bitrate / pacing ceiling (default: 25 nvenc, 400 ternary)\n"
         "  --fec <0..4>              Cauchy GF(2^8) parity shards (default: 1)\n"
         "  --frames <N>              Stop after N frames (default: 0 = continuous)\n"
         "  --no-rio                  Disable Winsock RIO on sender\n"
         "  --zpsync <secret>         Enable ZPSync P2P NAT traversal with shared secret\n"
         "  --zp-peer <ip>            Optional manual peer IPv4/IPv6 (skips DHT)\n"
         "  --zp-port <port>          Optional manual peer UDP port (0 = deterministic)\n"
         "  --zp-local-port <port>    Optional local UDP port override (0 = deterministic)\n"
         "  --zp-birthday             Force Birthday Paradox CGNAT spray\n"
         "  --zp-no-sentinel          Disable Layer-3 ICMP Sentinel\n"
         "  --sim-loss <pct>          Sender drops this share of its datagrams (test)\n"
         "  --sim-burst <pct> <len>   ... and starts bursts of <len> drops (test)\n"
         "  --help                    Show this help\n",
         exe);
}

typedef struct {
  const char *secret;
  const char *peer_ip;
  uint16_t peer_port;
  uint16_t local_port;
  bool force_birthday;
  bool disable_sentinel;
} zpsync_stream_opts_t;

static const char *zp_nat_name(zpsync_nat_type_e nat) {
  switch (nat) {
    case ZPSYNC_NAT_OPEN_INTERNET: return "Open Internet";
    case ZPSYNC_NAT_CONE: return "Cone NAT";
    case ZPSYNC_NAT_SYMMETRIC: return "Symmetric NAT/CGNAT";
    default: return "Unknown";
  }
}

static int zp_connect_peer(const zpsync_stream_opts_t *opts, zpsync_result_s *out_res) {
  zpsync_config_s cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.secret = (const uint8_t *)opts->secret;
  cfg.secret_len = strlen(opts->secret);
  cfg.manual_peer_ip = (opts->peer_ip && opts->peer_ip[0] != '\0') ? opts->peer_ip : NULL;
  cfg.manual_peer_port = opts->peer_port;
  cfg.local_port_override = opts->local_port;
  cfg.disable_sentinel = opts->disable_sentinel;
  cfg.force_birthday = opts->force_birthday;
  cfg.birthday_sockets = ZPSYNC_BIRTHDAY_DEFAULT_SOCKETS;

  log_out("[ZPSYNC] Connecting (%s)...\n",
          cfg.manual_peer_ip ? cfg.manual_peer_ip : "Mainline DHT + STUN");
  if (zpsync_connect(&cfg, out_res) != 0) {
    log_err("[ZPSYNC] Connection failed.\n");
    return -1;
  }
  log_out("[ZPSYNC] Connected! Path=%s | NAT=%s | Family=IPv%u | Session=0x%llx | Role=%s | Ports=%u->%u | RTT=%u ms | PFS=%s\n",
          zpsync_path_name(out_res->active_path),
          zp_nat_name(out_res->local_nat),
          (unsigned)out_res->family,
          (unsigned long long)out_res->session_id,
          out_res->local_is_initiator ? "Initiator" : "Responder",
          (unsigned)out_res->local_port,
          (unsigned)out_res->peer_port,
          (unsigned)out_res->rtt_ms,
          out_res->pfs_enabled ? "X25519" : "PSK");
  return 0;
}

static gpu_pipeline_t *create_pipeline(stream_codec_t codec, uint32_t width,
                                       uint32_t height, uint32_t fps,
                                       uint32_t bitrate_bps, bool show_window,
                                       gpu_pipeline_info_t *info) {
  gpu_pipeline_t *gpu = NULL;
  int rc = 0;
  if (codec == CODEC_TERNARY) {
    rc = gpu_pipeline_create_ternary(&gpu, width, height, fps, 6U, 4U, 256U,
                                     128U, show_window, info);
    if (rc == 0 && gpu) {
      gpu_pipeline_set_bitrate(gpu, bitrate_bps);
      gpu_pipeline_ternary_set_packed(gpu, true);
    }
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

/* What the receiver sends back on the control port (video port + 1):
 *   ZCSY                 its picture is out of sync: start an intra resync
 *   ZCEC [u32 words] [u32 caps]  it reads range-coded records, this many per
 *                        frame; caps bit 0: quarter-pel vectors, bit 1:
 *                        transform blocks, intra and inter (receivers that predate a feature
 *                        do not set its bit, and the sender leaves it off)
 *   ZCAC [u32 fid][u32 bytes]  ack/OWD feedback for the SCReAM/L4S pacer
 *   ZCNK id count idx..  fragments of a frame it is still missing */
typedef struct {
  SOCKET ctrl_sock;
  SOCKET tx_sock;
  zc_video_tx_t *tx;
  gpu_pipeline_t *gpu;
  struct sockaddr_in dst_addr;
  stream_codec_t codec;
  bool apply_impair;
  bool use_connected_sock;
  bool peer_reads_ec;
  uint32_t peer_caps;
  uint32_t resync_kf_frames;
  uint64_t frame_send_us[64];
  uint64_t nacks_received;
  uint64_t resyncs;
  /* Adaptive FEC: no parity while the receiver reports no loss; the
   * configured parity from the first retransmission request on, dropped
   * again after TERN_FEC_QUIET_US without one. */
  uint8_t fec_cfg;
  uint64_t last_loss_us;
} sender_ctrl_t;

#define TERN_FEC_QUIET_US 2000000ULL

static void sender_note_loss(sender_ctrl_t *c) {
  c->last_loss_us = qpc_now_us();
  if (c->tx->fec_shards != c->fec_cfg) {
    c->tx->fec_shards = c->fec_cfg;
    printf("[SENDER] loss reported: FEC on (%u parity shard%s per frame)\n",
           (unsigned)c->fec_cfg, c->fec_cfg > 1U ? "s" : "");
    fflush(stdout);
  }
}

static void sender_fec_tick(sender_ctrl_t *c) {
  if (c->tx->fec_shards != 0U && qpc_now_us() - c->last_loss_us > TERN_FEC_QUIET_US) {
    c->tx->fec_shards = 0U;
    printf("[SENDER] no loss for %llu s: FEC off\n",
           (unsigned long long)(TERN_FEC_QUIET_US / 1000000ULL));
    fflush(stdout);
  }
}

static void sender_poll_ctrl(sender_ctrl_t *c) {
  if (c->ctrl_sock == INVALID_SOCKET) return;
  uint8_t cbuf[ZC_NACK_HDR_SZ + 2U * ZC_NACK_MAX_FRAGS + 16U];
  bool sync_req = false;
  for (;;) {
    if (c->use_connected_sock) {
      WSAPOLLFD pfd = {0};
      pfd.fd = c->ctrl_sock;
      pfd.events = POLLRDNORM;
      if (WSAPoll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLRDNORM)) break;
    }
    int clen = c->use_connected_sock
                   ? recv(c->ctrl_sock, (char *)cbuf, sizeof(cbuf), 0)
                   : recvfrom(c->ctrl_sock, (char *)cbuf, sizeof(cbuf), 0, NULL, NULL);
    if (clen < 4) break;
    if (memcmp(cbuf, "ZCSY", 4) == 0) {
      sync_req = true;
    } else if (memcmp(cbuf, "ZCAC", 4) == 0 && clen >= 12) {
      uint32_t ack_fid = 0, ack_bytes = 0;
      memcpy(&ack_fid, cbuf + 4, 4);
      memcpy(&ack_bytes, cbuf + 8, 4);
      uint64_t sent_us = c->frame_send_us[ack_fid & 63U];
      uint64_t now_us = qpc_now_us();
      if (sent_us != 0ULL && now_us >= sent_us && ack_bytes > 0U) {
        uint32_t owd_us = (uint32_t)((now_us - sent_us) / 2ULL);
        if (owd_us == 0U) owd_us = 1U;
        zc_video_tx_on_ack(c->tx, owd_us, ack_bytes, 0U);
      }
    } else if (memcmp(cbuf, "ZCEC", 4) == 0 && c->codec == CODEC_TERNARY) {
      if (clen >= 8) {
        uint32_t limit = 0;
        memcpy(&limit, cbuf + 4, 4);
        gpu_pipeline_ternary_set_entropy_limit(c->gpu, limit);
      }
      uint32_t caps = 0;
      if (clen >= 12) memcpy(&caps, cbuf + 8, 4);
      if (caps != c->peer_caps) {
        c->peer_caps = caps;
        gpu_pipeline_ternary_set_qpel(c->gpu, (caps & 1U) != 0U);
        gpu_pipeline_ternary_set_transform(c->gpu, (caps & 2U) != 0U);
        gpu_pipeline_ternary_set_transform_inter(c->gpu, (caps & 2U) != 0U);
        printf("[SENDER] receiver decodes quarter-pel: %s, transform intra: %s\n",
               (caps & 1U) ? "on" : "off", (caps & 2U) ? "on" : "off");
        fflush(stdout);
      }
      if (!c->peer_reads_ec) {
        /* A receiver built before range-coded records existed never says
         * so and keeps getting the plain layout. */
        c->peer_reads_ec = true;
        gpu_pipeline_ternary_set_entropy(c->gpu, true);
        printf("[SENDER] receiver accepts range-coded records: enabled\n");
        fflush(stdout);
      }
    } else if (memcmp(cbuf, ZC_NACK_MAGIC, 4) == 0 && clen >= (int)ZC_NACK_HDR_SZ) {
      uint32_t frame_id = 0;
      uint16_t count = 0;
      memcpy(&frame_id, cbuf + 4, 4);
      memcpy(&count, cbuf + 8, 2);
      if (count <= ZC_NACK_MAX_FRAGS && clen >= (int)(ZC_NACK_HDR_SZ + 2U * count)) {
        uint16_t frags[ZC_NACK_MAX_FRAGS];
        memcpy(frags, cbuf + ZC_NACK_HDR_SZ, (size_t)count * 2U);
        c->nacks_received++;
        sender_note_loss(c);
        zc_video_tx_resend(c->tx, (uintptr_t)c->tx_sock,
                           c->use_connected_sock ? NULL : (const struct sockaddr *)&c->dst_addr,
                           c->use_connected_sock ? 0 : (int)sizeof(c->dst_addr),
                           frame_id, frags, count,
                           qpc_now_us(), c->apply_impair);
      }
    }
  }
  if (sync_req) sender_note_loss(c);
  if (sync_req && c->codec == CODEC_TERNARY) {
    c->resyncs++;
    c->resync_kf_frames = 4U;
    gpu_pipeline_ternary_request_keyframe(c->gpu);
  }
}

static int run_sender(stream_codec_t codec, const char *host, uint16_t port,
                      uint32_t width, uint32_t height, uint32_t fps,
                      uint32_t bitrate_bps, uint8_t fec_shards,
                      uint64_t max_frames, bool use_rio,
                      const zc_net_impair_t *sim,
                      const zpsync_stream_opts_t *zp_opts) {
  gpu_pipeline_info_t info;
  gpu_pipeline_t *gpu =
      create_pipeline(codec, width, height, fps, bitrate_bps, false, &info);
  if (!gpu) return 1;

  bool zp_active = (zp_opts && zp_opts->secret && zp_opts->secret[0] != '\0');
  zpsync_result_s zp_res;
  memset(&zp_res, 0, sizeof(zp_res));

  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  SOCKET tx_sock = INVALID_SOCKET;
  bool rio_enabled = false;

  if (zp_active) {
    if (zp_connect_peer(zp_opts, &zp_res) != 0) {
      gpu_pipeline_destroy(gpu);
      return 1;
    }
    tx_sock = (SOCKET)zp_res.sock;
    int sndbuf = 8 * 1024 * 1024;
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(tx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
    setsockopt(tx_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    use_rio = false;
  } else {
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dst_addr.sin_addr) != 1) {
      fprintf(stderr, "Invalid --host IPv4 address: %s\n", host);
      gpu_pipeline_destroy(gpu);
      return 1;
    }

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
  }

  zc_video_tx_t *tx = (zc_video_tx_t *)calloc(1, sizeof(*tx));
  zc_net_impair_t impair = *sim;
  const bool apply_impair = (impair.loss_pct > 0 || impair.burst_prob_pct > 0);
  if (!tx ||
      zc_video_tx_init(tx, RUDP_PAGE_POOL_PAGES, bitrate_bps, bitrate_bps / 4U,
                       bitrate_bps * 2U, fec_shards, &impair,
                       qpc_now_us()) != 0) {
    fprintf(stderr, "zc_video_tx_init failed\n");
    free(tx);
    if (zp_active) zpsync_close(&zp_res);
    else closesocket(tx_sock);
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
  if (zp_active) {
    printf("Target   : ZPSync (%s, %u -> %u) | RIO=OFF | FEC=%u shards\n",
           zpsync_path_name(zp_res.active_path), (unsigned)zp_res.local_port,
           (unsigned)zp_res.peer_port, (unsigned)fec_shards);
  } else {
    printf("Target   : %s:%u | RIO=%s | FEC=%u shards\n", host, (unsigned)port,
           rio_enabled ? "ON" : "OFF", (unsigned)fec_shards);
  }
  printf("Adapter  : %s\n", info.adapter_name);
  printf("Encoder  : %s (%ux%u @ %u fps, %.1f Mbps)\n", info.encoder_name,
         width, height, fps, (double)bitrate_bps / 1e6);
  printf("Capture  : %s\n",
         info.using_desktop_dup ? "DXGI Desktop Duplication" : "Synthetic fallback");
  fflush(stdout);

  /* Lost fragments are sent again on request: keep half a second of frames. */
  if (zc_video_tx_enable_history(tx, 64U) != 0) {
    fprintf(stderr, "retransmission history unavailable\n");
  }
  if (apply_impair) {
    printf("Simulated: %d%% loss, %d%% bursts of %d\n", impair.loss_pct,
           impair.burst_prob_pct, impair.burst_len);
  }

  SOCKET ctrl_sock = INVALID_SOCKET;
  if (zp_active) {
    ctrl_sock = tx_sock;
  } else if (port < 65535U) {
    ctrl_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctrl_sock != INVALID_SOCKET) {
      struct sockaddr_in caddr;
      memset(&caddr, 0, sizeof(caddr));
      caddr.sin_family = AF_INET;
      caddr.sin_addr.s_addr = htonl(INADDR_ANY);
      caddr.sin_port = htons((uint16_t)(port + 1U));
      if (bind(ctrl_sock, (const struct sockaddr *)&caddr, sizeof(caddr)) == 0) {
        u_long nb = 1;
        ioctlsocket(ctrl_sock, FIONBIO, &nb);
      } else {
        closesocket(ctrl_sock);
        ctrl_sock = INVALID_SOCKET;
      }
    }
  }

  const uint64_t frame_period_us = 1000000ULL / (fps ? fps : 60U);
  uint64_t next_tick_us = qpc_now_us();
  uint64_t last_report_us = next_tick_us;
  uint64_t frames_sent = 0;
  uint64_t bytes_period = 0;
  uint64_t frames_period = 0;
  zc_rate_budget_t rate_budget;
  zc_rate_budget_init(&rate_budget, tx, bitrate_bps);

  sender_ctrl_t ctl;
  memset(&ctl, 0, sizeof(ctl));
  ctl.ctrl_sock = ctrl_sock;
  ctl.tx_sock = tx_sock;
  ctl.tx = tx;
  ctl.gpu = gpu;
  ctl.dst_addr = dst_addr;
  ctl.codec = codec;
  ctl.apply_impair = apply_impair;
  ctl.use_connected_sock = zp_active;
  /* Parity only once losses show up (see sender_note_loss). */
  ctl.fec_cfg = tx->fec_shards;
  tx->fec_shards = 0U;

  for (uint32_t f = 0; InterlockedCompareExchange(&g_running, 1, 1) == 1; ++f) {
    if (max_frames > 0 && frames_sent >= max_frames) break;

    sender_poll_ctrl(&ctl);
    sender_fec_tick(&ctl);

    const uint8_t *bs = NULL;
    size_t bs_len = 0;
    bool keyframe = false;
    double cap_us = 0.0, enc_us = 0.0;
    int erc = gpu_pipeline_capture_and_encode(gpu, f, &bs, &bs_len, &keyframe,
                                              &cap_us, &enc_us);
    if (erc == 0 && bs && bs_len > 0) {
      uint64_t now_us = qpc_now_us();
      bool tx_kf = keyframe || (ctl.resync_kf_frames > 0U);
      if (ctl.resync_kf_frames > 0U) ctl.resync_kf_frames--;
      ctl.frame_send_us[frames_sent & 63U] = now_us;
      /* Transport ids count the frames actually sent: the receiver takes a
       * hole in them for a lost frame and waits for it. */
      zc_video_tx_send_frame(tx, (uintptr_t)tx_sock,
                             zp_active ? NULL : (const struct sockaddr *)&dst_addr,
                             zp_active ? 0 : (int)sizeof(dst_addr),
                             (uint32_t)frames_sent, bs, bs_len,
                             tx_kf, now_us, apply_impair);
      frames_sent++;
      frames_period++;
      bytes_period += bs_len;
      if (codec == CODEC_TERNARY) {
        /* The encoder gets what is left of the link once headers, parity
         * and retransmissions are paid for. */
        gpu_pipeline_set_bitrate(gpu, zc_rate_budget_update(&rate_budget, tx, bs_len));
      }
    }

    uint64_t now_us = qpc_now_us();
    if (now_us - last_report_us >= 1000000ULL) {
      double dt_s = (double)(now_us - last_report_us) / 1e6;
      double cur_fps = (double)frames_period / dt_s;
      double cur_mbps = ((double)bytes_period * 8.0) / (dt_s * 1e6);
      printf("[SENDER] %.1f fps | %.2f Mbps | total=%llu frames | pkts=%llu | errs=%llu | "
             "nacks=%llu resent=%llu resyncs=%llu\n",
             cur_fps, cur_mbps, (unsigned long long)frames_sent,
             (unsigned long long)tx->packets_sent,
             (unsigned long long)tx->send_errors,
             (unsigned long long)ctl.nacks_received,
             (unsigned long long)tx->frags_resent,
             (unsigned long long)ctl.resyncs);
      if (codec == CODEC_TERNARY) {
        uint64_t hf = 0, hm = 0, hmt = 0, hst = 0;
        gpu_pipeline_ternary_hint_stats(gpu, &hf, &hm, &hmt, &hst);
        printf("[SENDER] capture hints: %llu frames, %llu move rects, %llu moved tiles, %llu unchanged tiles\n",
               (unsigned long long)hf, (unsigned long long)hm, (unsigned long long)hmt,
               (unsigned long long)hst);
      }
      fflush(stdout);
      last_report_us = now_us;
      frames_period = 0;
      bytes_period = 0;
    }

    next_tick_us += frame_period_us;
    now_us = qpc_now_us();
    if (next_tick_us > now_us) {
      /* Retransmission requests are answered while waiting for the next
       * frame, not once per frame: that would add up to a frame period. */
      for (;;) {
        now_us = qpc_now_us();
        if (now_us >= next_tick_us) break;
        sender_poll_ctrl(&ctl);
        if (next_tick_us - now_us > 2000ULL) {
          Sleep(1);
        } else {
          YieldProcessor();
        }
      }
    } else if (now_us - next_tick_us > frame_period_us * 4ULL) {
      next_tick_us = now_us;
    }
  }

  printf("Sender stopped: %llu frames sent, %llu packets, %llu send errors, "
         "%llu dropped (simulated), %llu requests, %llu fragments resent, %llu resyncs.\n",
         (unsigned long long)frames_sent, (unsigned long long)tx->packets_sent,
         (unsigned long long)tx->send_errors,
         (unsigned long long)tx->packets_dropped_sim,
         (unsigned long long)ctl.nacks_received,
         (unsigned long long)tx->frags_resent, (unsigned long long)ctl.resyncs);

  bool had_rio = tx->rio_ready;
  zc_video_tx_destroy(tx);
  free(tx);
  if (zp_active) {
    zpsync_close(&zp_res);
  } else {
    if (ctrl_sock != INVALID_SOCKET) closesocket(ctrl_sock);
    if (!had_rio) closesocket(tx_sock);
  }
  gpu_pipeline_destroy(gpu);
  return 0;
}

static int run_receiver(stream_codec_t codec, uint16_t port, uint32_t width,
                        uint32_t height, uint32_t fps, uint32_t bitrate_bps,
                        uint64_t max_frames,
                        const zpsync_stream_opts_t *zp_opts) {
  gpu_pipeline_info_t info;
  gpu_pipeline_t *gpu =
      create_pipeline(codec, width, height, fps, bitrate_bps, true, &info);
  if (!gpu) return 1;

  bool zp_active = (zp_opts && zp_opts->secret && zp_opts->secret[0] != '\0');
  zpsync_result_s zp_res;
  memset(&zp_res, 0, sizeof(zp_res));

  SOCKET rx_sock = INVALID_SOCKET;
  if (zp_active) {
    if (zp_connect_peer(zp_opts, &zp_res) != 0) {
      gpu_pipeline_destroy(gpu);
      return 1;
    }
    rx_sock = (SOCKET)zp_res.sock;
    int rcvbuf = 8 * 1024 * 1024;
    int sndbuf = 8 * 1024 * 1024;
    setsockopt(rx_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    setsockopt(rx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
  } else {
    rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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
  }

  u_long nonblock = 1;
  ioctlsocket(rx_sock, FIONBIO, &nonblock);

  zc_video_rx_t *rx = (zc_video_rx_t *)calloc(1, sizeof(*rx));
  if (!rx || zc_video_rx_init(rx, 20 * 1024 * 1024) != 0) {
    fprintf(stderr, "zc_video_rx_init failed\n");
    free(rx);
    if (zp_active) zpsync_close(&zp_res);
    else closesocket(rx_sock);
    gpu_pipeline_destroy(gpu);
    return 1;
  }

  /* In-order delivery; a missing frame is waited for 100 ms at most, asked
   * for after 3 ms and then every 10 ms. */
  zc_video_rx_set_ordered(rx, 100000ULL, 3000ULL, 10000ULL);

  log_out("=== zcrudp_stream RECEIVER ===\n");
  if (zp_active) {
    log_out("Listening: ZPSync (%s, %u <- %u)\n",
            zpsync_path_name(zp_res.active_path), (unsigned)zp_res.local_port,
            (unsigned)zp_res.peer_port);
  } else {
    log_out("Listening: 0.0.0.0:%u\n", (unsigned)port);
  }
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
  uint64_t last_sync_req_us = 0;
  uint64_t last_caps_us = 0;
  uint64_t prev_drops = 0;
  bool need_sync = false;
  struct sockaddr_in sender_addr;
  memset(&sender_addr, 0, sizeof(sender_addr));
  bool have_sender_addr = zp_active;

  while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
    if (!gpu_pipeline_pump_events(gpu)) break;
    if (max_frames > 0 && frames_decoded >= max_frames) break;

    WSAPOLLFD pfd = {0};
    pfd.fd = rx_sock;
    pfd.events = POLLRDNORM;
    int prc = WSAPoll(&pfd, 1, 1);
    zc_video_rx_tick(rx, qpc_now_us());
    if (prc > 0 && (pfd.revents & POLLRDNORM)) {
      for (;;) {
        struct sockaddr_in src_addr;
        int src_len = (int)sizeof(src_addr);
        int rlen = zp_active
                       ? recv(rx_sock, (char *)pkt, sizeof(pkt), 0)
                       : recvfrom(rx_sock, (char *)pkt, sizeof(pkt), 0,
                                  (struct sockaddr *)&src_addr, &src_len);
        if (rlen <= 0) break;
        if (!zp_active && !have_sender_addr && src_len == (int)sizeof(src_addr)) {
          sender_addr = src_addr;
          sender_addr.sin_port = htons((uint16_t)(port + 1U));
          have_sender_addr = true;
        }
        pkts_period++;
        if (!seen_first_pkt) {
          seen_first_pkt = true;
          log_out("[RECEIVER] first UDP packet received (%d bytes)\n", rlen);
        }
        if (zc_video_rx_feed(rx, pkt, (size_t)rlen, NULL, NULL, NULL, NULL) < 0) {
          rx_err_period++;
        }
      }
    }

    /* Frames come out in order; one that is missing is waited for while the
     * requests below get its fragments sent again. */
    uint32_t ack_bytes_pending = 0;
    uint32_t last_ack_fid = 0;
    for (;;) {
      size_t rx_len = 0;
      uint32_t rx_fid = 0;
      bool rx_kf = false, rx_fec = false;
      if (zc_video_rx_next(rx, &rx_len, &rx_fid, &rx_kf, &rx_fec) != 1) break;
      if (!seen_first_frame) {
        seen_first_frame = true;
        if (rx_fid > 0 && !rx_kf) need_sync = true;
        log_out("[RECEIVER] first frame assembled: id=%u len=%zu kf=%d fec=%d\n",
                rx_fid, rx_len, rx_kf ? 1 : 0, rx_fec ? 1 : 0);
      } else if (rx_kf) {
        need_sync = false;
      }
      ack_bytes_pending += (uint32_t)rx_len;
      last_ack_fid = rx_fid;
      double dec_us = 0.0, prs_us = 0.0;
      int dec_rc = gpu_pipeline_decode_and_present(gpu, rx->frame_buf, rx_len,
                                                   &dec_us, &prs_us);
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

    if (have_sender_addr && (zp_active || port < 65535U)) {
      uint64_t now_ctl = qpc_now_us();
      if (ack_bytes_pending > 0U) {
        uint8_t ack_pkt[12];
        memcpy(ack_pkt, "ZCAC", 4);
        memcpy(ack_pkt + 4, &last_ack_fid, 4);
        memcpy(ack_pkt + 8, &ack_bytes_pending, 4);
        if (zp_active) {
          send(rx_sock, (const char *)ack_pkt, 12, 0);
        } else {
          sendto(rx_sock, (const char *)ack_pkt, 12, 0,
                 (const struct sockaddr *)&sender_addr, (int)sizeof(sender_addr));
        }
      }
      uint8_t nack[ZC_NACK_HDR_SZ + 2U * ZC_NACK_MAX_FRAGS];
      size_t nack_len;
      while ((nack_len = zc_video_rx_build_nack(rx, nack, sizeof(nack))) > 0) {
        if (zp_active) {
          send(rx_sock, (const char *)nack, (int)nack_len, 0);
        } else {
          sendto(rx_sock, (const char *)nack, (int)nack_len, 0,
                 (const struct sockaddr *)&sender_addr, (int)sizeof(sender_addr));
        }
      }
      if (codec == CODEC_TERNARY) {
        /* Capability notice, repeated since it travels unprotected: range
         * coding is understood, and this many record words per frame can be
         * decoded in time on this machine. */
        if (last_caps_us == 0 || now_ctl - last_caps_us >= 1000000ULL) {
          uint8_t caps[12];
          uint32_t budget = gpu_pipeline_ternary_entropy_budget(gpu);
          uint32_t features = 1U | 2U; /* quarter-pel vectors, transform intra */
          memcpy(caps, "ZCEC", 4);
          memcpy(caps + 4, &budget, 4);
          memcpy(caps + 8, &features, 4);
          if (zp_active) {
            send(rx_sock, (const char *)caps, 12, 0);
          } else {
            sendto(rx_sock, (const char *)caps, 12, 0, (const struct sockaddr *)&sender_addr,
                   (int)sizeof(sender_addr));
          }
          last_caps_us = now_ctl;
        }
        /* A frame given up on leaves the decoder with a stale reference.
         * Keep retrying ZCSY periodically until a kf_burst keyframe arrives. */
        if (rx->frames_dropped > prev_drops) {
          need_sync = true;
          prev_drops = rx->frames_dropped;
        }
        if (need_sync && (last_sync_req_us == 0 || now_ctl - last_sync_req_us >= 150000ULL)) {
          for (int rep_i = 0; rep_i < 2; ++rep_i) {
            if (zp_active) {
              send(rx_sock, "ZCSY", 4, 0);
            } else {
              sendto(rx_sock, "ZCSY", 4, 0, (const struct sockaddr *)&sender_addr,
                     (int)sizeof(sender_addr));
            }
          }
          last_sync_req_us = now_ctl;
        }
      }
    }

    uint64_t now_us = qpc_now_us();
    if (now_us - last_report_us >= 1000000ULL) {
      double dt_s = (double)(now_us - last_report_us) / 1e6;
      double cur_fps = (double)frames_period / dt_s;
      double cur_mbps = ((double)bytes_period * 8.0) / (dt_s * 1e6);
      log_out("[RECEIVER] %.1f fps | %.2f Mbps | decoded=%llu | fec_rec=%llu | dropped=%llu | requests=%llu | pkts=%llu | rx_err=%llu | dec_fail=%llu\n",
              cur_fps, cur_mbps, (unsigned long long)frames_decoded,
              (unsigned long long)rx->frames_fec_recovered,
              (unsigned long long)rx->frames_dropped,
              (unsigned long long)rx->nacks_built,
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
  if (zp_active) {
    zpsync_close(&zp_res);
  } else {
    closesocket(rx_sock);
  }
  gpu_pipeline_destroy(gpu);
  return 0;
}

int main(int argc, char **argv) {
  stream_role_t role = ROLE_NONE;
  stream_codec_t codec = CODEC_TERNARY;
  const char *host = "127.0.0.1";
  uint16_t port = 9900;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t fps = 120;
  uint32_t bitrate_mbps = 0;
  uint8_t fec_shards = 1;
  uint64_t max_frames = 0;
  bool use_rio = true;
  zc_net_impair_t sim;
  memset(&sim, 0, sizeof(sim));
  zpsync_stream_opts_t zp_opts;
  memset(&zp_opts, 0, sizeof(zp_opts));

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
    } else if (strcmp(argv[i], "--zpsync") == 0 && i + 1 < argc) {
      zp_opts.secret = argv[++i];
    } else if (strcmp(argv[i], "--zp-peer") == 0 && i + 1 < argc) {
      zp_opts.peer_ip = argv[++i];
    } else if (strcmp(argv[i], "--zp-port") == 0 && i + 1 < argc) {
      zp_opts.peer_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--zp-local-port") == 0 && i + 1 < argc) {
      zp_opts.local_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--zp-birthday") == 0) {
      zp_opts.force_birthday = true;
    } else if (strcmp(argv[i], "--zp-no-sentinel") == 0) {
      zp_opts.disable_sentinel = true;
    } else if (strcmp(argv[i], "--sim-loss") == 0 && i + 1 < argc) {
      sim.loss_pct = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--sim-burst") == 0 && i + 2 < argc) {
      sim.burst_prob_pct = atoi(argv[++i]);
      sim.burst_len = atoi(argv[++i]);
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
                    bitrate_mbps * 1000000U, fec_shards, max_frames, use_rio, &sim,
                    &zp_opts);
  } else {
    rc = run_receiver(codec, port, width, height, fps, bitrate_mbps * 1000000U,
                      max_frames, &zp_opts);
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
