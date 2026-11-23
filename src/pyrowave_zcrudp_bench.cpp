#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "context.hpp"
#include "device.hpp"
#include "pyrowave_decoder.hpp"
#include "pyrowave_encoder.hpp"
#include "zcrudp_video_bridge.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Granite;
using namespace Vulkan;

static LARGE_INTEGER g_qpc_freq;

static uint64_t qpc_now_us(void) {
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

static void print_stat_row(const char *label, std::vector<double> &vals) {
  if (vals.empty()) return;
  std::sort(vals.begin(), vals.end());
  double sum = 0.0;
  for (double v : vals) sum += v;
  size_t n = vals.size();
  printf("  %-20s | min: %7.1f us | p50: %7.1f us | p95: %7.1f us | p99: %7.1f us | avg: %7.1f us\n",
         label, vals[0], vals[(n * 50) / 100], vals[(n * 95) / 100],
         vals[(n * 99) / 100], sum / (double)n);
}

struct YCbCrImages {
  ImageHandle images[3];
  PyroWave::ViewBuffers views;
};

static YCbCrImages create_ycbcr_images(Device &device, int width, int height,
                                       VkFormat fmt, PyroWave::ChromaSubsampling chroma) {
  YCbCrImages out;
  auto info = ImageCreateInfo::immutable_2d_image(width, height, fmt);
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;

  out.images[0] = device.create_image(info);
  if (chroma == PyroWave::ChromaSubsampling::Chroma420) {
    info.width >>= 1;
    info.height >>= 1;
  }
  out.images[1] = device.create_image(info);
  out.images[2] = device.create_image(info);

  for (int i = 0; i < 3; ++i) {
    out.views.planes[i] = &out.images[i]->get_view();
  }
  return out;
}

int main(int argc, char **argv) {
  int width = 1920;
  int height = 1080;
  uint32_t fps = 120;
  uint32_t target_frame_bytes = 125000; /* ~120 Mbps @ 120fps */
  size_t num_frames = 200;
  uint8_t fec_shards = 0;
  bool use_rio = true;
  zc_net_impair_t impair = {};

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--fec") == 0 && i + 1 < argc) fec_shards = (uint8_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--no-fec") == 0) fec_shards = 0;
    else if (strcmp(argv[i], "--no-rio") == 0) use_rio = false;
    else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
      int v = atoi(argv[++i]);
      fps = (v > 0) ? (uint32_t)v : 120U;
    }
    else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) num_frames = (size_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--target-bytes") == 0 && i + 1 < argc) target_frame_bytes = (uint32_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width = atoi(argv[++i]);
    else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) height = atoi(argv[++i]);
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
  }

  QueryPerformanceFrequency(&g_qpc_freq);
  timeBeginPeriod(1);
  HANDLE pace_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }

  SOCKET rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  SOCKET tx_sock = use_rio
      ? WSASocketA(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_REGISTERED_IO)
      : INVALID_SOCKET;
  bool rio_socket = (tx_sock != INVALID_SOCKET);
  if (!rio_socket) {
    tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  }
  if (rx_sock == INVALID_SOCKET || tx_sock == INVALID_SOCKET) {
    fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
    return 1;
  }
  int rcvbuf = 4 * 1024 * 1024;
  setsockopt(rx_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
  setsockopt(tx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in bind_addr = {};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind_addr.sin_port = 0;
  if (bind(rx_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
    fprintf(stderr, "bind failed\n");
    return 1;
  }
  int addr_len = sizeof(bind_addr);
  getsockname(rx_sock, (struct sockaddr *)&bind_addr, &addr_len);
  bool tx_connected = connect(tx_sock, (struct sockaddr *)&bind_addr, addr_len) == 0;
  u_long nonblock = 1;
  ioctlsocket(rx_sock, FIONBIO, &nonblock);

  if (!Context::init_loader(nullptr)) {
    fprintf(stderr, "Vulkan loader init failed\n");
    return 1;
  }
  Context ctx;
  if (!ctx.init_instance_and_device(nullptr, 0, nullptr, 0,
                                    CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT)) {
    fprintf(stderr, "Vulkan context init failed\n");
    return 1;
  }
  Device device;
  device.set_context(ctx);

  const auto &props = device.get_gpu_properties();
  auto chroma = PyroWave::ChromaSubsampling::Chroma420;
  auto enc_inputs = create_ycbcr_images(device, width, height, VK_FORMAT_R8_UNORM, chroma);
  auto dec_outputs = create_ycbcr_images(device, width, height, VK_FORMAT_R8_UNORM, chroma);

  /* Populate input planes in VRAM with a clean gradient pattern */
  {
    auto cmd = device.request_command_buffer();
    for (int i = 0; i < 3; ++i) {
      cmd->image_barrier(*enc_inputs.images[i], VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                         VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT);
      uint8_t *ptr = static_cast<uint8_t *>(cmd->update_image(*enc_inputs.images[i]));
      uint32_t pw = enc_inputs.images[i]->get_width();
      uint32_t ph = enc_inputs.images[i]->get_height();
      for (uint32_t y = 0; y < ph; ++y) {
        memset(ptr + (size_t)y * pw, (uint8_t)((y + i * 64) & 0xFF), pw);
      }
      cmd->image_barrier(*enc_inputs.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    device.submit(cmd);
    device.wait_idle();
  }

  PyroWave::Encoder enc;
  PyroWave::Decoder dec;
  if (!enc.init(&device, width, height, chroma)) {
    fprintf(stderr, "PyroWave::Encoder init failed\n");
    return 1;
  }
  bool frag_dec = PyroWave::Decoder::device_prefers_fragment_path(device);
  if (!dec.init(&device, width, height, chroma, frag_dec)) {
    fprintf(stderr, "PyroWave::Decoder init failed\n");
    return 1;
  }

  BufferCreateInfo bi = {};
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bi.size = enc.get_meta_required_size();
  bi.domain = BufferDomain::Device;
  auto meta_dev = device.create_buffer(bi);
  bi.domain = BufferDomain::CachedHost;
  auto meta_host = device.create_buffer(bi);

  bi.size = target_frame_bytes + 2 * enc.get_meta_required_size();
  bi.domain = BufferDomain::Device;
  auto bs_dev = device.create_buffer(bi);
  bi.domain = BufferDomain::CachedHost;
  auto bs_host = device.create_buffer(bi);

  PyroWave::Encoder::BitstreamBuffers bs_bufs = {};
  bs_bufs.meta.buffer = meta_dev.get();
  bs_bufs.meta.size = meta_dev->get_create_info().size;
  bs_bufs.bitstream.buffer = bs_dev.get();
  bs_bufs.bitstream.size = bs_dev->get_create_info().size;
  bs_bufs.target_size = target_frame_bytes;

  zc_video_tx_t *tx = (zc_video_tx_t *)calloc(1, sizeof(*tx));
  zc_video_rx_t *rx = (zc_video_rx_t *)calloc(1, sizeof(*rx));
  uint64_t bps_est64 = (uint64_t)target_frame_bytes * 8ULL * (uint64_t)fps;
  if (bps_est64 < 4000000ULL) bps_est64 = 4000000ULL;
  if (bps_est64 > 2000000000ULL) bps_est64 = 2000000000ULL;
  uint32_t bps_est = (uint32_t)bps_est64;
  size_t rx_buf_sz = std::max<size_t>(2 * 1024 * 1024, (size_t)target_frame_bytes * 2U + 65536U);
  if (!tx || !rx ||
      zc_video_tx_init(tx, RUDP_PAGE_POOL_PAGES, zc_pace_bps(bps_est, ZC_PACE_HEADROOM),
                       bps_est / 4U, zc_pace_bps(bps_est, 2U * ZC_PACE_HEADROOM),
                       fec_shards, &impair, qpc_now_us()) != 0 ||
      zc_video_rx_init(rx, rx_buf_sz) != 0) {
    fprintf(stderr, "zcrudp bridge init failed\n");
    return 1;
  }
  /* The bridge sends data, FEC parity and delayed packets through RIO once
   * enabled. Ordinary WSASendTo calls fail on a WSA_FLAG_REGISTERED_IO
   * socket, so without RIO the socket is replaced by a plain one. */
  if (rio_socket && tx_connected &&
      zc_video_tx_enable_rio(tx, (uintptr_t)tx_sock) == 0) {
    printf("Winsock RIO   : ENABLED (Registered I/O batch submit)\n");
  } else if (rio_socket) {
    closesocket(tx_sock);
    tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx_sock == INVALID_SOCKET) {
      fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
      return 1;
    }
    setsockopt(tx_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
  }

  printf("=== Pyrowave (Vulkan Wavelet GPU) + zcrudp::nextgen ===\n");
  printf("GPU Adapter   : %s (Vulkan)\n", props.deviceName);
  printf("Video Encoder : Pyrowave Wavelet (%s decode) | %dx%d @ %ufps Chroma420\n",
         frag_dec ? "Fragment" : "Compute", width, height, fps);
  printf("Target Frame  : %u bytes (~%.1f Mbps @ %ufps) | Cauchy FEC=%u shards\n\n",
         target_frame_bytes, (double)bps_est64 / 1e6, fps, fec_shards);

  std::vector<uint8_t> pkt_buf(target_frame_bytes + 65536);
  std::vector<double> enc_arr, ntx_arr, nrx_arr, dec_arr, tot_arr;
  size_t total_payload_bytes = 0;
  uint8_t rx_pkt[2048];
  const uint32_t warmup = 30;
  bool has_impair = (impair.loss_pct > 0 || impair.burst_prob_pct > 0 ||
                     impair.reorder_pct > 0 || impair.dup_pct > 0 ||
                     impair.base_owd_us > 0 || impair.jitter_us > 0);
  /* Duplicates also sit in the bridge's delay queue (+200 us). */
  bool has_delay = (impair.base_owd_us > 0 || impair.jitter_us > 0 ||
                    impair.reorder_pct > 0 || impair.dup_pct > 0);
  Fence fence;

  /* Frames start on their slot of the frame rate, as a capture would deliver
   * them (same pacing as zcvid_zcrudp_bench). Sent back to back, the pacer
   * made each frame wait for the previous one and the TX/total rows measured
   * that queueing instead of the transport. */
  const uint64_t frame_period_us = 1000000ULL / (fps ? fps : 120U);
  uint64_t next_frame_us = qpc_now_us();
  for (uint32_t f = 0; f < (uint32_t)(num_frames + warmup); ++f) {
    {
      uint64_t now = qpc_now_us();
      if (now < next_frame_us) {
        uint64_t rem = next_frame_us - now;
        if (pace_timer && rem > 600ULL) {
          LARGE_INTEGER due;
          due.QuadPart = -(LONGLONG)((rem - 350ULL) * 10ULL);
          if (SetWaitableTimer(pace_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(pace_timer, INFINITE);
          }
        } else if (rem > 1500ULL) {
          Sleep((DWORD)((rem - 1000ULL) / 1000ULL));
        }
        while (qpc_now_us() < next_frame_us) YieldProcessor();
      }
      next_frame_us += frame_period_us;
      now = qpc_now_us();
      if (now > next_frame_us + frame_period_us) next_frame_us = now; /* fell behind: no catch-up burst */
    }
    LARGE_INTEGER t0, t1, t2, t3, t4;
    QueryPerformanceCounter(&t0);

    /* 1. Pyrowave GPU Wavelet Encode + Readback */
    {
      auto cmd = device.request_command_buffer(CommandBuffer::Type::AsyncCompute);
      enc.encode(*cmd, enc_inputs.views, bs_bufs);
      cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      cmd->copy_buffer(*bs_host, *bs_dev);
      cmd->copy_buffer(*meta_host, *meta_dev);
      cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_HOST_READ_BIT);
      fence.reset();
      device.submit(cmd, &fence);
      fence->wait();
    }

    PyroWave::Encoder::Packet packet = {};
    enc.packetize(&packet, pkt_buf.size(), pkt_buf.data(), pkt_buf.size(),
                  device.map_host_buffer(*meta_host, MEMORY_ACCESS_READ_BIT),
                  device.map_host_buffer(*bs_host, MEMORY_ACCESS_READ_BIT));

    QueryPerformanceCounter(&t1);

    /* 2. zcrudp TX (PagePool + Scatter-Gather WSASendTo + FEC) */
    uint64_t now_us = qpc_now_us();
    zc_video_tx_send_frame(tx, (uintptr_t)tx_sock, &bind_addr, addr_len,
                           f, pkt_buf.data() + packet.offset, packet.size,
                           true, now_us, (f >= warmup) && has_impair);

    QueryPerformanceCounter(&t2);

    /* 3. zcrudp RX + Push into Pyrowave Decoder */
    bool got_frame = false;
    size_t rx_len = 0;
    uint32_t rx_fid = 0;
    bool rx_kf = false, rx_fec = false;
    uint64_t t_rx_start = qpc_now_us();
    /* Loopback delivery is asynchronous: poll until the deadline instead of
     * giving up on the first empty read. Frames are intra-only, so a late
     * earlier frame is simply skipped. */
    uint64_t wait_limit = has_delay ? (uint64_t)(impair.base_owd_us + impair.jitter_us + 8500U) : 2000U;
    uint64_t drops_before = tx->packets_dropped_sim;

    while ((qpc_now_us() - t_rx_start) <= wait_limit) {
      if (has_delay) {
        zc_video_tx_flush_net(tx, (uintptr_t)tx_sock, &bind_addr, addr_len, qpc_now_us());
      }
      int rlen = recvfrom(rx_sock, (char *)rx_pkt, sizeof(rx_pkt), 0, nullptr, nullptr);
      if (rlen > 0) {
        if (zc_video_rx_feed(rx, rx_pkt, (size_t)rlen, &rx_len, &rx_fid,
                             &rx_kf, &rx_fec) == 1 && rx_fid == f) {
          got_frame = true;
          break;
        }
      } else if (!has_delay && tx->packets_dropped_sim != drops_before &&
                 (qpc_now_us() - t_rx_start) > 300U) {
        break;
      }
    }

    bool ready = false;
    if (got_frame) {
      dec.clear();
      dec.push_packet(rx->frame_buf, rx_len);
      ready = dec.decode_is_ready(false);
    }

    QueryPerformanceCounter(&t3);

    /* 4. Pyrowave GPU Wavelet Decode */
    if (ready) {
      auto cmd = device.request_command_buffer();
      cmd->begin_barrier_batch();
      for (auto &plane : dec_outputs.views.planes) {
        cmd->image_barrier(plane->get_image(), VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
      }
      cmd->end_barrier_batch();
      dec.decode(*cmd, dec_outputs.views);
      fence.reset();
      device.submit(cmd, &fence);
      fence->wait();
    }

    QueryPerformanceCounter(&t4);
    device.next_frame_context();

    if (f >= warmup && ready) {
      double e_us = qpc_delta_us(t0, t1);
      double tx_us = qpc_delta_us(t1, t2);
      double rx_us = qpc_delta_us(t2, t3);
      double d_us = qpc_delta_us(t3, t4);
      enc_arr.push_back(e_us);
      ntx_arr.push_back(tx_us);
      nrx_arr.push_back(rx_us);
      dec_arr.push_back(d_us);
      tot_arr.push_back(e_us + tx_us + rx_us + d_us);
      total_payload_bytes += packet.size;
    }
  }

  double avg_kb = tot_arr.empty() ? 0.0 : ((double)total_payload_bytes / (double)tot_arr.size() / 1024.0);
  double avg_mbps = tot_arr.empty() ? 0.0 : (((double)total_payload_bytes / (double)tot_arr.size()) * 8.0 * (double)fps / 1e6);
  double deliv_pct = num_frames ? (100.0 * (double)tot_arr.size() / (double)num_frames) : 0.0;
  printf("Frames Sent    : %zu | Delivered & Decoded: %zu (%.1f%%)\n",
         num_frames, tot_arr.size(), deliv_pct);
  printf("Avg bitrate    : %.2f Mbps (%.1f KB/frame)\n", avg_mbps, avg_kb);
  printf("Network TX     : %llu sent | %llu dropped | %llu reordered | %llu dup | %llu FEC | %llu errors\n",
         (unsigned long long)tx->packets_sent,
         (unsigned long long)tx->packets_dropped_sim,
         (unsigned long long)tx->packets_reordered_sim,
         (unsigned long long)tx->packets_duplicated_sim,
         (unsigned long long)tx->fec_packets_sent,
         (unsigned long long)tx->send_errors);
  printf("Network RX     : %llu completed | %llu FEC-recovered | %llu dropped\n\n",
         (unsigned long long)rx->frames_completed,
         (unsigned long long)rx->frames_fec_recovered,
         (unsigned long long)rx->frames_dropped);

  printf("--- Pyrowave + zcrudp Pipeline Latency Breakdown (microseconds) ---\n");
  print_stat_row("1. Pyrowave Encode", enc_arr);
  print_stat_row("2. zcrudp TX", ntx_arr);
  print_stat_row("3. zcrudp RX", nrx_arr);
  print_stat_row("4. Pyrowave Decode", dec_arr);
  printf("  ----------------------------------------------------------------------------------------\n");
  print_stat_row("TOTAL GLASS-GLASS", tot_arr);

  device.wait_idle();
  zc_video_rx_destroy(rx);
  zc_video_tx_destroy(tx);
  free(rx);
  free(tx);
  closesocket(rx_sock);
  closesocket(tx_sock);
  WSACleanup();
  if (pace_timer) CloseHandle(pace_timer);
  timeEndPeriod(1);
  return 0;
}
