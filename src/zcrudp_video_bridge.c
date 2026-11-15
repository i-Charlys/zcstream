#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include "zcrudp_video_bridge.h"
#include "ternary_zcrudp_bridge.h"
#include <stdlib.h>
#include <string.h>

/* Winsock Registered I/O (RIO) ABI definitions (host OS layer) */
typedef struct RIO_BUFFERID_t *ZC_RIO_BUFFERID;
typedef struct RIO_CQ_t *ZC_RIO_CQ;
typedef struct RIO_RQ_t *ZC_RIO_RQ;

#ifndef WSA_FLAG_REGISTERED_IO
#define WSA_FLAG_REGISTERED_IO 0x100
#endif
#ifndef SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER
#define SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER _WSAIORW(IOC_WS2, 36)
#endif
#define ZC_RIO_MSG_DEFER       0x00000002
#define ZC_RIO_MSG_COMMIT_ONLY 0x00000008
#define ZC_RIO_INVALID_BUFID   ((ZC_RIO_BUFFERID)(ULONG_PTR)-1)

typedef struct {
  ZC_RIO_BUFFERID BufferId;
  ULONG Offset;
  ULONG Length;
} ZC_RIO_BUF;

typedef struct {
  LONG Status;
  ULONG BytesTransferred;
  ULONGLONG SocketContext;
  ULONGLONG RequestContext;
} ZC_RIORESULT;

typedef enum {
  ZC_RIO_EVENT_COMPLETION = 1,
  ZC_RIO_IOCP_COMPLETION = 2
} ZC_RIO_NOTIFICATION_COMPLETION_TYPE;

typedef struct {
  ZC_RIO_NOTIFICATION_COMPLETION_TYPE Type;
  union {
    struct { HANDLE EventHandle; BOOL NotifyReset; } Event;
    struct { HANDLE IocpHandle; PVOID CompletionKey; PVOID Overlapped; } Iocp;
  };
} ZC_RIO_NOTIFICATION_COMPLETION;

typedef struct {
  DWORD cbSize;
  BOOL (PASCAL *RIOReceive)(ZC_RIO_RQ, ZC_RIO_BUF *, ULONG, DWORD, PVOID);
  BOOL (PASCAL *RIOReceiveEx)(ZC_RIO_RQ, ZC_RIO_BUF *, ULONG, ZC_RIO_BUF *, ZC_RIO_BUF *, ZC_RIO_BUF *, ZC_RIO_BUF *, DWORD, PVOID);
  BOOL (PASCAL *RIOSend)(ZC_RIO_RQ, ZC_RIO_BUF *, ULONG, DWORD, PVOID);
  BOOL (PASCAL *RIOSendEx)(ZC_RIO_RQ, ZC_RIO_BUF *, ULONG, ZC_RIO_BUF *, ZC_RIO_BUF *, ZC_RIO_BUF *, ZC_RIO_BUF *, DWORD, PVOID);
  void (PASCAL *RIOCloseCompletionQueue)(ZC_RIO_CQ);
  ZC_RIO_CQ (PASCAL *RIOCreateCompletionQueue)(DWORD, ZC_RIO_NOTIFICATION_COMPLETION *);
  ZC_RIO_RQ (PASCAL *RIOCreateRequestQueue)(SOCKET, ULONG, ULONG, ULONG, ULONG, ZC_RIO_CQ, ZC_RIO_CQ, PVOID);
  ULONG (PASCAL *RIODequeueCompletion)(ZC_RIO_CQ, ZC_RIORESULT *, ULONG);
  void (PASCAL *RIODeregisterBuffer)(ZC_RIO_BUFFERID);
  INT (PASCAL *RIONotify)(ZC_RIO_CQ);
  ZC_RIO_BUFFERID (PASCAL *RIORegisterBuffer)(PCHAR, DWORD);
  BOOL (PASCAL *RIOResizeCompletionQueue)(ZC_RIO_CQ, DWORD);
  BOOL (PASCAL *RIOResizeRequestQueue)(ZC_RIO_RQ, DWORD, DWORD);
} ZC_RIO_FN;

typedef struct {
  ZC_RIO_FN fn;
  ZC_RIO_BUFFERID buf_id;
  ZC_RIO_CQ cq;
  ZC_RIO_RQ rq;
  uint32_t ring_cursor;
  uint32_t ring_cap;
  uint32_t inflight;
  bool deferred; /* Sends queued with RIO_MSG_DEFER awaiting a commit */
} zc_rio_state_t;

_Static_assert(sizeof(zc_video_hdr_t) == 16, "zc_video_hdr_t is a 16-byte wire header");

