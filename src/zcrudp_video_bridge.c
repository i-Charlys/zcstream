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
  SOCKET sock;
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

  rs->sock = sock;
  rs->ring_cap = pool_bytes;
  rs->ring_cursor = 0;
  tx->rio_state = rs;
  tx->rio_ready = true;
  return 0;
}

static void rio_reap(zc_video_tx_t *tx, zc_rio_state_t *rs) {
  ZC_RIORESULT results[256];
  ULONG done;
  while ((done = rs->fn.RIODequeueCompletion(rs->cq, results, 256)) > 0 &&
         done != (ULONG)-1 /* RIO_CORRUPT_CQ */) {
    for (ULONG i = 0; i < done; ++i) {
      if (results[i].Status != 0 && tx) {
        tx->send_errors++;
      }
    }
    rs->inflight = (rs->inflight >= done) ? (rs->inflight - done) : 0;
  }
  if (done == (ULONG)-1 && tx) {
    tx->send_errors++;
  }
}

/* Rings the kernel doorbell once for every send queued with RIO_MSG_DEFER. */
static void rio_commit(zc_video_tx_t *tx, zc_rio_state_t *rs) {
  if (rs->deferred) {
    if (!rs->fn.RIOSend(rs->rq, NULL, 0, ZC_RIO_MSG_COMMIT_ONLY, NULL)) {
      if (tx) tx->send_errors++;
    }
    rs->deferred = false;
  }
}

void zc_video_tx_destroy(zc_video_tx_t *tx) {
  if (!tx) return;
  if (tx->history) {
    for (uint32_t i = 0; i < tx->history_frames; ++i) free(tx->history[i].data);
    free(tx->history);
    tx->history = NULL;
    tx->history_frames = 0;
  }
  if (tx->rio_state) {
    zc_rio_state_t *rs = (zc_rio_state_t *)tx->rio_state;
    for (unsigned spin = 0; rs->inflight > 0 && spin < 20000U; ++spin) {
      rio_commit(tx, rs);
      rio_reap(tx, rs);
      if (rs->inflight == 0) break;
      SwitchToThread();
    }
    if (rs->sock != INVALID_SOCKET) {
      closesocket(rs->sock);
      rs->sock = INVALID_SOCKET;
    }
    if (rs->cq) {
      rio_reap(tx, rs);
      rs->fn.RIOCloseCompletionQueue(rs->cq);
    }
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
    rio_commit(tx, rs);
    rio_reap(tx, rs);
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
  if (!rs->fn.RIOSend(rs->rq, &rb, 1, ZC_RIO_MSG_DEFER, NULL)) {
    rio_commit(tx, rs);
    return false;
  }

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
    rio_commit(tx, rs);
    rio_reap(tx, rs);
  }
}

static uint64_t bridge_now_us(void) {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (uint64_t)((c.QuadPart * 1000000LL) / f.QuadPart);
}

/* Refill tokens, pace when token-limited, and charge wire_bytes (including
 * parity shards and resends) against the SCReAM/L4S pacer budget. */
