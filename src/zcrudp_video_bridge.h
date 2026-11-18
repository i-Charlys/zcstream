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
  uint16_t magic;       /* 'ZV' = 0x5A56 */
  uint16_t tail_len;    /* Bytes in the last data fragment (1..ZC_FRAG_MTU) */
  uint32_t frame_id;    /* Monotonic frame index */
  uint16_t frag_idx;    /* 0..total_frags-1, or total_frags + parity_id */
  uint16_t total_frags; /* Number of data fragments in this frame */
  uint16_t payload_len; /* Actual bytes in this fragment (<= ZC_FRAG_MTU) */
  uint8_t flags;        /* bit 0 = keyframe, bit 1 = fec_parity */
  uint8_t parity_count; /* Total parity shards generated for this frame */
} zc_video_hdr_t;

/*
 * Every data fragment but the last carries exactly ZC_FRAG_MTU bytes; the
 * last carries tail_len. Repeating tail_len in each header lets the receiver
 * restore the exact frame length when FEC rebuilds the last fragment.
 *
 * FEC: parity 0 is a plain XOR, parities 1..3 are Cauchy rows over GF(2^8).
 * Data fragments ZC_FEC_COLUMN_PERIOD apart share a coefficient column, so
 * two losses exactly that far apart in one frame need retransmission.
 */
#define ZC_VIDEO_MAGIC 0x5A56U
#define ZC_VIDEO_FLAG_KEYFRAME 0x01U
#define ZC_VIDEO_FLAG_PARITY   0x02U
#define ZC_FRAG_MTU            1200U
#define ZC_MAX_FRAGS_PER_FRAME 16384U
#define ZC_MAX_PARITY_SHARDS   4U
#define ZC_FEC_COLUMN_PERIOD   239U

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

/* A frame the transmitter keeps so that lost fragments can be sent again. */
typedef struct {
  bool valid;
  bool keyframe;
  uint8_t fec_shards;
  uint32_t frame_id;
  size_t len;
  size_t cap;
  uint8_t *data;
} zc_tx_history_t;

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
  uint64_t send_errors;   /* OS refused the datagram, or delay queue full */
  uint32_t impair_rng;    /* xorshift32 state of the impairment simulator */
  /* Optional Winsock RIO batch TX state (host-side OS I/O, outside zcrudp) */
  bool rio_ready;
  void *rio_state;
  /* Retransmission history (zc_video_tx_enable_history), a ring by frame id */
  zc_tx_history_t *history;
  uint32_t history_frames;
  uint64_t frags_resent;
  uint64_t resend_misses; /* fragments asked for a frame no longer kept */
} zc_video_tx_t;

/* Frames being reassembled at the same time. Jitter and reordering interleave
 * the datagrams of consecutive frames: with a single slot, the first datagram
 * of a newer frame threw away an older one that was a datagram short. */
#define ZC_RX_SLOTS 32U
#define ZC_RX_RECENT 64U

typedef struct {
  bool used;
  uint32_t frame_id;
  uint16_t total_frags;
  uint16_t tail_len;
  uint16_t received_data_frags;
  uint16_t highest_frag_seen;
  uint8_t flags;
  uint8_t parity_received_count;
  uint16_t parity_lens[ZC_MAX_PARITY_SHARDS];
  uint8_t parity_present[ZC_MAX_PARITY_SHARDS];
  bool complete;         /* ordered mode: whole, waiting for its turn */
  bool fec_used;
  uint64_t first_us;     /* when the slot was opened */
  uint64_t last_rx_us;   /* last datagram of this frame, 0 = none yet */
  uint64_t complete_us;
  uint64_t last_nack_us; /* 0 = never asked for */
  uint16_t *frag_lens;   /* ZC_MAX_FRAGS_PER_FRAME entries */
  uint8_t *frag_present; /* ZC_MAX_FRAGS_PER_FRAME entries */
  uint8_t *buf;          /* frame_cap bytes reserved, buf_committed of them usable */
  size_t buf_committed;
  uint8_t *parity_buf;   /* ZC_MAX_PARITY_SHARDS * ZC_FRAG_MTU */
} zc_video_rx_slot_t;

