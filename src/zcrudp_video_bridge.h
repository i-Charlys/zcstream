#ifndef ZCRUDP_VIDEO_BRIDGE_H
#define ZCRUDP_VIDEO_BRIDGE_H

#include "rudp_io.h"
#include "rudp_nextgen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wire header for each video fragment transported over zcrudp::nextgen.
 * Kept compact (16 bytes) so each UDP datagram stays well within 1500B MTU.
 */
typedef struct {
  uint32_t magic;       /* 'ZCV1' = 0x5A435631 */
  uint32_t frame_id;    /* Monotonic frame index */
  uint16_t frag_idx;    /* 0..total_frags-1, or total_frags + parity_id */
  uint16_t total_frags; /* Number of data fragments in this frame */
  uint16_t payload_len; /* Actual bytes in this fragment (<= ZC_FRAG_MTU) */
  uint8_t flags;        /* bit 0 = keyframe, bit 1 = fec_parity */
  uint8_t parity_count; /* Total parity shards generated for this frame */
} zc_video_hdr_t;

#define ZC_VIDEO_MAGIC 0x5A435631U
#define ZC_VIDEO_FLAG_KEYFRAME 0x01U
#define ZC_VIDEO_FLAG_PARITY   0x02U
#define ZC_FRAG_MTU            1200U
#define ZC_MAX_FRAGS_PER_FRAME 512U
#define ZC_MAX_PARITY_SHARDS   4U

typedef struct {
  int loss_pct;        /* Random independent packet drop % (0..100) */
  int burst_prob_pct;  /* Probability to trigger a Gilbert-Elliott burst */
  int burst_len;       /* Consecutive packets dropped when burst triggers */
  int reorder_pct;     /* Probability to delay a packet out-of-order */
  int dup_pct;         /* Probability to duplicate a packet */
  uint32_t base_owd_us;/* Base one-way network delay in microseconds */
  uint32_t jitter_us;  /* Random jitter added to OWD in microseconds */
} zc_net_impair_t;

typedef struct {
  uint8_t data[sizeof(zc_video_hdr_t) + ZC_FRAG_MTU];
  size_t len;
  uint64_t deliver_at_us;
  bool valid;
} zc_net_queued_pkt_t;

#define ZC_NET_QUEUE_CAP 2048U

typedef struct {
  uint8_t *pool_backing;
  size_t pool_pages;
  rudp_page_pool_s page_pool;
  rudp_pacer_s pacer;
  uint8_t fec_shards; /* 0..ZC_MAX_PARITY_SHARDS */
  zc_net_impair_t impair;
  int burst_remaining;
  zc_net_queued_pkt_t queue[ZC_NET_QUEUE_CAP];
  uint64_t bytes_sent;
  uint64_t packets_sent;
  uint64_t packets_dropped_sim;
  uint64_t packets_reordered_sim;
  uint64_t packets_duplicated_sim;
  uint64_t fec_packets_sent;
  /* Optional Winsock RIO batch TX state (host-side OS I/O, outside zcrudp) */
  bool rio_ready;
  void *rio_state;
} zc_video_tx_t;

typedef struct {
  uint32_t active_frame_id;
  uint32_t last_completed_frame_id;
  uint16_t total_frags;
  uint16_t received_data_frags;
  uint8_t flags;
  uint8_t parity_received_count;
  uint16_t parity_lens[ZC_MAX_PARITY_SHARDS];
  uint8_t parity_present[ZC_MAX_PARITY_SHARDS];
  uint16_t frag_lens[ZC_MAX_FRAGS_PER_FRAME];
  uint8_t frag_present[ZC_MAX_FRAGS_PER_FRAME];
  uint8_t *frame_buf;   /* Caller-owned contiguous output buffer */
  size_t frame_cap;
  uint8_t *parity_buf;  /* ZC_MAX_PARITY_SHARDS * ZC_FRAG_MTU */
  uint64_t frames_completed;
  uint64_t frames_fec_recovered;
  uint64_t frames_dropped;
  uint64_t dup_packets_ignored;
  uint64_t ooo_packets_seen;
  uint16_t highest_frag_seen;
} zc_video_rx_t;

int zc_video_tx_init(zc_video_tx_t *tx, size_t pool_pages,
                     uint32_t initial_bps, uint32_t min_bps,
                     uint32_t max_bps, uint8_t fec_shards,
                     const zc_net_impair_t *impair, uint64_t now_us);
int zc_video_tx_enable_rio(zc_video_tx_t *tx, uintptr_t connected_udp_sock);
void zc_video_tx_destroy(zc_video_tx_t *tx);

int zc_video_tx_send_frame(zc_video_tx_t *tx, uintptr_t udp_sock,
                           const void *dst_addr, int dst_addr_len,
                           uint32_t frame_id, const uint8_t *bitstream,
                           size_t len, bool is_keyframe, uint64_t now_us,
                           bool apply_impair);

/** Flushes delayed/reordered packets whose delivery timestamp has arrived. */
int zc_video_tx_flush_net(zc_video_tx_t *tx, uintptr_t udp_sock,
                          const void *dst_addr, int dst_addr_len,
                          uint64_t now_us);

int zc_video_rx_init(zc_video_rx_t *rx, size_t max_frame_bytes);
void zc_video_rx_destroy(zc_video_rx_t *rx);

int zc_video_rx_feed(zc_video_rx_t *rx, const uint8_t *pkt, size_t pkt_len,
                     size_t *out_len, uint32_t *out_frame_id,
                     bool *out_keyframe, bool *out_fec_used);

#ifdef __cplusplus
}
#endif

#endif /* ZCRUDP_VIDEO_BRIDGE_H */