/* One RIO ring slot: header + payload rounded up to a cache line. */
#define ZC_RIO_SLOT_BYTES \
  ((uint32_t)((sizeof(zc_video_hdr_t) + ZC_FRAG_MTU + 63U) & ~(size_t)63U))
#define ZC_RIO_MAX_OUTSTANDING 8192U

static uint32_t xorshift32(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x ? x : 0x12345678U;
  return x;
}

/* GF(2^8) with polynomial 0x11D (0x1D), matching zcrudp/src/nextgen.c */
static uint8_t gf256_mul_1d(uint8_t a, uint8_t b) {
  uint8_t p = 0U;
  for (unsigned i = 0U; i < 8U; ++i) {
    if (b & 1U) p ^= a;
    uint8_t hi = (uint8_t)(a & 0x80U);
    a <<= 1U;
    if (hi) a ^= 0x1DU;
    b >>= 1U;
  }
  return p;
}

static uint8_t gf256_inv_1d(uint8_t a) {
  if (a == 0U) return 0U;
  uint8_t r = 1U;
  uint8_t base = a;
  uint8_t exp = 254U;
  while (exp > 0U) {
    if (exp & 1U) r = gf256_mul_1d(r, base);
    base = gf256_mul_1d(base, base);
    exp >>= 1U;
  }
  return r;
}

/*
 * Extended Cauchy matrix: row 0 is all ones (plain XOR), row p >= 1 is
 * 1 / (x_p ^ y_i) with x_p = p + 1 in [2, 4] and y_i in [16, 254]. The two
 * sets are disjoint and the y_i are distinct for data indices less than
 * ZC_FEC_COLUMN_PERIOD apart, so any square submatrix over such columns is
 * invertible.
 */
static uint8_t cauchy_coeff(uint8_t parity_idx, uint16_t data_idx) {
  if (parity_idx == 0U) return 1U;
  uint8_t x = (uint8_t)(parity_idx + 1U);
  uint8_t y = (uint8_t)((data_idx % ZC_FEC_COLUMN_PERIOD) + 16U);
  return gf256_inv_1d((uint8_t)(x ^ y));
}

/* Length every data fragment must have, given the frame geometry. */
static uint16_t expected_frag_len(uint16_t frag_idx, uint16_t total_frags,
                                  uint16_t tail_len) {
  return (frag_idx + 1U == total_frags) ? tail_len : (uint16_t)ZC_FRAG_MTU;
}

int zc_video_tx_init(zc_video_tx_t *tx, size_t pool_pages,
                     uint32_t initial_bps, uint32_t min_bps,
                     uint32_t max_bps, uint8_t fec_shards,
                     const zc_net_impair_t *impair, uint64_t now_us) {
  if (!tx || pool_pages == 0 || pool_pages > RUDP_PAGE_POOL_PAGES) {
    return -1;
  }
  memset(tx, 0, sizeof(*tx));
  tx->pool_pages = pool_pages;
  tx->fec_shards = (fec_shards > ZC_MAX_PARITY_SHARDS) ? ZC_MAX_PARITY_SHARDS : fec_shards;
  tx->impair_rng = 0xCAFEBABEU;
  if (impair) {
    tx->impair = *impair;
  }
  tx->pool_backing = (uint8_t *)VirtualAlloc(
      NULL, pool_pages * RUDP_PAGE_SIZE, MEM_COMMIT | MEM_RESERVE,
      PAGE_READWRITE);
  if (!tx->pool_backing) {
    return -1;
  }
  /* Best effort: locking fails without a raised working-set quota. */
  (void)VirtualLock(tx->pool_backing, pool_pages * RUDP_PAGE_SIZE);

  if (rudp_page_pool_init(&tx->page_pool, tx->pool_backing, pool_pages) != 0 ||
      rudp_pacer_init(&tx->pacer, initial_bps / 8U, min_bps / 8U, max_bps / 8U,
                      2000U, now_us) != 0) {
    VirtualFree(tx->pool_backing, 0, MEM_RELEASE);
    tx->pool_backing = NULL;
    return -1;
  }
  return 0;
}

