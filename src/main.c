#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "gpu_pipeline_win32.h"
#include "zcrudp_video_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *a, const void *b) {
  double da = *(const double *)a;
  double db = *(const double *)b;
  return (da > db) - (da < db);
}

static void print_stat_row(const char *label, double *vals, size_t n) {
  if (n == 0) return;
  qsort(vals, n, sizeof(double), cmp_double);
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) sum += vals[i];
  double avg = sum / (double)n;
  double p50 = vals[(n * 50) / 100];
  double p95 = vals[(n * 95) / 100];
  double p99 = vals[(n * 99) / 100];
  printf("  %-18s | min: %7.1f us | p50: %7.1f us | p95: %7.1f us | p99: %7.1f us | avg: %7.1f us\n",
         label, vals[0], p50, p95, p99, avg);
}

int main(int argc, char **argv) {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t fps = 120;
  uint32_t bitrate_bps = 25000000;
  size_t num_frames = 240;
  uint8_t fec_shards = 1;
  bool show_window = false;
  zc_net_impair_t impair = {0};

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--live") == 0) show_window = true;
    else if (strcmp(argv[i], "--no-fec") == 0) fec_shards = 0;
    else if (strcmp(argv[i], "--fec") == 0 && i + 1 < argc) fec_shards = (uint8_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc) impair.loss_pct = atoi(argv[++i]);
    else if (strcmp(argv[i], "--burst") == 0 && i + 2 < argc) {
      impair.burst_prob_pct = atoi(argv[++i]);
      impair.burst_len = atoi(argv[++i]);
    }
    else if (strcmp(argv[i], "--reorder") == 0 && i + 1 < argc) impair.reorder_pct = atoi(argv[++i]);
    else if (strcmp(argv[i], "--dup") == 0 && i + 1 < argc) impair.dup_pct = atoi(argv[++i]);
    else if (strcmp(argv[i], "--owd") == 0 && i + 1 < argc) impair.base_owd_us = (uint32_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--jitter") == 0 && i + 1 < argc) impair.jitter_us = (uint32_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--nasty") == 0) {
      impair.loss_pct = 5;
      impair.burst_prob_pct = 8;
      impair.burst_len = 3;
      impair.reorder_pct = 15;
      impair.dup_pct = 5;
      impair.base_owd_us = 2000;
      impair.jitter_us = 3000;
    }
    else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) num_frames = (size_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width = (uint32_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) height = (uint32_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) bitrate_bps = (uint32_t)atoi(argv[++i]);
  }

  timeBeginPeriod(1);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }

  SOCKET rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  SOCKET tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (rx_sock == INVALID_SOCKET || tx_sock == INVALID_SOCKET) {
    fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
    return 1;
  }

  int rcvbuf = 4 * 1024 * 1024;
  setsockopt(rx_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
  setsockopt(tx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind_addr.sin_port = 0;
  if (bind(rx_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
    fprintf(stderr, "bind failed\n");
    return 1;
  }
  int addr_len = sizeof(bind_addr);
  getsockname(rx_sock, (struct sockaddr *)&bind_addr, &addr_len);

  u_long nonblock = 1;
  ioctlsocket(rx_sock, FIONBIO, &nonblock);

  gpu_pipeline_t *gpu = NULL;
  gpu_pipeline_info_t info;
  int rc = gpu_pipeline_create(&gpu, width, height, fps, bitrate_bps,
                               show_window, &info);
  if (rc != 0) {
    fprintf(stderr, "gpu_pipeline_create failed: %d\n", rc);
    return 1;
  }

  printf("=== zcrudp::nextgen + D3D11/DXGI Zero-Copy Video Pipeline ===\n");
  printf("GPU Adapter   : %s\n", info.adapter_name);
  printf("Video Encoder : %s | Decoder: %s\n", info.encoder_name, info.decoder_name);
  printf("Config        : %ux%u @ %u fps (%u Mbps) | Cauchy GF(2^8) FEC=%u shards\n",
         width, height, fps, bitrate_bps / 1000000U, fec_shards);
  printf("Net Impair    : loss=%d%% | burst=%d%% (len=%d) | reorder=%d%% | dup=%d%% | owd=%uus + jitter=%uus\n\n",
         impair.loss_pct, impair.burst_prob_pct, impair.burst_len,
         impair.reorder_pct, impair.dup_pct, impair.base_owd_us, impair.jitter_us);

  zc_video_tx_t *tx = (zc_video_tx_t *)calloc(1, sizeof(*tx));
  zc_video_rx_t *rx = (zc_video_rx_t *)calloc(1, sizeof(*rx));
  if (!tx || !rx ||
      zc_video_tx_init(tx, RUDP_PAGE_POOL_PAGES, bitrate_bps,
                       bitrate_bps / 4U, bitrate_bps * 2U, fec_shards,
                       &impair, qpc_now_us()) != 0 ||
      zc_video_rx_init(rx, 2 * 1024 * 1024) != 0) {
    fprintf(stderr, "zcrudp video bridge init failed\n");
    return 1;
  }

  double *cap_arr = (double *)calloc(num_frames, sizeof(double));
  double *enc_arr = (double *)calloc(num_frames, sizeof(double));
  double *ntx_arr = (double *)calloc(num_frames, sizeof(double));
  double *nrx_arr = (double *)calloc(num_frames, sizeof(double));
  double *dec_arr = (double *)calloc(num_frames, sizeof(double));
  double *prs_arr = (double *)calloc(num_frames, sizeof(double));
  double *tot_arr = (double *)calloc(num_frames, sizeof(double));
  if (!cap_arr || !enc_arr || !ntx_arr || !nrx_arr || !dec_arr || !prs_arr || !tot_arr) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  size_t measured = 0;
  size_t total_bytes = 0;
  size_t sent_frames = 0;

  uint8_t rx_pkt[2048];
  const uint32_t warmup = 60;
  /* Duplicates also sit in the bridge's delay queue (+200 us). */
  bool has_delay_queue = (impair.base_owd_us > 0 || impair.jitter_us > 0 ||
                          impair.reorder_pct > 0 || impair.dup_pct > 0);

  for (uint32_t f = 0; f < (uint32_t)(num_frames + warmup); ++f) {
    if (show_window && !gpu_pipeline_pump_events(gpu)) break;

    const uint8_t *bs = NULL;
    size_t bs_len = 0;
    bool keyframe = false;
    double cap_us = 0, enc_us = 0;

    int erc = gpu_pipeline_capture_and_encode(gpu, f, &bs, &bs_len, &keyframe,
                                              &cap_us, &enc_us);
    if (erc != 0 || bs_len == 0) continue;

    if (f >= warmup) sent_frames++;

    uint64_t t_tx0 = qpc_now_us();
    zc_video_tx_send_frame(tx, (uintptr_t)tx_sock, &bind_addr, addr_len,
                           f, bs, bs_len, keyframe, t_tx0, (f >= warmup));
    uint64_t t_tx1 = qpc_now_us();

    uint64_t t_rx0 = qpc_now_us();
    bool got_frame = false;
    size_t rx_len = 0;
    uint32_t rx_fid = 0;
    bool rx_kf = false, rx_fec = false;

    /* Loopback delivery is asynchronous: an empty socket right after the
     * send does not mean the frame is lost, so poll until the deadline.
     * Once a datagram of this frame was dropped on purpose and nothing more
     * is queued, the frame cannot complete and waiting is pointless. */
    uint64_t wait_limit_us = has_delay_queue
        ? (uint64_t)(impair.base_owd_us + impair.jitter_us + 8500U)
        : 2000U;
    uint64_t drops_before = tx->packets_dropped_sim;

    while ((qpc_now_us() - t_rx0) <= wait_limit_us) {
      if (has_delay_queue) {
        zc_video_tx_flush_net(tx, (uintptr_t)tx_sock, &bind_addr, addr_len, qpc_now_us());
      }
      int rlen = recvfrom(rx_sock, (char *)rx_pkt, sizeof(rx_pkt), 0, NULL, NULL);
      if (rlen > 0) {
        if (zc_video_rx_feed(rx, rx_pkt, (size_t)rlen, &rx_len, &rx_fid,
                             &rx_kf, &rx_fec) == 1) {
          if (rx_fid == f) {
            got_frame = true;
            break;
          }
          /* A late earlier frame: decode it to keep the H.264 reference
           * chain intact, but do not time it as the current frame. */
          double late_dec = 0, late_prs = 0;
          (void)gpu_pipeline_decode_and_present(gpu, rx->frame_buf, rx_len,
                                                &late_dec, &late_prs);
        }
      } else if (!has_delay_queue && tx->packets_dropped_sim != drops_before &&
                 (qpc_now_us() - t_rx0) > 300U) {
        break;
      }
    }
    uint64_t t_rx1 = qpc_now_us();

    if (!got_frame) continue;

    double dec_us = 0, prs_us = 0;
    int drc = gpu_pipeline_decode_and_present(gpu, rx->frame_buf, rx_len,
                                              &dec_us, &prs_us);
    if (drc != 0) continue;

    if (f >= warmup && measured < num_frames) {
      double ntx_us = (double)(t_tx1 - t_tx0);
      double nrx_us = (double)(t_rx1 - t_rx0);
      cap_arr[measured] = cap_us;
      enc_arr[measured] = enc_us;
      ntx_arr[measured] = ntx_us;
      nrx_arr[measured] = nrx_us;
      dec_arr[measured] = dec_us;
      prs_arr[measured] = prs_us;
      tot_arr[measured] = cap_us + enc_us + ntx_us + nrx_us + dec_us + prs_us;
      total_bytes += bs_len;
      measured++;
    }
  }

  printf("Frames Sent    : %zu | Delivered & Decoded: %zu (%.1f%%)\n",
         sent_frames, measured, sent_frames ? (100.0 * (double)measured / (double)sent_frames) : 0.0);
  printf("Network TX     : %llu sent | %llu dropped(sim) | %llu reordered | %llu dup | %llu FEC pkts | %llu send errors\n",
         (unsigned long long)tx->packets_sent,
         (unsigned long long)tx->packets_dropped_sim,
         (unsigned long long)tx->packets_reordered_sim,
         (unsigned long long)tx->packets_duplicated_sim,
         (unsigned long long)tx->fec_packets_sent,
         (unsigned long long)tx->send_errors);
  printf("Network RX     : %llu completed | %llu FEC-recovered | %llu ooo-pkts | %llu dup-ignored | %llu dropped\n\n",
         (unsigned long long)rx->frames_completed,
         (unsigned long long)rx->frames_fec_recovered,
         (unsigned long long)rx->ooo_packets_seen,
         (unsigned long long)rx->dup_packets_ignored,
         (unsigned long long)rx->frames_dropped);

  if (measured > 0) {
    double avg_mbps = ((double)total_bytes * 8.0 * (double)fps) /
                      ((double)measured * 1000000.0);
    printf("Avg bitrate    : %.2f Mbps (%.1f KB/frame)\n",
           avg_mbps, ((double)total_bytes / (double)measured) / 1024.0);
    printf("--- End-to-End Pipeline Latency Breakdown (microseconds) ---\n");
    print_stat_row("1. Capture+NV12", cap_arr, measured);
    print_stat_row("2. HW Encode", enc_arr, measured);
    print_stat_row("3. zcrudp TX", ntx_arr, measured);
    print_stat_row("4. Net+FEC RX", nrx_arr, measured);
    print_stat_row("5. HW Decode", dec_arr, measured);
    print_stat_row("6. Present(FLIP)", prs_arr, measured);
    printf("  --------------------------------------------------------------------------------------\n");
    print_stat_row("TOTAL GLASS-GLASS", tot_arr, measured);
  }

  free(cap_arr);
  free(enc_arr);
  free(ntx_arr);
  free(nrx_arr);
  free(dec_arr);
  free(prs_arr);
  free(tot_arr);
  zc_video_rx_destroy(rx);
  zc_video_tx_destroy(tx);
  free(rx);
  free(tx);
  gpu_pipeline_destroy(gpu);
  closesocket(rx_sock);
  closesocket(tx_sock);
  WSACleanup();
  timeEndPeriod(1);
  return 0;
}
