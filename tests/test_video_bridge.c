/*
 * Loopback tests for the video fragment bridge: fragmentation, Cauchy FEC
 * recovery (exact bytes and exact length), header validation and the
 * single-datagram slice path. No GPU required.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "zcrudp_video_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      exit(1);                                                             \
    }                                                                      \
  } while (0)

#define MAX_PKTS 256
#define PKT_CAP (sizeof(zc_video_hdr_t) + ZC_FRAG_MTU)

static SOCKET g_rx_sock, g_tx_sock;
static struct sockaddr_in g_addr;
static int g_addr_len;
static uint8_t g_pkts[MAX_PKTS][PKT_CAP];
static int g_pkt_lens[MAX_PKTS];

static uint64_t now_us(void) {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (uint64_t)((c.QuadPart * 1000000LL) / f.QuadPart);
}

static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed) {
  for (size_t i = 0; i < len; ++i) {
    seed = seed * 1664525U + 1013904223U;
    buf[i] = (uint8_t)(seed >> 24);
  }
}

/* Reads exactly `expected` datagrams from the loopback socket. */
static void collect(int expected) {
  uint64_t deadline = now_us() + 2000000ULL;
  int got = 0;
  CHECK(expected <= MAX_PKTS);
  while (got < expected && now_us() < deadline) {
    int n = recv(g_rx_sock, (char *)g_pkts[got], (int)PKT_CAP, 0);
    if (n > 0) {
      g_pkt_lens[got++] = n;
    } else {
      Sleep(0);
    }
  }
  CHECK(got == expected);
}

static bool in_set(const uint16_t *set, size_t n, uint16_t v) {
  for (size_t i = 0; i < n; ++i) {
    if (set[i] == v) return true;
  }
  return false;
}

/*
 * Sends one frame with `fec` parity shards, withholds the fragments listed in
 * `drop` (wire fragment indices, parity = total_frags + p), feeds the rest in
 * reverse order and returns whether the frame was delivered intact.
 */
static bool roundtrip(zc_video_tx_t *tx, zc_video_rx_t *rx, uint32_t frame_id,
                      size_t len, uint8_t fec, const uint16_t *drop,
                      size_t num_drop) {
  static uint8_t frame[64 * ZC_FRAG_MTU];
  CHECK(len <= sizeof(frame));
  fill_pattern(frame, len, frame_id * 2654435761U + 1U);

  tx->fec_shards = fec;
  CHECK(zc_video_tx_send_frame(tx, (uintptr_t)g_tx_sock, &g_addr, g_addr_len,
                               frame_id, frame, len, false, now_us(),
                               false) == 0);
  int total_frags = (int)((len + ZC_FRAG_MTU - 1U) / ZC_FRAG_MTU);
  int wire_pkts = total_frags + fec;
  collect(wire_pkts);

  bool delivered = false;
  for (int i = wire_pkts - 1; i >= 0; --i) {
    zc_video_hdr_t hdr;
    memcpy(&hdr, g_pkts[i], sizeof(hdr));
    if (in_set(drop, num_drop, hdr.frag_idx)) continue;

    size_t out_len = 0;
    uint32_t out_id = 0;
    bool kf = true, fec_used = false;
    int rc = zc_video_rx_feed(rx, g_pkts[i], (size_t)g_pkt_lens[i], &out_len,
                              &out_id, &kf, &fec_used);
    CHECK(rc == 0 || rc == 1);
    if (rc == 1) {
      CHECK(!delivered);
      CHECK(out_id == frame_id);
      CHECK(out_len == len);
      CHECK(memcmp(rx->frame_buf, frame, len) == 0);
      delivered = true;
    }
  }
  return delivered;
}