int zc_video_tx_enable_rio(zc_video_tx_t *tx, uintptr_t connected_udp_sock) {
  if (!tx || !tx->pool_backing || tx->rio_state) return -1;
  SOCKET sock = (SOCKET)connected_udp_sock;
  GUID rio_guid = {0x8509e081, 0x96dd, 0x4005,
                   {0xb1, 0x65, 0x9e, 0x2e, 0xe8, 0xc7, 0x9e, 0x3f}};

  DWORD pool_bytes = (DWORD)(tx->pool_pages * RUDP_PAGE_SIZE);
  if (pool_bytes < 2U * ZC_RIO_SLOT_BYTES) return -1;

  zc_rio_state_t *rs = (zc_rio_state_t *)calloc(1, sizeof(*rs));
  if (!rs) return -1;

  DWORD bytes_ret = 0;
  if (WSAIoctl(sock, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
               &rio_guid, sizeof(rio_guid), &rs->fn, sizeof(rs->fn),
               &bytes_ret, NULL, NULL) != 0) {
    free(rs);
    return -2;
  }

  rs->buf_id = rs->fn.RIORegisterBuffer((PCHAR)tx->pool_backing, pool_bytes);
  if (rs->buf_id == ZC_RIO_INVALID_BUFID) {
    free(rs);
    return -3;
  }

  rs->cq = rs->fn.RIOCreateCompletionQueue(2U * ZC_RIO_MAX_OUTSTANDING, NULL);
  if (!rs->cq) {
    rs->fn.RIODeregisterBuffer(rs->buf_id);
    free(rs);
    return -4;
  }

  rs->rq = rs->fn.RIOCreateRequestQueue(sock, 0, 1, ZC_RIO_MAX_OUTSTANDING, 1,
                                        rs->cq, rs->cq, NULL);
  if (!rs->rq) {
    rs->fn.RIOCloseCompletionQueue(rs->cq);
    rs->fn.RIODeregisterBuffer(rs->buf_id);
    free(rs);
    return -5;
  }

  rs->ring_cap = pool_bytes;
  rs->ring_cursor = 0;
  tx->rio_state = rs;
  tx->rio_ready = true;
  return 0;
}

void zc_video_tx_destroy(zc_video_tx_t *tx) {
  if (!tx) return;
  if (tx->rio_state) {
    zc_rio_state_t *rs = (zc_rio_state_t *)tx->rio_state;
    if (rs->cq) rs->fn.RIOCloseCompletionQueue(rs->cq);
    if (rs->buf_id && rs->buf_id != ZC_RIO_INVALID_BUFID) {
      rs->fn.RIODeregisterBuffer(rs->buf_id);
    }
    free(rs);
    tx->rio_state = NULL;
    tx->rio_ready = false;
  }
  if (tx->pool_backing) {
    VirtualUnlock(tx->pool_backing, tx->pool_pages * RUDP_PAGE_SIZE);
    VirtualFree(tx->pool_backing, 0, MEM_RELEASE);
    tx->pool_backing = NULL;
  }
}

static void rio_reap(zc_rio_state_t *rs) {
  ZC_RIORESULT results[256];
  ULONG done;
  while ((done = rs->fn.RIODequeueCompletion(rs->cq, results, 256)) > 0 &&
         done != (ULONG)-1 /* RIO_CORRUPT_CQ */) {
    rs->inflight = (rs->inflight >= done) ? (rs->inflight - done) : 0;
  }
}

/* Rings the kernel doorbell once for every send queued with RIO_MSG_DEFER. */
static void rio_commit(zc_rio_state_t *rs) {
  if (rs->deferred) {
    (void)rs->fn.RIOSend(rs->rq, NULL, 0, ZC_RIO_MSG_COMMIT_ONLY, NULL);
    rs->deferred = false;
  }
}

/* Copies one datagram into the registered ring and queues it (deferred). */
static bool rio_queue_datagram(zc_video_tx_t *tx, const uint8_t *hdr,
                               size_t hdr_len, const uint8_t *payload,
                               size_t payload_len) {
  zc_rio_state_t *rs = (zc_rio_state_t *)tx->rio_state;
  uint32_t pkt_bytes = (uint32_t)(hdr_len + payload_len);
  if (pkt_bytes == 0 || pkt_bytes > ZC_RIO_SLOT_BYTES) return false;

  /* A slot may be reused only once its send completed: keep the in-flight
   * window smaller than the ring and than the request queue. */
  uint32_t ring_slots = rs->ring_cap / ZC_RIO_SLOT_BYTES;
  uint32_t window = (ring_slots - 1U < ZC_RIO_MAX_OUTSTANDING)
      ? ring_slots - 1U : ZC_RIO_MAX_OUTSTANDING;
  for (unsigned spin = 0; rs->inflight >= window; ++spin) {
    rio_commit(rs);
    rio_reap(rs);
    if (rs->inflight < window) break;
    if (spin > 20000U) return false;
    SwitchToThread();
  }

  if (rs->ring_cursor + ZC_RIO_SLOT_BYTES > rs->ring_cap) rs->ring_cursor = 0;
  uint8_t *slot = tx->pool_backing + rs->ring_cursor;
  if (hdr_len) memcpy(slot, hdr, hdr_len);
  if (payload_len) memcpy(slot + hdr_len, payload, payload_len);

  ZC_RIO_BUF rb;
  rb.BufferId = rs->buf_id;
  rb.Offset = rs->ring_cursor;
  rb.Length = pkt_bytes;
  if (!rs->fn.RIOSend(rs->rq, &rb, 1, ZC_RIO_MSG_DEFER, NULL)) return false;

  rs->deferred = true;
  rs->inflight++;
  rs->ring_cursor += ZC_RIO_SLOT_BYTES;
  return true;
}