typedef struct {
  zc_video_rx_slot_t slots[ZC_RX_SLOTS];
  uint32_t last_completed_frame_id;
  /* false (default): frames are delivered in increasing id order only, and
   * completing one gives up on the older ones still in reassembly.
   * true: every frame is delivered when it completes, whatever the order
   * (for callers whose frames are independent). */
  bool allow_out_of_order;
  /* Ordered mode (zc_video_rx_set_ordered): frames are handed out by
   * zc_video_rx_next in id order, a missing one being waited for (and asked
   * for again, zc_video_rx_build_nack) for hold_us before it is given up. */
  bool ordered;
  uint64_t hold_us;
  uint64_t nack_delay_us;
  uint64_t nack_interval_us;
  uint64_t now_us;        /* zc_video_rx_tick */
  uint64_t nacks_built;
  uint32_t recent_ids[ZC_RX_RECENT]; /* frames already delivered */
  uint32_t recent_count;
  uint32_t recent_pos;
  uint8_t *frame_buf;   /* The frame delivered by the last feed that returned 1 */
  size_t frame_cap;
  uint8_t slice_buf[ZC_FRAG_MTU]; /* Last slice from zc_video_rx_feed_slice */
  uint64_t frames_completed;
  uint64_t frames_fec_recovered;
  uint64_t frames_dropped;
  uint64_t dup_packets_ignored;
  uint64_t ooo_packets_seen;
} zc_video_rx_t;

/* The pacer runs this many times faster than the encoder's bitrate (and up
 * to twice that when the delay feedback allows): at exactly the bitrate, a
 * frame larger than average (start, scene cut, burst of motion) waits for
 * the bucket and queues the frames behind it. Pacing only has to keep the
 * link from bursting, as WebRTC's pacer does at 2.5x. */
#define ZC_PACE_HEADROOM 2U
static inline uint32_t zc_pace_bps(uint32_t bitrate_bps, uint32_t factor) {
  uint64_t v = (uint64_t)bitrate_bps * factor;
  return (v > 0xF0000000ULL) ? 0xF0000000U : (uint32_t)v;
}

int zc_video_tx_init(zc_video_tx_t *tx, size_t pool_pages,
                     uint32_t initial_bps, uint32_t min_bps,
                     uint32_t max_bps, uint8_t fec_shards,
                     const zc_net_impair_t *impair, uint64_t now_us);
/**
 * Switches the transmitter to Winsock Registered I/O on a connected UDP socket
 * created with WSA_FLAG_REGISTERED_IO. Once enabled every datagram (data, FEC
 * parity, delayed/impaired packets) goes through RIO: ordinary
 * sendto/WSASendTo calls fail on such a socket, so the two paths cannot be
 * mixed. If this returns non-zero, send on a plain socket instead.
 */
int zc_video_tx_enable_rio(zc_video_tx_t *tx, uintptr_t connected_udp_sock);
void zc_video_tx_destroy(zc_video_tx_t *tx);

int zc_video_tx_send_frame(zc_video_tx_t *tx, uintptr_t udp_sock,
                           const void *dst_addr, int dst_addr_len,
                           uint32_t frame_id, const uint8_t *bitstream,
                           size_t len, bool is_keyframe, uint64_t now_us,
                           bool apply_impair);

/**
 * Flushes delayed/reordered/duplicated packets whose delivery timestamp has
 * arrived. Must be polled whenever any impairment is configured.
 */
int zc_video_tx_flush_net(zc_video_tx_t *tx, uintptr_t udp_sock,
                          const void *dst_addr, int dst_addr_len,
                          uint64_t now_us);

/**
 * Keeps a copy of the last `frames` frames sent (ids are expected to be
 * consecutive) so that zc_video_tx_resend can answer retransmission requests.
 */
int zc_video_tx_enable_history(zc_video_tx_t *tx, uint32_t frames);

/**
 * Sends again data fragments of a frame still in the history: the `count`
 * indices in `frags`, or every data fragment when count is 0. Returns the
 * number of fragments sent, 0 when the frame is no longer kept.
 */
int zc_video_tx_resend(zc_video_tx_t *tx, uintptr_t udp_sock,
                       const void *dst_addr, int dst_addr_len,
                       uint32_t frame_id, const uint16_t *frags, uint16_t count,
                       uint64_t now_us, bool apply_impair);

/**
 * Feeds an OWD + acked-bytes sample back into the SCReAM/L4S pacer.
 */
void zc_video_tx_on_ack(zc_video_tx_t *tx, uint32_t owd_us,
                        uint32_t acked_bytes, uint8_t ce_marked);

/** Retransmission request: "ZCNK", u32 frame id, u16 count, count x u16
 *  fragment indices (count 0 = the whole frame). Little endian. */