static void tx_pace_packet(zc_video_tx_t *tx, uint32_t wire_bytes,
                           uint64_t *cur_us) {
  if (!tx || wire_bytes == 0U || tx->pacer.rate_bytes_per_sec == 0U) return;
  bool can = rudp_pacer_refill_and_can_send(&tx->pacer, *cur_us, wire_bytes);
  if (!can && !tx->pacer.owd_initialized &&
      (uint64_t)tx->pacer.inflight_bytes + wire_bytes > tx->pacer.cwnd_bytes) {
    /* Callers without an ACK/OWD feedback loop cannot drain inflight_bytes. */
    tx->pacer.inflight_bytes = 0U;
    can = rudp_pacer_refill_and_can_send(&tx->pacer, *cur_us, wire_bytes);
  }
  if (!can) {
    uint32_t wait_us = rudp_pacer_next_wait_us(&tx->pacer, wire_bytes);
    if (wait_us > 0U && wait_us != UINT32_MAX) {
      tx_flush(tx);
      if (wait_us > 50000U) wait_us = 50000U;
      uint64_t t0 = bridge_now_us();
      if (wait_us >= 2000U) {
        Sleep(wait_us / 1000U);
      }
      while (bridge_now_us() - t0 < wait_us) {
        YieldProcessor();
      }
      uint64_t elapsed = bridge_now_us() - t0;
      *cur_us += (elapsed > 0U) ? elapsed : wait_us;
      (void)rudp_pacer_refill_and_can_send(&tx->pacer, *cur_us, wire_bytes);
    }
  }
  (void)rudp_pacer_consume(&tx->pacer, wire_bytes);
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
  if (tx->history) {
    zc_tx_history_t *e = &tx->history[frame_id % tx->history_frames];
    e->valid = false;
    if (len > e->cap) {
      uint8_t *d = (uint8_t *)realloc(e->data, len + len / 4U);
      if (d) {
        e->data = d;
        e->cap = len + len / 4U;
      }
    }
    if (len <= e->cap) {
      memcpy(e->data, bitstream, len);
      e->len = len;
      e->frame_id = frame_id;
      e->keyframe = is_keyframe;
      e->fec_shards = tx->fec_shards;
      e->valid = true;
    }
  }
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
  uint64_t cur_us = now_us;

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
    tx_pace_packet(tx, (uint32_t)(sizeof(hdr) + hdr.payload_len), &cur_us);
    emit_with_impair(tx, sock, dst, dst_addr_len, &hdr, src_slice, cur_us,
                     apply_impair);
  }

  hdr.flags |= ZC_VIDEO_FLAG_PARITY;
  hdr.payload_len = parity_len;
  for (uint8_t p = 0; p < tx->fec_shards; ++p) {
    hdr.frag_idx = (uint16_t)(total_frags + p);
    tx_pace_packet(tx, (uint32_t)(sizeof(hdr) + hdr.payload_len), &cur_us);
    emit_with_impair(tx, sock, dst, dst_addr_len, &hdr, parity_data[p], cur_us,
                     apply_impair);
  }

  /* One kernel doorbell for the whole frame on the RIO path. */
  tx_flush(tx);
  return 0;
}

/* Each entry holds a frame of this size from the start, pages touched, so
 * that keeping a frame for retransmission allocates nothing in the send
 * path (frames are tens of KB at 25-50 Mbps; a larger one still grows its
 * entry). */
#define ZC_HISTORY_ENTRY_BYTES (512U * 1024U)

int zc_video_tx_enable_history(zc_video_tx_t *tx, uint32_t frames) {
  if (!tx || frames == 0 || tx->history) return -1;
  tx->history = (zc_tx_history_t *)calloc(frames, sizeof(zc_tx_history_t));
  if (!tx->history) return -1;
  tx->history_frames = frames;
  for (uint32_t i = 0; i < frames; ++i) {
    zc_tx_history_t *e = &tx->history[i];
    e->data = (uint8_t *)malloc(ZC_HISTORY_ENTRY_BYTES);
    if (!e->data) break; /* the send path grows it when needed */
    memset(e->data, 0, ZC_HISTORY_ENTRY_BYTES);
    e->cap = ZC_HISTORY_ENTRY_BYTES;
  }
  return 0;
}

