#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "gpu_pipeline_win32.h"
#include "zcrudp_video_bridge.h"
#include "ternary_zcrudp_bridge.h"

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
  uint32_t bitrate_bps = 50000000;
  size_t num_frames = 200;
  uint8_t fec_shards = 1;
  bool show_window = false;
  bool measure_psnr = false;
  bool enable_rio = true;
  zc_net_impair_t impair = {0};

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--live") == 0) show_window = true;
    else if (strcmp(argv[i], "--psnr") == 0) measure_psnr = true;
    else if (strcmp(argv[i], "--no-rio") == 0) enable_rio = false;
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

  bool has_impair = (impair.loss_pct > 0 || impair.burst_prob_pct > 0 ||
                     impair.reorder_pct > 0 || impair.dup_pct > 0 ||
                     impair.base_owd_us > 0 || impair.jitter_us > 0);
  /* The bridge routes every datagram (FEC and delayed packets included)
   * through RIO once enabled, so RIO also works under simulated impairment. */
  bool want_rio = enable_rio;

  SOCKET rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  SOCKET tx_sock = want_rio
      ? WSASocketA(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0,
                   WSA_FLAG_REGISTERED_IO | WSA_FLAG_OVERLAPPED)
      : socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (tx_sock == INVALID_SOCKET) {
    want_rio = false;
    tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  }
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
  int rc = gpu_pipeline_create_ternary(&gpu, width, height, fps,
                                       6U, 8U, 250U, 128U,
                                       show_window, &info);
  if (rc != 0) {
    fprintf(stderr, "gpu_pipeline_create_ternary failed: %d\n", rc);
    return 1;
  }

  zc_video_tx_t *tx = (zc_video_tx_t *)calloc(1, sizeof(*tx));
  zc_video_rx_t *rx = (zc_video_rx_t *)calloc(1, sizeof(*rx));
  if (!tx || !rx ||
      zc_video_tx_init(tx, RUDP_PAGE_POOL_PAGES, bitrate_bps,
                       bitrate_bps / 4U, bitrate_bps * 2U, fec_shards,
                       &impair, qpc_now_us()) != 0 ||
      zc_video_rx_init(rx, 20 * 1024 * 1024) != 0) {
    fprintf(stderr, "zcrudp video bridge init failed\n");
    return 1;
  }

  if (want_rio) {
    /* A socket created with WSA_FLAG_REGISTERED_IO is only driven through
     * RIO here: if RIO setup fails, fall back to an ordinary socket. */
    int rio_rc = -1;
    if (connect(tx_sock, (struct sockaddr *)&bind_addr, addr_len) == 0) {
      rio_rc = zc_video_tx_enable_rio(tx, (uintptr_t)tx_sock);
    }
    if (rio_rc != 0) {
      fprintf(stderr, "Winsock RIO unavailable (%d), using WSASendTo\n", rio_rc);
      closesocket(tx_sock);
      tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (tx_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        return 1;
      }
      setsockopt(tx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
    }
  }

  printf("=== zcrudp::nextgen + Ternary HLSL Compute (cs_5_0) Zero-Copy Pipeline ===\n");
  printf("GPU Adapter   : %s\n", info.adapter_name);
  printf("Video Codec   : %s | RIO=%s\n", info.encoder_name,
         tx->rio_ready ? "ON" : "OFF");
  printf("Config        : %ux%u @ %u fps | Cauchy GF(2^8) FEC=%u shards\n",
         width, height, fps, fec_shards);
  printf("Net Impair    : loss=%d%% | burst=%d%% (len=%d) | reorder=%d%% | dup=%d%% | owd=%uus + jitter=%uus\n\n",
         impair.loss_pct, impair.burst_prob_pct, impair.burst_len,
         impair.reorder_pct, impair.dup_pct, impair.base_owd_us, impair.jitter_us);

  double *cap_arr = (double *)calloc(num_frames, sizeof(double));
  double *enc_arr = (double *)calloc(num_frames, sizeof(double));
  double *ntx_arr = (double *)calloc(num_frames, sizeof(double));
  double *nrx_arr = (double *)calloc(num_frames, sizeof(double));
  double *dec_arr = (double *)calloc(num_frames, sizeof(double));
  double *prs_arr = (double *)calloc(num_frames, sizeof(double));
  double *tot_arr = (double *)calloc(num_frames, sizeof(double));
  double *psnr_arr = measure_psnr ? (double *)calloc(num_frames, sizeof(double)) : NULL;

  size_t wire_cap = ZCRUDP_TERNARY_PKT_HDR_SZ + 20U * 1024U * 1024U;
  uint8_t *wire_buf = (uint8_t *)malloc(wire_cap);
  uint8_t rx_pkt[2048];
  /* Slices are capped at 280 words under impairment; 1 slice otherwise. */
  const uint32_t impaired_words_per_pkt = 280U;
  size_t slice_seen_cap = wire_cap / (impaired_words_per_pkt * sizeof(uint32_t)) + 1U;
  uint8_t *slice_seen = (uint8_t *)malloc(slice_seen_cap);
  if (!cap_arr || !enc_arr || !ntx_arr || !nrx_arr || !dec_arr || !prs_arr ||
      !tot_arr || (measure_psnr && !psnr_arr) || !wire_buf || !slice_seen) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  size_t measured = 0;
  size_t total_bytes = 0;
  size_t sent_frames = 0;
  const uint32_t warmup = 30;

  for (uint32_t f = 0; f < (uint32_t)(num_frames + warmup); ++f) {
    if (show_window && !gpu_pipeline_pump_events(gpu)) break;

    const uint8_t *bs = NULL;
    size_t bs_len = 0;
    bool keyframe = false;
    double cap_us = 0, enc_us = 0;

    if (gpu_pipeline_capture_and_encode(gpu, f, &bs, &bs_len, &keyframe,
                                        &cap_us, &enc_us) != 0 ||
        !bs || bs_len == 0) {
      continue;
    }

    if (f >= warmup) sent_frames++;

    /* bs layout: [uint32 record_words][uint32 mv_bytes][mvs][records] */
    uint32_t record_words = 0, mv_bytes = 0;
    memcpy(&record_words, bs, sizeof(uint32_t));
    if (bs_len < 8U) continue;
    memcpy(&mv_bytes, bs + sizeof(uint32_t), sizeof(uint32_t));
    if ((size_t)mv_bytes + (size_t)record_words * sizeof(uint32_t) > bs_len - 8U ||
        ZCRUDP_TERNARY_PKT_HDR_SZ + (size_t)mv_bytes > wire_cap) {
      continue;
    }
    const uint8_t *mv_ptr = bs + 8U;
    const uint8_t *rec_ptr = bs + 8U + mv_bytes; /* uint32 words, unaligned */

    /* Keep record_words even (2 words per record) and chunk at <= 280 words (1120B)
     * when network impairment is active (independent slice resilience);
     * on clean RIO, send records in 1 batched zc_video_tx_send_ternary_packet call (1 doorbell). */
    const uint32_t max_words_per_pkt = has_impair ? impaired_words_per_pkt
                                                  : (record_words > 0 ? record_words : 1U);
    uint32_t num_slices = (record_words == 0) ? 0U : ((record_words + max_words_per_pkt - 1U) / max_words_per_pkt);
    /* Transport ids are f * 2048 + 1 + slice: more would alias the next frame. */
    if (num_slices > 2047U || num_slices > slice_seen_cap) continue;
    memset(slice_seen, 0, num_slices);

    uint64_t t_tx0 = qpc_now_us();
    size_t frame_wire_bytes = 0;

    /* 1) Send Tile MVs on RELIABLE_FEC channel */
    {
      zcrudp_ternary_pkt_hdr_t mvhdr = {
          .magic = ZCRUDP_TERNARY_PKT_MAGIC,
          .pixfmt = ZCRUDP_TERNARY_MAKE_PF(ZCRUDP_TERNARY_FMT_GRAY8,
                                           ZCRUDP_TERNARY_PKT_TILE_MV),
          .plane = 0,
          .frame_id = (uint16_t)(f & 0xFFFFU),
          .start_block = num_slices,
          .num_tokens = (uint16_t)(mv_bytes / 8U)
      };
      zcrudp_ternary_pkt_hdr_write_le(wire_buf, &mvhdr);
      memcpy(wire_buf + ZCRUDP_TERNARY_PKT_HDR_SZ, mv_ptr, mv_bytes);
      size_t mv_wire_len = ZCRUDP_TERNARY_PKT_HDR_SZ + mv_bytes;
      frame_wire_bytes += mv_wire_len;
      zc_video_tx_send_ternary_packet(tx, (uintptr_t)tx_sock, &bind_addr, addr_len,
                                      f * 2048U, wire_buf, mv_wire_len, t_tx0, has_impair);
    }

    /* 2) Send record slices on UNRELIABLE_RIO channel (independent 1-UDP-pkt slices) */
    for (uint32_t s = 0; s < num_slices; ++s) {
      uint32_t off = s * max_words_per_pkt;
      uint32_t wcnt = record_words - off;
      if (wcnt > max_words_per_pkt) wcnt = max_words_per_pkt;

      zcrudp_ternary_pkt_hdr_t dhdr = {
          .magic = ZCRUDP_TERNARY_PKT_MAGIC,
          .pixfmt = ZCRUDP_TERNARY_MAKE_PF(ZCRUDP_TERNARY_FMT_GRAY8,
                                           ZCRUDP_TERNARY_PKT_DELTA),
          .plane = (uint8_t)(s + 1U == num_slices ? 1U : 0U),
          .frame_id = (uint16_t)(f & 0xFFFFU),
          .start_block = s,
          .num_tokens = (uint16_t)(wcnt > 0xFFFFU ? 0xFFFFU : wcnt) /* informative */
      };
      zcrudp_ternary_pkt_hdr_write_le(wire_buf, &dhdr);
      memcpy(wire_buf + ZCRUDP_TERNARY_PKT_HDR_SZ, rec_ptr + (size_t)off * sizeof(uint32_t),
             (size_t)wcnt * sizeof(uint32_t));
      size_t d_wire_len = ZCRUDP_TERNARY_PKT_HDR_SZ + (size_t)wcnt * sizeof(uint32_t);
      frame_wire_bytes += d_wire_len;
      zc_video_tx_send_ternary_packet(tx, (uintptr_t)tx_sock, &bind_addr, addr_len,
                                      f * 2048U + 1U + s, wire_buf, d_wire_len, t_tx0, has_impair);
    }
    uint64_t t_tx1 = qpc_now_us();

    /* Receive MVs + independent slices until frame complete or deadline */
    bool got_mv = false;
    uint32_t rx_mv_bytes = 0;
    uint32_t rx_record_words = 0;
    uint32_t rx_slices = 0;
    uint32_t expected_slices = num_slices;
    uint8_t *rx_dec_buf = wire_buf;
    uint8_t *rx_rec_dst = rx_dec_buf + 8U + mv_bytes;

    uint64_t t_rx0 = qpc_now_us();
    uint64_t last_rx_us = t_rx0;
    uint64_t wait_deadline = t_rx0 + (has_impair ? 20000ULL : 10000ULL);
    uint64_t idle_grace_us = has_impair ? 2500ULL : 200ULL;

    while (qpc_now_us() < wait_deadline) {
      if (has_impair) {
        zc_video_tx_flush_net(tx, (uintptr_t)tx_sock, &bind_addr, addr_len,
                              qpc_now_us());
      }
      int n = recv(rx_sock, (char *)rx_pkt, sizeof(rx_pkt), 0);
      if (n > 0) {
        last_rx_us = qpc_now_us();
        const uint8_t *rx_data = NULL;
        size_t rx_len = 0;
        uint32_t rx_fid = 0;
        bool rx_kf = false, rx_fec = false;
        int frc = zc_video_rx_feed_slice(rx, rx_pkt, (size_t)n, &rx_data, &rx_len,
                                         &rx_fid, &rx_kf, &rx_fec);
        if (frc == 1 && rx_len >= ZCRUDP_TERNARY_PKT_HDR_SZ) {
          zcrudp_ternary_pkt_hdr_t rhdr;
          if (zcrudp_ternary_pkt_hdr_read_le(rx_data, &rhdr) &&
              rhdr.frame_id == (uint16_t)(f & 0xFFFFU)) {
            uint8_t ptype = ZCRUDP_TERNARY_HDR_TYPE(&rhdr);
            if (ptype == ZCRUDP_TERNARY_PKT_TILE_MV) {
              rx_mv_bytes = (uint32_t)(rx_len - ZCRUDP_TERNARY_PKT_HDR_SZ);
              if (rx_mv_bytes == mv_bytes) {
                memcpy(rx_dec_buf + 8U, rx_data + ZCRUDP_TERNARY_PKT_HDR_SZ, rx_mv_bytes);
                got_mv = true;
              }
              expected_slices = rhdr.start_block;
            } else if (rhdr.start_block < num_slices && !slice_seen[rhdr.start_block]) {
              /* Records carry their own position, so slices may be appended
               * in arrival order; a duplicated datagram is dropped here. */
              slice_seen[rhdr.start_block] = 1U;
              uint32_t wbytes = (uint32_t)(rx_len - ZCRUDP_TERNARY_PKT_HDR_SZ);
              uint32_t wcnt = wbytes / (uint32_t)sizeof(uint32_t);
              if (8U + mv_bytes + (size_t)(rx_record_words + wcnt) * sizeof(uint32_t) <= wire_cap) {
                memcpy(rx_rec_dst + (size_t)rx_record_words * sizeof(uint32_t),
                       rx_data + ZCRUDP_TERNARY_PKT_HDR_SZ,
                       (size_t)wcnt * sizeof(uint32_t));
                rx_record_words += wcnt;
              }
              rx_slices++;
            }
            if (got_mv && rx_slices >= expected_slices) {
              break;
            }
          }
        }
      } else {
        if (got_mv && rx_slices >= expected_slices) break;
        if ((got_mv || rx_slices > 0) && (qpc_now_us() - last_rx_us) > idle_grace_us) break;
        Sleep(0);
      }
    }
    uint64_t t_rx1 = qpc_now_us();

    if (got_mv || rx_slices > 0) {
      if (!got_mv) {
        rx_mv_bytes = mv_bytes;
        memset(rx_dec_buf + 8U, 0, rx_mv_bytes);
      }
      memcpy(rx_dec_buf, &rx_record_words, sizeof(uint32_t));
      memcpy(rx_dec_buf + sizeof(uint32_t), &rx_mv_bytes, sizeof(uint32_t));
      size_t dec_in_len = 8U + (size_t)rx_mv_bytes + (size_t)rx_record_words * sizeof(uint32_t);

      double dec_us = 0, prs_us = 0;
      int drc = gpu_pipeline_decode_and_present(gpu, rx_dec_buf, dec_in_len,
                                                &dec_us, &prs_us);
      if (drc == 0 && f >= warmup && measured < num_frames) {
        cap_arr[measured] = cap_us;
        enc_arr[measured] = enc_us;
        ntx_arr[measured] = (double)(t_tx1 - t_tx0);
        nrx_arr[measured] = (double)(t_rx1 - t_rx0);
        dec_arr[measured] = dec_us;
        prs_arr[measured] = prs_us;
        tot_arr[measured] = cap_us + enc_us + ntx_arr[measured] +
                            nrx_arr[measured] + dec_us + prs_us;
        if (measure_psnr && psnr_arr) {
          double pdb = 0.0;
          if (gpu_pipeline_ternary_compute_psnr(gpu, &pdb) == 0) {
            psnr_arr[measured] = pdb;
          }
        }
        total_bytes += frame_wire_bytes;
        measured++;
      }
    }
  }

  printf("Measured frames: %zu / %zu (FEC recovered: %llu | dropped: %llu)\n",
         measured, sent_frames,
         (unsigned long long)rx->frames_fec_recovered,
         (unsigned long long)rx->frames_dropped);
  printf("Network TX     : %llu sent | %llu dropped(sim) | %llu reordered | %llu dup | %llu FEC pkts | %llu send errors\n",
         (unsigned long long)tx->packets_sent,
         (unsigned long long)tx->packets_dropped_sim,
         (unsigned long long)tx->packets_reordered_sim,
         (unsigned long long)tx->packets_duplicated_sim,
         (unsigned long long)tx->fec_packets_sent,
         (unsigned long long)tx->send_errors);
  if (measured > 0) {
    double avg_mbps = ((double)total_bytes * 8.0 * (double)fps) /
                      ((double)measured * 1000000.0);
    printf("Avg bitrate    : %.2f Mbps (%.1f KB/frame)\n\n",
           avg_mbps, ((double)total_bytes / (double)measured) / 1024.0);
    print_stat_row("1. Capture GPU", cap_arr, measured);
    print_stat_row("2. Ternary Enc", enc_arr, measured);
    print_stat_row("3. zcrudp TX", ntx_arr, measured);
    print_stat_row("4. zcrudp RX", nrx_arr, measured);
    print_stat_row("5. Ternary Dec", dec_arr, measured);
    print_stat_row("6. Present", prs_arr, measured);
    print_stat_row("TOTAL (Cap->Pr)", tot_arr, measured);

    if (measure_psnr && psnr_arr) {
      double psum = 0.0;
      for (size_t i = 0; i < measured; ++i) psum += psnr_arr[i];
      printf("  %-18s | avg: %7.2f dB\n", "Y-PSNR (VRAM)", psum / (double)measured);
      /* Closed-loop check: on a lossless link the encoder reference and the
       * decoder reconstruction must be bit-identical. */
      uint64_t mismatch_px = 0;
      if (gpu_pipeline_ternary_ref_mismatch(gpu, &mismatch_px) == 0) {
        printf("  %-18s | %llu px differ (last frame)\n", "Enc/Dec reference",
               (unsigned long long)mismatch_px);
      }
    }
  }

  free(slice_seen);
  free(wire_buf);
  free(cap_arr);
  free(enc_arr);
  free(ntx_arr);
  free(nrx_arr);
  free(dec_arr);
  free(prs_arr);
  free(tot_arr);
  free(psnr_arr);
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