static void test_fec_recovery(zc_video_tx_t *tx, zc_video_rx_t *rx) {
  uint32_t id = 1;

  /* No loss, no parity; exact multiple of the fragment size and a 1-byte tail. */
  CHECK(roundtrip(tx, rx, id++, 3U * ZC_FRAG_MTU, 0, NULL, 0));
  CHECK(roundtrip(tx, rx, id++, 3U * ZC_FRAG_MTU + 1U, 0, NULL, 0));
  CHECK(roundtrip(tx, rx, id++, 1U, 0, NULL, 0));

  /* Lost LAST fragment: the short tail must come back with its true length. */
  const uint16_t last_of_5[] = {4};
  CHECK(roundtrip(tx, rx, id++, 4U * ZC_FRAG_MTU + 200U, 1, last_of_5, 1));

  /* Single-fragment frame rebuilt from its parity shard. */
  const uint16_t only[] = {0};
  CHECK(roundtrip(tx, rx, id++, 300U, 1, only, 1));

  /* Two losses 16 apart must have distinct coefficient columns. */
  const uint16_t apart16[] = {0, 16};
  CHECK(roundtrip(tx, rx, id++, 40U * ZC_FRAG_MTU + 7U, 2, apart16, 2));

  /* Four losses including the tail, using all four shards. */
  const uint16_t four[] = {0, 1, 17, 39};
  CHECK(roundtrip(tx, rx, id++, 40U * ZC_FRAG_MTU + 7U, 4, four, 4));

  /* Any subset of shards works: parity 0 and 2 are themselves lost. */
  const uint16_t mixed[] = {3, 9, 40, 42};
  CHECK(roundtrip(tx, rx, id++, 40U * ZC_FRAG_MTU + 7U, 4, mixed, 4));

  /* More losses than shards: the frame must not be delivered. */
  const uint16_t five[] = {0, 1, 2, 3, 4};
  CHECK(!roundtrip(tx, rx, id++, 40U * ZC_FRAG_MTU + 7U, 4, five, 5));

  /* ...and the receiver must recover cleanly on the next frame. */
  CHECK(roundtrip(tx, rx, id++, 2U * ZC_FRAG_MTU + 5U, 1, NULL, 0));
  printf("[TEST] FEC recovery, exact length and bytes... PASSED\n");
}

static void test_header_validation(zc_video_rx_t *rx) {
  uint8_t pkt[PKT_CAP];
  memset(pkt, 0xAB, sizeof(pkt));
  zc_video_hdr_t hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = ZC_VIDEO_MAGIC;
  hdr.tail_len = 100;
  hdr.frame_id = 1000;
  hdr.frag_idx = 0;
  hdr.total_frags = 2;
  hdr.payload_len = (uint16_t)ZC_FRAG_MTU;

#define FEED(h, len) (memcpy(pkt, &(h), sizeof(h)), \
                      zc_video_rx_feed(rx, pkt, (len), NULL, NULL, NULL, NULL))

  zc_video_hdr_t bad = hdr;
  CHECK(FEED(bad, sizeof(bad) - 1U) == -1); /* shorter than a header */
  CHECK(FEED(bad, sizeof(bad) + 10U) == -1); /* payload_len past the datagram */
  bad.magic = 0x1234;
  CHECK(FEED(bad, PKT_CAP) == -1);
  bad = hdr;
  bad.total_frags = 0;
  CHECK(FEED(bad, PKT_CAP) == -1);
  bad = hdr;
  bad.tail_len = 0;
  CHECK(FEED(bad, PKT_CAP) == -1);
  bad = hdr;
  bad.tail_len = (uint16_t)(ZC_FRAG_MTU + 1U);
  CHECK(FEED(bad, PKT_CAP) == -1);
  bad = hdr;
  bad.payload_len = 10; /* a non-final fragment must be full */
  CHECK(FEED(bad, PKT_CAP) == -1);

  CHECK(FEED(hdr, PKT_CAP) == 0); /* valid first fragment, frame pending */
  bad = hdr;
  bad.frag_idx = 1;
  bad.total_frags = 3; /* geometry differs from the frame in progress */
  CHECK(FEED(bad, PKT_CAP) == -1);
  bad = hdr;
  bad.frag_idx = 7; /* past the end of the frame */
  CHECK(FEED(bad, PKT_CAP) == -1);
  bad = hdr;
  bad.flags = ZC_VIDEO_FLAG_PARITY;
  bad.frag_idx = 2 + ZC_MAX_PARITY_SHARDS; /* parity shard out of range */
  CHECK(FEED(bad, PKT_CAP) == -1);

  hdr.frag_idx = 1;
  hdr.payload_len = 100;
  size_t out_len = 0;
  memcpy(pkt, &hdr, sizeof(hdr));
  CHECK(zc_video_rx_feed(rx, pkt, sizeof(hdr) + 100U, &out_len, NULL, NULL,
                         NULL) == 1);
  CHECK(out_len == ZC_FRAG_MTU + 100U);