int zc_video_tx_resend(zc_video_tx_t *tx, uintptr_t udp_sock,
                       const void *dst_addr, int dst_addr_len,
                       uint32_t frame_id, const uint16_t *frags, uint16_t count,
                       uint64_t now_us, bool apply_impair) {
  if (!tx || !dst_addr || !tx->history) return 0;
  const zc_tx_history_t *e = &tx->history[frame_id % tx->history_frames];
  if (!e->valid || e->frame_id != frame_id) {
    tx->resend_misses += (count > 0U) ? count : 1U;
    return 0;
  }
  uint16_t total_frags = (uint16_t)((e->len + ZC_FRAG_MTU - 1U) / ZC_FRAG_MTU);
  uint16_t tail_len = (uint16_t)(e->len - (size_t)(total_frags - 1U) * ZC_FRAG_MTU);

  /* Same header as the first transmission: the receiver cannot tell them apart. */
  zc_video_hdr_t hdr;
  hdr.magic = ZC_VIDEO_MAGIC;
  hdr.tail_len = tail_len;
  hdr.frame_id = frame_id;
  hdr.total_frags = total_frags;
  hdr.parity_count = e->fec_shards;
  hdr.flags = e->keyframe ? ZC_VIDEO_FLAG_KEYFRAME : 0U;

  SOCKET sock = (SOCKET)udp_sock;
  const struct sockaddr *dst = (const struct sockaddr *)dst_addr;
  uint64_t cur_us = now_us;
  uint32_t n = (count > 0U) ? count : total_frags;
  int sent = 0;
  for (uint32_t k = 0; k < n; ++k) {
    uint16_t i = (count > 0U) ? frags[k] : (uint16_t)k;
    if (i >= total_frags) continue;
    hdr.frag_idx = i;
    hdr.payload_len = expected_frag_len(i, total_frags, tail_len);
    tx_pace_packet(tx, (uint32_t)(sizeof(hdr) + hdr.payload_len), &cur_us);
    emit_with_impair(tx, sock, dst, dst_addr_len, &hdr,
                     e->data + (size_t)i * ZC_FRAG_MTU, cur_us, apply_impair);
    sent++;
  }
  tx->frags_resent += (uint64_t)sent;
  tx_flush(tx);
  return sent;
}

void zc_video_tx_on_ack(zc_video_tx_t *tx, uint32_t owd_us,
                        uint32_t acked_bytes, uint8_t ce_marked) {
  if (!tx) return;
  (void)rudp_pacer_on_owd_sample(&tx->pacer, owd_us, acked_bytes, ce_marked);
}

void zc_rate_budget_init(zc_rate_budget_t *rb, const zc_video_tx_t *tx, uint32_t link_bps) {
  if (!rb) return;
  rb->link_bps = link_bps;
  rb->ratio_q16 = 1U << 16;
  rb->wire_mark = tx ? tx->bytes_sent : 0U;
  rb->video_acc = 0;
}

uint32_t zc_rate_budget_update(zc_rate_budget_t *rb, const zc_video_tx_t *tx,
                               size_t video_bytes) {
  if (!rb || !tx) return 0;
  rb->video_acc += video_bytes;
  uint64_t wire = tx->bytes_sent - rb->wire_mark;
  /* A sample once enough has gone out to mean something (about 16 KB):
   * small frames alone are mostly header. */
  if (wire >= 16384U && rb->video_acc > 0U) {
    uint64_t r = (rb->video_acc << 16) / wire;
    if (r > (1U << 16)) r = 1U << 16;
    if (r < (1U << 14)) r = 1U << 14; /* never below a quarter */
    rb->ratio_q16 = (uint32_t)(((uint64_t)rb->ratio_q16 * 7U + r) / 8U);
    rb->wire_mark = tx->bytes_sent;
    rb->video_acc = 0;
  }
  return (uint32_t)(((uint64_t)rb->link_bps * rb->ratio_q16) >> 16);
}

#define ZC_RX_SLOT_COMMIT ((size_t)1024U * 1024U)