/*
 * Single exit point to the OS for every datagram: header + payload through
 * RIO when enabled, a scatter-gather WSASendTo otherwise.
 */
static bool tx_emit(zc_video_tx_t *tx, SOCKET sock, const struct sockaddr *dst,
                    int dst_len, const uint8_t *hdr, size_t hdr_len,
                    const uint8_t *payload, size_t payload_len) {
  bool ok;
  if (tx->rio_ready) {
    ok = rio_queue_datagram(tx, hdr, hdr_len, payload, payload_len);
  } else {
    WSABUF bufs[2];
    DWORD nbufs = 0;
    if (hdr_len) {
      bufs[nbufs].buf = (CHAR *)(uintptr_t)hdr;
      bufs[nbufs].len = (ULONG)hdr_len;
      nbufs++;
    }
    if (payload_len) {
      bufs[nbufs].buf = (CHAR *)(uintptr_t)payload;
      bufs[nbufs].len = (ULONG)payload_len;
      nbufs++;
    }
    DWORD sent = 0;
    ok = nbufs > 0 &&
         WSASendTo(sock, bufs, nbufs, &sent, 0, dst, dst_len, NULL, NULL) == 0;
  }
  if (ok) {
    tx->bytes_sent += hdr_len + payload_len;
    tx->packets_sent++;
  } else {
    tx->send_errors++;
  }
  return ok;
}

static void tx_flush(zc_video_tx_t *tx) {
  if (tx->rio_ready) {
    zc_rio_state_t *rs = (zc_rio_state_t *)tx->rio_state;
    rio_commit(rs);
    rio_reap(rs);
  }
}

static void enqueue_delayed_pkt(zc_video_tx_t *tx, const zc_video_hdr_t *hdr,
                                const uint8_t *payload, uint64_t deliver_at_us) {
  for (size_t i = 0; i < ZC_NET_QUEUE_CAP; ++i) {
    if (!tx->queue[i].valid) {
      memcpy(tx->queue[i].data, hdr, sizeof(*hdr));
      memcpy(tx->queue[i].data + sizeof(*hdr), payload, hdr->payload_len);
      tx->queue[i].len = sizeof(*hdr) + hdr->payload_len;
      tx->queue[i].deliver_at_us = deliver_at_us;
      tx->queue[i].valid = true;
      return;
    }
  }
  tx->send_errors++; /* Delay queue full: the packet is lost for real. */
}

static bool impair_active(const zc_video_tx_t *tx) {
  return tx->impair.loss_pct > 0 || tx->impair.burst_prob_pct > 0 ||
         tx->impair.reorder_pct > 0 || tx->impair.dup_pct > 0 ||
         tx->impair.base_owd_us > 0 || tx->impair.jitter_us > 0;
}

static bool impair_roll(zc_video_tx_t *tx, int pct) {
  return pct > 0 && (int)(xorshift32(&tx->impair_rng) % 100U) < pct;
}

static void emit_with_impair(zc_video_tx_t *tx, SOCKET sock,
                             const struct sockaddr *dst, int dst_len,
                             const zc_video_hdr_t *hdr, const uint8_t *payload,
                             uint64_t now_us, bool apply_impair) {
  bool is_fec = (hdr->flags & ZC_VIDEO_FLAG_PARITY) != 0;
  if (is_fec) tx->fec_packets_sent++;

  if (!apply_impair || !impair_active(tx)) {
    (void)tx_emit(tx, sock, dst, dst_len, (const uint8_t *)hdr, sizeof(*hdr),
                  payload, hdr->payload_len);
    return;
  }

  /* Gilbert-Elliott burst + random loss */
  bool drop = false;
  if (tx->burst_remaining > 0) {
    drop = true;
    tx->burst_remaining--;
  } else if (impair_roll(tx, tx->impair.burst_prob_pct)) {
    drop = true;
    tx->burst_remaining = (tx->impair.burst_len > 1) ? (tx->impair.burst_len - 1) : 0;
  } else if (impair_roll(tx, tx->impair.loss_pct)) {
    drop = true;
  }

  if (drop) {
    tx->packets_dropped_sim++;
    return;
  }

  uint64_t delay = tx->impair.base_owd_us;
  if (tx->impair.jitter_us > 0) {
    delay += xorshift32(&tx->impair_rng) % (tx->impair.jitter_us + 1U);
  }
  if (impair_roll(tx, tx->impair.reorder_pct)) {
    delay += 3500U + (xorshift32(&tx->impair_rng) % 4000U);
    tx->packets_reordered_sim++;
  }

  if (delay == 0) {
    (void)tx_emit(tx, sock, dst, dst_len, (const uint8_t *)hdr, sizeof(*hdr),
                  payload, hdr->payload_len);
  } else {
    enqueue_delayed_pkt(tx, hdr, payload, now_us + delay);
  }

  if (impair_roll(tx, tx->impair.dup_pct)) {
    tx->packets_duplicated_sim++;
    enqueue_delayed_pkt(tx, hdr, payload, now_us + delay + 200U);
  }
}