#undef FEED
  printf("[TEST] Header validation... PASSED\n");
}

/* A lone slice arriving mid-reassembly must not disturb the pending frame. */
static void test_slice_does_not_clobber_frame(zc_video_tx_t *tx, zc_video_rx_t *rx) {
  static uint8_t frame[3 * ZC_FRAG_MTU];
  static uint8_t slice[500];
  fill_pattern(frame, sizeof(frame), 77U);
  fill_pattern(slice, sizeof(slice), 99U);

  tx->fec_shards = 0;
  CHECK(zc_video_tx_send_frame(tx, (uintptr_t)g_tx_sock, &g_addr, g_addr_len,
                               5000, frame, sizeof(frame), false, now_us(),
                               false) == 0);
  CHECK(zc_video_tx_send_frame(tx, (uintptr_t)g_tx_sock, &g_addr, g_addr_len,
                               5001, slice, sizeof(slice), false, now_us(),
                               false) == 0);
  collect(4);

  const int order[4] = {0, 3, 1, 2}; /* frag 0, the slice, frags 1 and 2 */
  bool got_slice = false, got_frame = false;
  for (int k = 0; k < 4; ++k) {
    int i = order[k];
    const uint8_t *data = NULL;
    size_t out_len = 0;
    uint32_t out_id = 0;
    int rc = zc_video_rx_feed_slice(rx, g_pkts[i], (size_t)g_pkt_lens[i], &data,
                                    &out_len, &out_id, NULL, NULL);
    if (rc != 1) continue;
    if (out_id == 5001) {
      CHECK(out_len == sizeof(slice) && memcmp(data, slice, sizeof(slice)) == 0);
      got_slice = true;
    } else {
      CHECK(out_id == 5000 && out_len == sizeof(frame));
      CHECK(memcmp(data, frame, sizeof(frame)) == 0);
      got_frame = true;
    }
  }
  CHECK(got_slice && got_frame);
  printf("[TEST] Slice during frame reassembly... PASSED\n");
}

int main(void) {
  WSADATA wsa;
  CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

  g_rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  g_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  CHECK(g_rx_sock != INVALID_SOCKET && g_tx_sock != INVALID_SOCKET);
  int rcvbuf = 1024 * 1024;
  setsockopt(g_rx_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));

  memset(&g_addr, 0, sizeof(g_addr));
  g_addr.sin_family = AF_INET;
  g_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(g_rx_sock, (struct sockaddr *)&g_addr, sizeof(g_addr)) == 0);
  g_addr_len = sizeof(g_addr);
  CHECK(getsockname(g_rx_sock, (struct sockaddr *)&g_addr, &g_addr_len) == 0);
  u_long nonblock = 1;
  ioctlsocket(g_rx_sock, FIONBIO, &nonblock);

  zc_video_tx_t *tx = (zc_video_tx_t *)calloc(1, sizeof(*tx));
  zc_video_rx_t *rx = (zc_video_rx_t *)calloc(1, sizeof(*rx));
  CHECK(tx && rx);
  CHECK(zc_video_tx_init(tx, 64, 100000000U, 1000000U, 400000000U, 0, NULL,
                         now_us()) == 0);
  CHECK(zc_video_rx_init(rx, 64U * ZC_FRAG_MTU) == 0);

  test_fec_recovery(tx, rx);
  test_header_validation(rx);
  test_slice_does_not_clobber_frame(tx, rx);
  CHECK(tx->send_errors == 0);

  zc_video_rx_destroy(rx);
  zc_video_tx_destroy(tx);
  free(rx);
  free(tx);
  closesocket(g_rx_sock);
  closesocket(g_tx_sock);
  WSACleanup();
  printf("=== ALL VIDEO BRIDGE TESTS PASSED ===\n");
  return 0;
}