int zc_video_rx_init(zc_video_rx_t *rx, size_t max_frame_bytes) {
  if (!rx || max_frame_bytes == 0) return -1;
  memset(rx, 0, sizeof(*rx));
  rx->last_completed_frame_id = UINT32_MAX;
  rx->frame_cap = max_frame_bytes;
  for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
    zc_video_rx_slot_t *s = &rx->slots[i];
    /* Address space only: a slot commits what its largest frame needed, so
     * the slots that only ever see small frames stay small. */
    s->buf = (uint8_t *)VirtualAlloc(NULL, max_frame_bytes, MEM_RESERVE, PAGE_READWRITE);
    s->parity_buf = (uint8_t *)malloc(ZC_MAX_PARITY_SHARDS * ZC_FRAG_MTU);
    s->frag_lens = (uint16_t *)malloc(ZC_MAX_FRAGS_PER_FRAME * sizeof(uint16_t));
    s->frag_present = (uint8_t *)malloc(ZC_MAX_FRAGS_PER_FRAME);
    if (!s->buf || !s->parity_buf || !s->frag_lens || !s->frag_present) {
      zc_video_rx_destroy(rx);
      return -1;
    }
  }
  /* Slot 0 is what callers read before any frame completed. */
  if (!VirtualAlloc(rx->slots[0].buf, max_frame_bytes, MEM_COMMIT, PAGE_READWRITE)) {
    zc_video_rx_destroy(rx);
    return -1;
  }
  rx->slots[0].buf_committed = max_frame_bytes;
  /* The other slots get ZC_RX_SLOT_COMMIT committed and touched now, so
   * that receiving an ordinary frame never commits or faults pages in the
   * receive path; only a frame larger than that still commits more. */
  for (uint32_t i = 1; i < ZC_RX_SLOTS; ++i) {
    zc_video_rx_slot_t *s = &rx->slots[i];
    size_t c = (ZC_RX_SLOT_COMMIT < max_frame_bytes) ? ZC_RX_SLOT_COMMIT : max_frame_bytes;
    if (VirtualAlloc(s->buf, c, MEM_COMMIT, PAGE_READWRITE)) {
      memset(s->buf, 0, c);
      s->buf_committed = c;
    }
  }
  for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
    memset(rx->slots[i].parity_buf, 0, ZC_MAX_PARITY_SHARDS * ZC_FRAG_MTU);
    memset(rx->slots[i].frag_lens, 0, ZC_MAX_FRAGS_PER_FRAME * sizeof(uint16_t));
    memset(rx->slots[i].frag_present, 0, ZC_MAX_FRAGS_PER_FRAME);
  }
  memset(rx->slots[0].buf, 0, (ZC_RX_SLOT_COMMIT < max_frame_bytes) ? ZC_RX_SLOT_COMMIT : max_frame_bytes);
  rx->frame_buf = rx->slots[0].buf;
  return 0;
}

void zc_video_rx_destroy(zc_video_rx_t *rx) {
  if (!rx) return;
  for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
    zc_video_rx_slot_t *s = &rx->slots[i];
    if (s->buf) VirtualFree(s->buf, 0, MEM_RELEASE);
    free(s->parity_buf);
    free(s->frag_lens);
    free(s->frag_present);
    memset(s, 0, sizeof(*s));
  }
  rx->frame_buf = NULL;
}

/* Gives up on whatever the slot holds. */
static void rx_release_slot(zc_video_rx_t *rx, zc_video_rx_slot_t *s) {
  /* total_frags == 0: a frame known only by the gap it left. */
  if (s->used && (s->total_frags == 0U || s->received_data_frags < s->total_frags)) {
    rx->frames_dropped++;
  }
  s->used = false;
}