int zc_video_tx_flush_net(zc_video_tx_t *tx, uintptr_t udp_sock,
                          const void *dst_addr, int dst_addr_len,
                          uint64_t now_us) {
  if (!tx || !dst_addr) return -1;
  SOCKET sock = (SOCKET)udp_sock;
  const struct sockaddr *dst = (const struct sockaddr *)dst_addr;
  int flushed = 0;

  for (size_t i = 0; i < ZC_NET_QUEUE_CAP; ++i) {
    if (tx->queue[i].valid && tx->queue[i].deliver_at_us <= now_us) {
      if (tx_emit(tx, sock, dst, dst_addr_len, tx->queue[i].data,
                  tx->queue[i].len, NULL, 0)) {
        flushed++;
      }
      tx->queue[i].valid = false;
    }
  }
  tx_flush(tx);
  return flushed;
}

int zc_video_tx_send_frame(zc_video_tx_t *tx, uintptr_t udp_sock,
                           const void *dst_addr, int dst_addr_len,
                           uint32_t frame_id, const uint8_t *bitstream,
                           size_t len, bool is_keyframe, uint64_t now_us,
                           bool apply_impair) {
  if (!tx || !bitstream || len == 0 || !dst_addr) return -1;
  size_t frag_count = (len + ZC_FRAG_MTU - 1U) / ZC_FRAG_MTU;
  if (frag_count > ZC_MAX_FRAGS_PER_FRAME) return -1;
  uint16_t total_frags = (uint16_t)frag_count;
  uint16_t tail_len = (uint16_t)(len - (size_t)(total_frags - 1U) * ZC_FRAG_MTU);
  /* Parity shards cover the longest fragment (shorter ones are zero-padded). */
  uint16_t parity_len = (total_frags > 1U) ? (uint16_t)ZC_FRAG_MTU : tail_len;

  uint8_t parity_data[ZC_MAX_PARITY_SHARDS][ZC_FRAG_MTU];
  for (uint8_t p = 0; p < tx->fec_shards; ++p) {
    memset(parity_data[p], 0, ZC_FRAG_MTU);
  }

  SOCKET sock = (SOCKET)udp_sock;
  const struct sockaddr *dst = (const struct sockaddr *)dst_addr;

  /* Single pacer reservation for the whole frame burst. Accounting only: the
   * benchmarks send every frame immediately and never feed OWD samples back,
   * so a refused reservation is ignored rather than delaying the frame. */
  size_t est_wire = len + (size_t)total_frags * sizeof(zc_video_hdr_t);
  (void)rudp_pacer_refill_and_can_send(&tx->pacer, now_us, (uint32_t)est_wire);
  rudp_pacer_consume(&tx->pacer, (uint32_t)est_wire);

  zc_video_hdr_t hdr;
  hdr.magic = ZC_VIDEO_MAGIC;
  hdr.tail_len = tail_len;
  hdr.frame_id = frame_id;
  hdr.total_frags = total_frags;
  hdr.parity_count = tx->fec_shards;

  /* Data fragments point straight at the caller's bitstream (no staging copy
   * on the WSASendTo path; RIO copies once into its registered ring). */
  hdr.flags = is_keyframe ? ZC_VIDEO_FLAG_KEYFRAME : 0U;
  for (uint16_t i = 0; i < total_frags; ++i) {
    const uint8_t *src_slice = bitstream + (size_t)i * ZC_FRAG_MTU;
    uint16_t chunk = expected_frag_len(i, total_frags, tail_len);

    for (uint8_t p = 0; p < tx->fec_shards; ++p) {
      rudp_gf256_xor_scaled(parity_data[p], src_slice, chunk, cauchy_coeff(p, i));
    }

    hdr.frag_idx = i;
    hdr.payload_len = chunk;
    emit_with_impair(tx, sock, dst, dst_addr_len, &hdr, src_slice, now_us,
                     apply_impair);
  }

  hdr.flags |= ZC_VIDEO_FLAG_PARITY;
  hdr.payload_len = parity_len;
  for (uint8_t p = 0; p < tx->fec_shards; ++p) {
    hdr.frag_idx = (uint16_t)(total_frags + p);
    emit_with_impair(tx, sock, dst, dst_addr_len, &hdr, parity_data[p], now_us,
                     apply_impair);
  }

  /* One kernel doorbell for the whole frame on the RIO path. */
  tx_flush(tx);
  return 0;
}