#define ZC_NACK_MAGIC "ZCNK"
#define ZC_NACK_HDR_SZ 10U
#define ZC_NACK_MAX_FRAGS 512U

/**
 * Splits a link budget between video and what carrying it costs. Fragment
 * headers, FEC parity and retransmissions all ride on the same link, so a
 * video encoder sized to the full link rate overshoots it as soon as the link
 * loses packets. Call zc_rate_budget_update once per frame sent, with the
 * video bytes of that frame: it returns the rate to give the encoder.
 */
typedef struct {
  uint32_t link_bps;   /* what the link may carry, everything included */
  uint32_t ratio_q16;  /* video bytes per wire byte, running average (Q16) */
  uint64_t wire_mark;  /* tx->bytes_sent at the previous update */
  uint64_t video_acc;  /* video bytes since the last sample */
} zc_rate_budget_t;

void zc_rate_budget_init(zc_rate_budget_t *rb, const zc_video_tx_t *tx, uint32_t link_bps);
uint32_t zc_rate_budget_update(zc_rate_budget_t *rb, const zc_video_tx_t *tx,
                               size_t video_bytes);

int zc_video_rx_init(zc_video_rx_t *rx, size_t max_frame_bytes);

/**
 * Switches the receiver to ordered delivery with retransmission requests.
 * zc_video_rx_feed then only takes datagrams in (it returns 0 or an error);
 * frames come out of zc_video_rx_next, and the caller sends what
 * zc_video_rx_build_nack produces back to the sender.
 *  hold_us           how long a missing or incomplete frame is waited for
 *  nack_delay_us     age at which an incomplete frame is first asked for
 *  nack_interval_us  time between two requests for the same frame
 */
void zc_video_rx_set_ordered(zc_video_rx_t *rx, uint64_t hold_us,
                             uint64_t nack_delay_us, uint64_t nack_interval_us);

/** Current time, for the ordered mode timers. Call before feed/next/build_nack. */
void zc_video_rx_tick(zc_video_rx_t *rx, uint64_t now_us);

/** Ordered mode: next frame in id order, if ready. Returns 1 and fills the
 *  outputs (data in rx->frame_buf, valid until the next call), else 0. */
int zc_video_rx_next(zc_video_rx_t *rx, size_t *out_len, uint32_t *out_frame_id,
                     bool *out_keyframe, bool *out_fec_used);

/** Ordered mode: writes one retransmission request into buf (at least
 *  ZC_NACK_HDR_SZ + 2 * ZC_NACK_MAX_FRAGS bytes) and returns its size, or 0
 *  when nothing is due. Call until it returns 0. */
size_t zc_video_rx_build_nack(zc_video_rx_t *rx, uint8_t *buf, size_t cap);
void zc_video_rx_destroy(zc_video_rx_t *rx);

int zc_video_rx_feed(zc_video_rx_t *rx, const uint8_t *pkt, size_t pkt_len,
                     size_t *out_len, uint32_t *out_frame_id,
                     bool *out_keyframe, bool *out_fec_used);

/**
 * Like zc_video_rx_feed, but delivers single-datagram (total_frags==1, parity==0)
 * independent ternary slices immediately even if reordered within a frame.
 * Such slices bypass frame tracking, so duplicated datagrams are delivered
 * twice: the caller deduplicates if its payload is not idempotent.
 * On return 1, *out_data points at the payload (a slice buffer or frame_buf),
 * valid until the next feed call.
 */
int zc_video_rx_feed_slice(zc_video_rx_t *rx, const uint8_t *pkt, size_t pkt_len,
                           const uint8_t **out_data, size_t *out_len,
                           uint32_t *out_frame_id, bool *out_keyframe,
                           bool *out_fec_used);

/**
 * Routes a ternary video packet via zcrudp_ternary_route_packet:
 *  - ZCRUDP_TERNARY_CHAN_RELIABLE_FEC -> FEC-protected zcrudp TX (INTRA / TILE_MV)
 *  - ZCRUDP_TERNARY_CHAN_UNRELIABLE_RIO -> direct Winsock RIO / unreliable TX (DELTA / DELTA_2X)
 */
int zc_video_tx_send_ternary_packet(zc_video_tx_t *tx, uintptr_t udp_sock,
                                    const void *dst_addr, int dst_addr_len,
                                    uint32_t pkt_seq_id, const uint8_t *pkt,
                                    size_t pkt_len, uint64_t now_us,
                                    bool apply_impair);

#ifdef __cplusplus
}
#endif

#endif /* ZCRUDP_VIDEO_BRIDGE_H */