static bool rx_start_frame(zc_video_rx_slot_t *s, const zc_video_hdr_t *hdr,
                           size_t frame_cap) {
  size_t need = (size_t)hdr->total_frags * ZC_FRAG_MTU;
  if (need > frame_cap) need = frame_cap;
  if (need > s->buf_committed) {
    need = (need + 0xFFFFFU) & ~(size_t)0xFFFFFU; /* 1 MB steps */
    if (need > frame_cap) need = frame_cap;
    if (!VirtualAlloc(s->buf, need, MEM_COMMIT, PAGE_READWRITE)) return false;
    s->buf_committed = need;
  }
  s->used = true;
  s->complete = false;
  s->fec_used = false;
  s->frame_id = hdr->frame_id;
  s->total_frags = hdr->total_frags;
  s->tail_len = hdr->tail_len;
  s->received_data_frags = 0;
  s->flags = hdr->flags & ZC_VIDEO_FLAG_KEYFRAME;
  s->parity_received_count = 0;
  s->highest_frag_seen = 0;
  memset(s->parity_present, 0, sizeof(s->parity_present));
  memset(s->parity_lens, 0, sizeof(s->parity_lens));
  memset(s->frag_present, 0, hdr->total_frags);
  memset(s->frag_lens, 0, hdr->total_frags * sizeof(uint16_t));
  return true;
}

static bool rx_recently_delivered(const zc_video_rx_t *rx, uint32_t frame_id) {
  for (uint32_t i = 0; i < rx->recent_count; ++i) {
    if (rx->recent_ids[i] == frame_id) return true;
  }
  return false;
}

/* The slot reassembling this frame, a new one for it, or NULL when the
 * datagram belongs to a frame older than everything in reassembly. */
static zc_video_rx_slot_t *rx_slot_for(zc_video_rx_t *rx, const zc_video_hdr_t *hdr) {
  zc_video_rx_slot_t *free_slot = NULL, *oldest = NULL;
  for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
    zc_video_rx_slot_t *s = &rx->slots[i];
    if (!s->used) {
      /* Never overwrite the buffer the caller still holds in rx->frame_buf. */
      if (s->buf != rx->frame_buf && !free_slot) free_slot = s;
      continue;
    }
    if (s->frame_id == hdr->frame_id) {
      if (s->total_frags == 0U) {
        /* Known so far only as a gap: this is its first datagram. */
        uint64_t first_us = s->first_us, last_nack_us = s->last_nack_us;
        uint64_t last_rx_us = s->last_rx_us;
        if (!rx_start_frame(s, hdr, rx->frame_cap)) return NULL;
        s->first_us = first_us;
        s->last_nack_us = last_nack_us;
        s->last_rx_us = last_rx_us;
      }
      return s;
    }
    if (s->buf != rx->frame_buf &&
        (!oldest || (int32_t)(s->frame_id - oldest->frame_id) < 0)) {
      oldest = s;
    }
  }
  if (!free_slot) {
    if (!oldest || (int32_t)(hdr->frame_id - oldest->frame_id) <= 0) return NULL;
    rx_release_slot(rx, oldest);
    free_slot = oldest;
  }
  if (!rx_start_frame(free_slot, hdr, rx->frame_cap)) return NULL;
  free_slot->first_us = rx->now_us;
  free_slot->last_rx_us = 0;
  free_slot->complete_us = 0;
  free_slot->last_nack_us = 0;
  return free_slot;
}