int zc_video_rx_init(zc_video_rx_t *rx, size_t max_frame_bytes) {
  if (!rx || max_frame_bytes == 0) return -1;
  memset(rx, 0, sizeof(*rx));
  rx->active_frame_id = UINT32_MAX;
  rx->last_completed_frame_id = UINT32_MAX;
  rx->frame_cap = max_frame_bytes;
  rx->frame_buf = (uint8_t *)VirtualAlloc(NULL, max_frame_bytes,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
  rx->parity_buf = (uint8_t *)malloc(ZC_MAX_PARITY_SHARDS * ZC_FRAG_MTU);
  if (!rx->frame_buf || !rx->parity_buf) {
    zc_video_rx_destroy(rx);
    return -1;
  }
  (void)VirtualLock(rx->frame_buf, max_frame_bytes);
  return 0;
}

void zc_video_rx_destroy(zc_video_rx_t *rx) {
  if (!rx) return;
  if (rx->frame_buf) {
    VirtualUnlock(rx->frame_buf, rx->frame_cap);
    VirtualFree(rx->frame_buf, 0, MEM_RELEASE);
    rx->frame_buf = NULL;
  }
  free(rx->parity_buf);
  rx->parity_buf = NULL;
}

static void rx_reset_frame(zc_video_rx_t *rx, const zc_video_hdr_t *hdr) {
  if (rx->active_frame_id != UINT32_MAX &&
      rx->received_data_frags < rx->total_frags) {
    rx->frames_dropped++;
  }
  rx->active_frame_id = hdr->frame_id;
  rx->total_frags = hdr->total_frags;
  rx->tail_len = hdr->tail_len;
  rx->received_data_frags = 0;
  rx->flags = hdr->flags & ZC_VIDEO_FLAG_KEYFRAME;
  rx->parity_received_count = 0;
  rx->highest_frag_seen = 0;
  memset(rx->parity_present, 0, sizeof(rx->parity_present));
  memset(rx->parity_lens, 0, sizeof(rx->parity_lens));
  memset(rx->frag_present, 0, hdr->total_frags);
  memset(rx->frag_lens, 0, hdr->total_frags * sizeof(uint16_t));
}

/* Solve m x m linear system over GF(2^8) to recover up to 4 erased fragments */
static bool rx_recover_cauchy(zc_video_rx_t *rx) {
  uint16_t missing[ZC_MAX_PARITY_SHARDS];
  uint8_t m = 0;
  for (uint16_t i = 0; i < rx->total_frags; ++i) {
    if (!rx->frag_present[i]) {
      if (m >= ZC_MAX_PARITY_SHARDS) return false;
      missing[m++] = i;
    }
  }
  if (m == 0) return true;
  if (rx->parity_received_count < m) return false;

  uint8_t p_used[ZC_MAX_PARITY_SHARDS];
  uint8_t pc = 0;
  for (uint8_t p = 0; p < ZC_MAX_PARITY_SHARDS && pc < m; ++p) {
    if (rx->parity_present[p]) p_used[pc++] = p;
  }
  if (pc < m) return false;

  /* Every stored parity shard was validated to this length on arrival. */
  uint16_t rec_len = (rx->total_frags > 1U) ? (uint16_t)ZC_FRAG_MTU : rx->tail_len;

  uint8_t mat[ZC_MAX_PARITY_SHARDS][ZC_MAX_PARITY_SHARDS];
  uint8_t rhs[ZC_MAX_PARITY_SHARDS][ZC_FRAG_MTU];

  for (uint8_t r = 0; r < m; ++r) {
    uint8_t p = p_used[r];
    memcpy(rhs[r], rx->parity_buf + (size_t)p * ZC_FRAG_MTU, rec_len);
    for (uint16_t i = 0; i < rx->total_frags; ++i) {
      if (rx->frag_present[i]) {
        uint8_t c = cauchy_coeff(p, i);
        rudp_gf256_xor_scaled(rhs[r], rx->frame_buf + (size_t)i * ZC_FRAG_MTU,
                              rx->frag_lens[i], c);
      }
    }
    for (uint8_t c = 0; c < m; ++c) {
      mat[r][c] = cauchy_coeff(p, missing[c]);
    }
  }

  /* Gauss-Jordan elimination in GF(2^8) */
  for (uint8_t col = 0; col < m; ++col) {
    uint8_t piv = col;
    while (piv < m && mat[piv][col] == 0U) piv++;
    if (piv == m) return false;
    if (piv != col) {
      for (uint8_t c = 0; c < m; ++c) {
        uint8_t tmp = mat[col][c];
        mat[col][c] = mat[piv][c];
        mat[piv][c] = tmp;
      }
      for (uint16_t k = 0; k < rec_len; ++k) {
        uint8_t tmp = rhs[col][k];
        rhs[col][k] = rhs[piv][k];
        rhs[piv][k] = tmp;
      }
    }
    uint8_t inv = gf256_inv_1d(mat[col][col]);
    for (uint8_t c = 0; c < m; ++c) mat[col][c] = gf256_mul_1d(mat[col][c], inv);
    for (uint16_t k = 0; k < rec_len; ++k) rhs[col][k] = gf256_mul_1d(rhs[col][k], inv);

    for (uint8_t r = 0; r < m; ++r) {
      if (r != col && mat[r][col] != 0U) {
        uint8_t factor = mat[r][col];
        for (uint8_t c = 0; c < m; ++c) {
          mat[r][c] ^= gf256_mul_1d(factor, mat[col][c]);
        }
        rudp_gf256_xor_scaled(rhs[r], rhs[col], rec_len, factor);
      }
    }
  }

  /* Check every destination before touching the frame: recovery is all or
   * nothing. The true fragment length comes from the frame geometry, not
   * from the zero-padded parity length. */
  for (uint8_t r = 0; r < m; ++r) {
    uint16_t flen = expected_frag_len(missing[r], rx->total_frags, rx->tail_len);
    if ((size_t)missing[r] * ZC_FRAG_MTU + flen > rx->frame_cap) return false;
  }
  for (uint8_t r = 0; r < m; ++r) {
    uint16_t idx = missing[r];
    uint16_t flen = expected_frag_len(idx, rx->total_frags, rx->tail_len);
    memcpy(rx->frame_buf + (size_t)idx * ZC_FRAG_MTU, rhs[r], flen);
    rx->frag_present[idx] = 1;
    rx->frag_lens[idx] = flen;
    rx->received_data_frags++;
  }
  return true;
}

/* Parses and sanity-checks one datagram header. */
static bool rx_parse_hdr(const uint8_t *pkt, size_t pkt_len, zc_video_hdr_t *hdr) {
  if (pkt_len < sizeof(*hdr)) return false;
  memcpy(hdr, pkt, sizeof(*hdr));
  return hdr->magic == ZC_VIDEO_MAGIC && hdr->total_frags != 0 &&
         hdr->total_frags <= ZC_MAX_FRAGS_PER_FRAME &&
         hdr->tail_len != 0 && hdr->tail_len <= ZC_FRAG_MTU &&
         hdr->payload_len <= ZC_FRAG_MTU &&
         hdr->parity_count <= ZC_MAX_PARITY_SHARDS &&
         pkt_len >= sizeof(*hdr) + hdr->payload_len;
}

int zc_video_rx_feed(zc_video_rx_t *rx, const uint8_t *pkt, size_t pkt_len,
                     size_t *out_len, uint32_t *out_frame_id,
                     bool *out_keyframe, bool *out_fec_used) {
  if (!rx || !pkt) return -1;
  zc_video_hdr_t hdr;
  if (!rx_parse_hdr(pkt, pkt_len, &hdr)) return -1;

  if (rx->last_completed_frame_id != UINT32_MAX &&
      (int32_t)(hdr.frame_id - rx->last_completed_frame_id) <= 0) {
    return 0;
  }

  if (rx->active_frame_id == UINT32_MAX ||
      (int32_t)(hdr.frame_id - rx->active_frame_id) > 0) {
    rx_reset_frame(rx, &hdr);
  } else if (hdr.frame_id != rx->active_frame_id) {
    return 0;
  }

  /* All datagrams of a frame must agree on its geometry. */
  if (hdr.total_frags != rx->total_frags || hdr.tail_len != rx->tail_len) {
    return -1;
  }

  if (hdr.frag_idx < rx->highest_frag_seen) {
    rx->ooo_packets_seen++;
  } else {
    rx->highest_frag_seen = hdr.frag_idx;
  }

  const uint8_t *payload = pkt + sizeof(hdr);

  if (hdr.flags & ZC_VIDEO_FLAG_PARITY) {
    uint16_t parity_len = (rx->total_frags > 1U) ? (uint16_t)ZC_FRAG_MTU : rx->tail_len;
    if (hdr.frag_idx < hdr.total_frags ||
        hdr.frag_idx - hdr.total_frags >= ZC_MAX_PARITY_SHARDS ||
        hdr.payload_len != parity_len) {
      return -1;
    }
    uint16_t p = (uint16_t)(hdr.frag_idx - hdr.total_frags);
    if (rx->parity_present[p]) {
      rx->dup_packets_ignored++;
    } else {
      memcpy(rx->parity_buf + (size_t)p * ZC_FRAG_MTU, payload, hdr.payload_len);
      rx->parity_lens[p] = hdr.payload_len;
      rx->parity_present[p] = 1;
      rx->parity_received_count++;
    }
  } else {
    if (hdr.frag_idx >= rx->total_frags ||
        hdr.payload_len != expected_frag_len(hdr.frag_idx, rx->total_frags,
                                             rx->tail_len)) {
      return -1;
    }
    if (rx->frag_present[hdr.frag_idx]) {
      rx->dup_packets_ignored++;
    } else {
      size_t dst_off = (size_t)hdr.frag_idx * ZC_FRAG_MTU;
      if (dst_off + hdr.payload_len > rx->frame_cap) return -2;
      memcpy(rx->frame_buf + dst_off, payload, hdr.payload_len);
      rx->frag_present[hdr.frag_idx] = 1;
      rx->frag_lens[hdr.frag_idx] = hdr.payload_len;
      rx->received_data_frags++;
    }
  }

  bool fec_used = false;
  if (rx->received_data_frags < rx->total_frags) {
    uint16_t missing_cnt = (uint16_t)(rx->total_frags - rx->received_data_frags);
    if (missing_cnt > rx->parity_received_count || !rx_recover_cauchy(rx)) {
      return 0;
    }
    rx->frames_fec_recovered++;
    fec_used = true;
  }

  rx->frames_completed++;
  if (out_len) {
    *out_len = (size_t)(rx->total_frags - 1U) * ZC_FRAG_MTU + rx->tail_len;
  }
  if (out_frame_id) *out_frame_id = rx->active_frame_id;
  if (out_keyframe) *out_keyframe = (rx->flags & ZC_VIDEO_FLAG_KEYFRAME) != 0;
  if (out_fec_used) *out_fec_used = fec_used;
  rx->last_completed_frame_id = rx->active_frame_id;
  rx->active_frame_id = UINT32_MAX;
  return 1;
}

int zc_video_tx_send_ternary_packet(zc_video_tx_t *tx, uintptr_t udp_sock,
                                    const void *dst_addr, int dst_addr_len,
                                    uint32_t pkt_seq_id, const uint8_t *pkt,
                                    size_t pkt_len, uint64_t now_us,
                                    bool apply_impair) {
  if (!tx || !pkt || pkt_len == 0) return -1;
  bool reliable = zcrudp_ternary_route_packet(pkt, pkt_len) ==
                  ZCRUDP_TERNARY_CHAN_RELIABLE_FEC;
  /* INTRA / TILE_MV always get at least one parity shard; DELTA gets none. */
  uint8_t saved_fec = tx->fec_shards;
  tx->fec_shards = reliable ? ((saved_fec > 0) ? saved_fec : 1U) : 0U;
  int rc = zc_video_tx_send_frame(tx, udp_sock, dst_addr, dst_addr_len,
                                  pkt_seq_id, pkt, pkt_len, reliable, now_us,
                                  apply_impair);
  tx->fec_shards = saved_fec;
  return rc;
}

int zc_video_rx_feed_slice(zc_video_rx_t *rx, const uint8_t *pkt, size_t pkt_len,
                           const uint8_t **out_data, size_t *out_len,
                           uint32_t *out_frame_id, bool *out_keyframe,
                           bool *out_fec_used) {
  if (!rx || !pkt || !out_data) return -1;
  zc_video_hdr_t hdr;
  if (!rx_parse_hdr(pkt, pkt_len, &hdr)) return -1;
  if (hdr.total_frags == 1U && hdr.parity_count == 0U &&
      !(hdr.flags & ZC_VIDEO_FLAG_PARITY)) {
    if (hdr.frag_idx != 0U || hdr.payload_len != hdr.tail_len) return -1;
    /* Not frame_buf: a multi-fragment frame may be mid-reassembly there. */
    memcpy(rx->slice_buf, pkt + sizeof(zc_video_hdr_t), hdr.payload_len);
    *out_data = rx->slice_buf;
    if (out_len) *out_len = hdr.payload_len;
    if (out_frame_id) *out_frame_id = hdr.frame_id;
    if (out_keyframe) *out_keyframe = (hdr.flags & ZC_VIDEO_FLAG_KEYFRAME) != 0;
    if (out_fec_used) *out_fec_used = false;
    return 1;
  }
  *out_data = rx->frame_buf;
  return zc_video_rx_feed(rx, pkt, pkt_len, out_len, out_frame_id,
                          out_keyframe, out_fec_used);
}