/* Solve m x m linear system over GF(2^8) to recover up to 4 erased fragments */
static bool rx_recover_cauchy(zc_video_rx_slot_t *rx, size_t frame_cap) {
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
        rudp_gf256_xor_scaled(rhs[r], rx->buf + (size_t)i * ZC_FRAG_MTU,
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
    if ((size_t)missing[r] * ZC_FRAG_MTU + flen > frame_cap) return false;
  }
  for (uint8_t r = 0; r < m; ++r) {
    uint16_t idx = missing[r];
    uint16_t flen = expected_frag_len(idx, rx->total_frags, rx->tail_len);
    memcpy(rx->buf + (size_t)idx * ZC_FRAG_MTU, rhs[r], flen);
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

void zc_video_rx_set_ordered(zc_video_rx_t *rx, uint64_t hold_us,
                             uint64_t nack_delay_us, uint64_t nack_interval_us) {
  if (!rx) return;
  rx->ordered = true;
  rx->allow_out_of_order = false;
  rx->hold_us = hold_us;
  rx->nack_delay_us = nack_delay_us;
  rx->nack_interval_us = nack_interval_us;
}

void zc_video_rx_tick(zc_video_rx_t *rx, uint64_t now_us) {
  if (rx) rx->now_us = now_us;
}

static zc_video_rx_slot_t *rx_find_slot(zc_video_rx_t *rx, uint32_t frame_id) {
  for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
    if (rx->slots[i].used && rx->slots[i].frame_id == frame_id) return &rx->slots[i];
  }
  return NULL;
}

/* Ordered mode: a datagram of frame_id shows that the frames between the
 * last one delivered and it exist. Those nothing was received of get a slot
 * without geometry, so that they are waited for and asked for too. */
static void rx_note_gap(zc_video_rx_t *rx, uint32_t frame_id) {
  if (rx->last_completed_frame_id == UINT32_MAX) return;
  int32_t ahead = (int32_t)(frame_id - rx->last_completed_frame_id);
  if (ahead <= 1 || ahead > 8) return; /* further apart: not worth holding for */
  for (uint32_t id = rx->last_completed_frame_id + 1U; id != frame_id; ++id) {
    if (rx_find_slot(rx, id)) continue;
    zc_video_rx_slot_t *fs = NULL;
    for (uint32_t i = 0; i < ZC_RX_SLOTS && !fs; ++i) {
      if (!rx->slots[i].used && rx->slots[i].buf != rx->frame_buf) fs = &rx->slots[i];
    }
    if (!fs) return;
    fs->used = true;
    fs->complete = false;
    fs->fec_used = false;
    fs->frame_id = id;
    fs->total_frags = 0;
    fs->received_data_frags = 0;
    fs->first_us = rx->now_us;
    fs->last_rx_us = 0;
    fs->complete_us = 0;
    fs->last_nack_us = 0;
  }
}

int zc_video_rx_next(zc_video_rx_t *rx, size_t *out_len, uint32_t *out_frame_id,
                     bool *out_keyframe, bool *out_fec_used) {
  if (!rx || !rx->ordered) return 0;
  for (;;) {
    zc_video_rx_slot_t *o = NULL;
    for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
      zc_video_rx_slot_t *s = &rx->slots[i];
      if (s->used && (!o || (int32_t)(s->frame_id - o->frame_id) < 0)) o = s;
    }
    if (!o) return 0;
    if (!o->complete) {
      /* The oldest frame is still short: everything waits for it, until
       * it has been given its time. */
      /* Counted from its last datagram: a large frame paced out at the
       * link rate legitimately takes longer than hold_us to arrive. */
      uint64_t since = (o->last_rx_us > o->first_us) ? o->last_rx_us : o->first_us;
      if (rx->now_us - since < rx->hold_us) return 0;
      rx_release_slot(rx, o);
      if (rx->last_completed_frame_id == UINT32_MAX ||
          (int32_t)(o->frame_id - rx->last_completed_frame_id) > 0) {
        rx->last_completed_frame_id = o->frame_id;
      }
      continue;
    }
    rx->frames_completed++;
    if (out_len) *out_len = (size_t)(o->total_frags - 1U) * ZC_FRAG_MTU + o->tail_len;
    if (out_frame_id) *out_frame_id = o->frame_id;
    if (out_keyframe) *out_keyframe = (o->flags & ZC_VIDEO_FLAG_KEYFRAME) != 0;
    if (out_fec_used) *out_fec_used = o->fec_used;
    rx->frame_buf = o->buf;
    o->used = false;
    rx->recent_ids[rx->recent_pos] = o->frame_id;
    rx->recent_pos = (rx->recent_pos + 1U) % ZC_RX_RECENT;
    if (rx->recent_count < ZC_RX_RECENT) rx->recent_count++;
    rx->last_completed_frame_id = o->frame_id;
    return 1;
  }
}

size_t zc_video_rx_build_nack(zc_video_rx_t *rx, uint8_t *buf, size_t cap) {
  if (!rx || !rx->ordered || !buf || cap < ZC_NACK_HDR_SZ + 2U * ZC_NACK_MAX_FRAGS) return 0;
  for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
    zc_video_rx_slot_t *s = &rx->slots[i];
    if (!s->used || s->complete) continue;
    if (rx->now_us - s->first_us < rx->nack_delay_us) continue;
    if (s->last_nack_us != 0 && rx->now_us - s->last_nack_us < rx->nack_interval_us) continue;

    /* The sender paces a frame out at the link rate, so fragments past the
     * highest one seen may simply not have been sent yet. While the frame
     * keeps arriving, only the fragments it skipped are missing (reordering
     * aside); once it has been silent for nack_delay_us, everything still
     * absent is: the tail was lost, or the whole frame. */
    uint64_t last = (s->last_rx_us > s->first_us) ? s->last_rx_us : s->first_us;
    bool stalled = (rx->now_us - last >= rx->nack_delay_us);
    uint16_t upto = s->total_frags;
    if (!stalled) {
      if (s->total_frags == 0U || s->highest_frag_seen == 0U) continue;
      upto = s->highest_frag_seen;
      bool gap = false;
      for (uint16_t f = 0; f < upto && !gap; ++f) gap = !s->frag_present[f];
      if (!gap) continue;
    }

    uint16_t count = 0;
    uint8_t *o = buf + ZC_NACK_HDR_SZ;
    if (s->total_frags != 0U) {
      for (uint16_t f = 0; f < upto; ++f) {
        if (s->frag_present[f]) continue;
        if (count == ZC_NACK_MAX_FRAGS) {
          count = 0; /* too many to list: the whole frame */
          break;
        }
        memcpy(o + (size_t)count * 2U, &f, 2);
        count++;
      }
    }
    memcpy(buf, ZC_NACK_MAGIC, 4);
    memcpy(buf + 4, &s->frame_id, 4);
    memcpy(buf + 8, &count, 2);
    s->last_nack_us = rx->now_us ? rx->now_us : 1U;
    rx->nacks_built++;
    return ZC_NACK_HDR_SZ + (size_t)count * 2U;
  }
  return 0;
}

int zc_video_rx_feed(zc_video_rx_t *rx, const uint8_t *pkt, size_t pkt_len,
                     size_t *out_len, uint32_t *out_frame_id,
                     bool *out_keyframe, bool *out_fec_used) {
  if (!rx || !pkt) return -1;
  zc_video_hdr_t hdr;
  if (!rx_parse_hdr(pkt, pkt_len, &hdr)) return -1;

  if (rx_recently_delivered(rx, hdr.frame_id)) {
    rx->dup_packets_ignored++;
    return 0;
  }
  if (!rx->allow_out_of_order && rx->last_completed_frame_id != UINT32_MAX &&
      (int32_t)(hdr.frame_id - rx->last_completed_frame_id) <= 0) {
    return 0;
  }

  if (rx->ordered) rx_note_gap(rx, hdr.frame_id);
  zc_video_rx_slot_t *s = rx_slot_for(rx, &hdr);
  if (!s) return 0;
  if (s->complete) {
    rx->dup_packets_ignored++;
    return 0;
  }

  /* All datagrams of a frame must agree on its geometry. */
  if (hdr.total_frags != s->total_frags || hdr.tail_len != s->tail_len) {
    return -1;
  }

  s->last_rx_us = rx->now_us ? rx->now_us : 1U;
  if (hdr.frag_idx < s->highest_frag_seen) {
    rx->ooo_packets_seen++;
  } else {
    s->highest_frag_seen = hdr.frag_idx;
  }

  const uint8_t *payload = pkt + sizeof(hdr);

  if (hdr.flags & ZC_VIDEO_FLAG_PARITY) {
    uint16_t parity_len = (s->total_frags > 1U) ? (uint16_t)ZC_FRAG_MTU : s->tail_len;
    if (hdr.frag_idx < hdr.total_frags ||
        hdr.frag_idx - hdr.total_frags >= ZC_MAX_PARITY_SHARDS ||
        hdr.payload_len != parity_len) {
      return -1;
    }
    uint16_t p = (uint16_t)(hdr.frag_idx - hdr.total_frags);
    if (s->parity_present[p]) {
      rx->dup_packets_ignored++;
    } else {
      memcpy(s->parity_buf + (size_t)p * ZC_FRAG_MTU, payload, hdr.payload_len);
      s->parity_lens[p] = hdr.payload_len;
      s->parity_present[p] = 1;
      s->parity_received_count++;
    }
  } else {
    if (hdr.frag_idx >= s->total_frags ||
        hdr.payload_len != expected_frag_len(hdr.frag_idx, s->total_frags,
                                             s->tail_len)) {
      return -1;
    }
    if (s->frag_present[hdr.frag_idx]) {
      rx->dup_packets_ignored++;
    } else {
      size_t dst_off = (size_t)hdr.frag_idx * ZC_FRAG_MTU;
      if (dst_off + hdr.payload_len > rx->frame_cap) return -2;
      memcpy(s->buf + dst_off, payload, hdr.payload_len);
      s->frag_present[hdr.frag_idx] = 1;
      s->frag_lens[hdr.frag_idx] = hdr.payload_len;
      s->received_data_frags++;
    }
  }

  bool fec_used = false;
  if (s->received_data_frags < s->total_frags) {
    uint16_t missing_cnt = (uint16_t)(s->total_frags - s->received_data_frags);
    if (missing_cnt > s->parity_received_count || !rx_recover_cauchy(s, rx->frame_cap)) {
      return 0;
    }
    rx->frames_fec_recovered++;
    fec_used = true;
  }

  if (rx->ordered) {
    s->complete = true;
    s->fec_used = fec_used;
    s->complete_us = rx->now_us;
    return 0; /* handed out by zc_video_rx_next */
  }

  rx->frames_completed++;
  if (out_len) {
    *out_len = (size_t)(s->total_frags - 1U) * ZC_FRAG_MTU + s->tail_len;
  }
  if (out_frame_id) *out_frame_id = s->frame_id;
  if (out_keyframe) *out_keyframe = (s->flags & ZC_VIDEO_FLAG_KEYFRAME) != 0;
  if (out_fec_used) *out_fec_used = fec_used;

  rx->frame_buf = s->buf;
  s->used = false;
  rx->recent_ids[rx->recent_pos] = s->frame_id;
  rx->recent_pos = (rx->recent_pos + 1U) % ZC_RX_RECENT;
  if (rx->recent_count < ZC_RX_RECENT) rx->recent_count++;
  if (rx->last_completed_frame_id == UINT32_MAX ||
      (int32_t)(s->frame_id - rx->last_completed_frame_id) > 0) {
    rx->last_completed_frame_id = s->frame_id;
  }
  if (!rx->allow_out_of_order) {
    /* Older frames can no longer be delivered in order. */
    for (uint32_t i = 0; i < ZC_RX_SLOTS; ++i) {
      zc_video_rx_slot_t *o = &rx->slots[i];
      if (o->used && (int32_t)(o->frame_id - rx->last_completed_frame_id) < 0) {
        rx_release_slot(rx, o);
      }
    }
  }
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
  int rc = zc_video_rx_feed(rx, pkt, pkt_len, out_len, out_frame_id,
                            out_keyframe, out_fec_used);
  *out_data = rx->frame_buf; /* the completed frame's buffer when rc == 1 */
  return rc;
}
