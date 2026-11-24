/*
 * gpu_pipeline_portable.c - Portable C11 implementation of gpu_pipeline_win32.h
 *
 * Implements the unified Zcvid transform+CABAC codec on Linux, macOS, Windows,
 * x86_64, and ARM64 using zcvid_gpu_cpu_dec.h and zcvid_entropy.h.
 * Zero OS GPU dependencies (D3D11/DXGI/NVENC); strictly bit-exact closed loop
 * and wire-compatible with the Windows GPU pipeline.
 */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gpu_pipeline_win32.h"
#include "protocol_zcvid.h"
#include "zcvid_entropy.h"
#include "zcvid_gpu_cpu_dec.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#if defined(__linux__) && defined(__has_include)
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/Xutil.h>) && __has_include(<X11/keysym.h>)
#define ZC_PORTABLE_HAVE_X11 1
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#endif
#endif
#endif

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire header flags (shared verbatim with gpu_pipeline_win32.c) */
#define ZCVID_PACKED_FLAG       0x80000000U
#define ZCVID_RAW_RECORDS_FLAG  0x40000000U
#define ZCVID_EC_FLAG           0x20000000U
#define ZCVID_V2_FLAG           0x10000000U
#define ZCVID_WIDE_MV_FLAG      0x08000000U
#define ZCVID_EC_MV_FLAG        0x04000000U
#define ZCVID_TQ_V3_FLAG        0x02000000U
#define ZCVID_RECORD_WORDS_MASK 0x01FFFFFFU
#define ZCVID_NO_RECORD         0xFFFFFFFFU
#define ZCVID_CLASSES_MAX       9U
#define ZCVID_EC_COUNT_BITS     24U
#define ZCVID_EC_MAX_CHUNKS     32U

static FILE *g_log_file = NULL;

uint64_t qpc_now_us(void) {
#ifdef _WIN32
  static LARGE_INTEGER freq = {0};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (uint64_t)(((double)now.QuadPart * 1e6) / (double)freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
#endif
}

static inline uint32_t zcvid_ctz32(uint32_t mask) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctz(mask);
#else
  uint32_t n = 0U;
  while ((mask & 1U) == 0U) {
    mask >>= 1;
    n++;
  }
  return n;
#endif
}

static inline uint32_t zcvid_popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_popcount(v);
#else
  v = v - ((v >> 1) & 0x55555555U);
  v = (v & 0x33333333U) + ((v >> 2) & 0x33333333U);
  return (((v + (v >> 4)) & 0x0F0F0F0FU) * 0x01010101U) >> 24;
#endif
}

static uint8_t *zcvid_put_varint(uint8_t *o, uint32_t v) {
  while (v >= 0x80U) {
    *o++ = (uint8_t)(v | 0x80U);
    v >>= 7;
  }
  *o++ = (uint8_t)v;
  return o;
}

static const uint8_t *zcvid_get_varint(const uint8_t *i, const uint8_t *end, uint32_t *v) {
  uint32_t r = 0U;
  for (unsigned sh = 0U; sh < 35U; sh += 7U) {
    if (i >= end) return NULL;
    uint8_t b = *i++;
    r |= (uint32_t)(b & 0x7FU) << sh;
    if (!(b & 0x80U)) {
      *v = r;
      return i;
    }
  }
  return NULL;
}

static uint32_t zcvid_record_class(uint32_t w0, uint32_t *id) {
  uint32_t tag = w0 >> 30;
  uint32_t bit29 = (w0 >> 29) & 1U;
  if (tag == 3U) {
    if (bit29) {
      if ((w0 >> 28) & 1U) {
        *id = w0 & 0x0FFFFFFFU;
        return 8U;
      }
      *id = w0 & 0x3FFFFFU;
      return 7U;
    }
    *id = w0 & 0x1FFFFFFFU;
    return 6U;
  }
  *id = w0 & 0x1FFFFFFFU;
  return tag * 2U + bit29;
}

static size_t zcvid_pack_bound(uint32_t num_records) {
  return (size_t)ZCVID_CLASSES_MAX * 5U + (size_t)num_records * 10U;
}

static inline uint32_t zcvid_tq_id_to_sb_key(uint32_t id, uint32_t bw, uint32_t sbw,
                                            uint64_t bw_recip) {
  uint32_t b = (uint32_t)(((uint64_t)id * 0xAAAAAAABULL) >> 37);
  uint32_t rem = id - b * 48U;
  uint32_t by = (uint32_t)(((uint64_t)b * bw_recip) >> 32);
  if (b - by * bw >= bw) by++;
  uint32_t bx = b - by * bw;
  uint32_t sb = (by >> 1) * sbw + (bx >> 1);
  uint32_t quad = ((by & 1U) << 1) | (bx & 1U);
  return ((sb << 2) | quad) * 48U + rem;
}

static void zcvid_radix_sort_256(uint64_t *a, uint64_t *b, uint32_t n) {
  if (n <= 1U) return;
  if (n <= 64U) {
    for (uint32_t i = 1U; i < n; ++i) {
      uint64_t kv = a[i];
      uint32_t j = i;
      while (j > 0U && (a[j - 1U] >> 32) > (kv >> 32)) {
        a[j] = a[j - 1U];
        j--;
      }
      a[j] = kv;
    }
    return;
  }
  uint32_t hist[4][256];
  memset(hist, 0, sizeof(hist));
  uint32_t kor = 0U, kand = 0xFFFFFFFFU;
  for (uint32_t i = 0U; i < n; ++i) {
    uint32_t k = (uint32_t)(a[i] >> 32);
    kor |= k;
    kand &= k;
    hist[0][k & 0xFFU]++;
    hist[1][(k >> 8) & 0xFFU]++;
    hist[2][(k >> 16) & 0xFFU]++;
    hist[3][k >> 24]++;
  }
  uint32_t diff = kor ^ kand;
  if (diff == 0U) return;
  uint64_t *src = a, *dst = b;
  for (unsigned pass = 0U; pass < 4U; ++pass) {
    if (((diff >> (pass * 8U)) & 0xFFU) == 0U) continue;
    uint32_t *h = hist[pass];
    uint32_t sum = 0U;
    for (unsigned j = 0U; j < 256U; ++j) {
      uint32_t c = h[j];
      h[j] = sum;
      sum += c;
    }
    unsigned shift = 32U + pass * 8U;
    for (uint32_t i = 0U; i < n; ++i) dst[h[(src[i] >> shift) & 0xFFU]++] = src[i];
    uint64_t *t = src;
    src = dst;
    dst = t;
  }
  if (src != a) memcpy(a, src, (size_t)n * sizeof(uint64_t));
}

/* ---------------------------------------------------------------------------
 * Range-coded intra / trit / class-8 CABAC helpers (wire-compatible)
 * ------------------------------------------------------------------------- */
typedef struct {
  zcrudp_iec_model_t luma;
  zcrudp_iec_model_t chroma;
  uint8_t *line;
  uint32_t *lrow;
  uint32_t row_base;
  uint32_t width;
  uint64_t width_recip;
  uint32_t last_idx;
  uint8_t last_up[3];
  bool last_up_ok;
  uint32_t act_y;
  uint32_t act_c;
} zcvid_ec_intra_t;

static void zcvid_ec_intra_reset(zcvid_ec_intra_t *s, uint8_t *line, uint32_t *lrow,
                                uint32_t *gen_io, uint32_t width, uint32_t height) {
  zcrudp_iec_model_reset(&s->luma);
  zcrudp_iec_model_reset(&s->chroma);
  s->line = line;
  s->lrow = lrow;
  uint32_t step = height + 2U;
  if (*gen_io == 0U || *gen_io > UINT32_MAX - step - 2U) {
    memset(lrow, 0, (size_t)width * sizeof(uint32_t));
    *gen_io = 1U;
  } else {
    *gen_io += step;
  }
  s->row_base = *gen_io;
  s->width = width;
  s->width_recip = (width > 0U) ? ((1ULL << 32) / (uint64_t)width) : 0ULL;
  s->last_idx = UINT32_MAX;
  s->last_up_ok = false;
  s->act_y = 0U;
  s->act_c = 0U;
}

static inline void zcvid_ec_idx_to_xy(const zcvid_ec_intra_t *s, uint32_t idx,
                                     uint32_t *out_row, uint32_t *out_x) {
  uint32_t w = s->width;
  uint32_t row = (uint32_t)(((uint64_t)idx * s->width_recip) >> 32);
  uint32_t rem = idx - row * w;
  if (rem >= w) {
    row++;
    rem -= w;
  }
  *out_row = row;
  *out_x = rem;
}

typedef struct {
  bool has_a, has_b, has_c;
  uint8_t a[3], b[3], c[3];
} zcvid_ec_nb_t;

static inline void zcvid_ec_neighbours_xy(const zcvid_ec_intra_t *s, uint32_t idx,
                                         uint32_t row, uint32_t x, zcvid_ec_nb_t *nb) {
  uint32_t cur_tag = s->row_base + row;
  nb->has_a = (x > 0U) && s->lrow[x - 1U] == cur_tag;
  nb->has_b = (row > 0U) && s->lrow[x] == cur_tag - 1U;
  nb->has_c = nb->has_a && s->last_up_ok && s->last_idx + 1U == idx;
  if (nb->has_a) {
    const uint8_t *pa = s->line + (size_t)(x - 1U) * 3U;
    nb->a[0] = pa[0]; nb->a[1] = pa[1]; nb->a[2] = pa[2];
  }
  if (nb->has_b) {
    const uint8_t *pb = s->line + (size_t)x * 3U;
    nb->b[0] = pb[0]; nb->b[1] = pb[1]; nb->b[2] = pb[2];
  }
  if (nb->has_c) {
    nb->c[0] = s->last_up[0]; nb->c[1] = s->last_up[1]; nb->c[2] = s->last_up[2];
  }
}

static inline uint32_t zcvid_ec_predict(const zcvid_ec_nb_t *nb, unsigned ch, uint32_t *grad) {
  if (nb->has_a && nb->has_b && nb->has_c) {
    int a = nb->a[ch], b = nb->b[ch], c = nb->c[ch];
    *grad = (uint32_t)(abs(a - c) + abs(b - c));
    return zcrudp_iec_med((uint8_t)a, (uint8_t)b, (uint8_t)c);
  }
  *grad = UINT32_MAX;
  if (nb->has_a) return nb->a[ch];
  if (nb->has_b) return nb->b[ch];
  return 128U;
}

static inline void zcvid_ec_store_xy(zcvid_ec_intra_t *s, uint32_t idx, uint32_t row, uint32_t x,
                                    uint8_t y, uint8_t cb, uint8_t cr) {
  uint8_t *l = s->line + (size_t)x * 3U;
  uint32_t cur_tag = s->row_base + row;
  s->last_up_ok = (row > 0U) && s->lrow[x] == cur_tag - 1U;
  s->last_up[0] = l[0]; s->last_up[1] = l[1]; s->last_up[2] = l[2];
  s->last_idx = idx;
  l[0] = y;
  l[1] = cb;
  l[2] = cr;
  s->lrow[x] = cur_tag;
}

static inline void zcvid_ec_put_sample(zcrudp_rc_enc_t *rc, zcrudp_iec_model_t *m, uint32_t *act,
                                      uint32_t pred8, uint32_t grad, unsigned shift, uint32_t v) {
  uint32_t a = (grad != UINT32_MAX) ? (grad >> shift) : (*act * 2U);
  int32_t res = (int32_t)v - (int32_t)(pred8 >> shift);
  zcrudp_iec_enc_residual(rc, m, zcrudp_iec_bucket(a), 0U, res);
  *act = (uint32_t)abs(res);
}

static inline uint32_t zcvid_ec_get_sample(zcrudp_rc_dec_t *rc, zcrudp_iec_model_t *m, uint32_t *act,
                                          uint32_t pred8, uint32_t grad, unsigned shift) {
  uint32_t a = (grad != UINT32_MAX) ? (grad >> shift) : (*act * 2U);
  int32_t res = zcrudp_iec_dec_residual(rc, m, zcrudp_iec_bucket(a), 0U);
  *act = (uint32_t)abs(res);
  return (uint32_t)((int32_t)(pred8 >> shift) + res) & (0xFFU >> shift);
}

static inline uint8_t zcvid_ec_expand6(uint32_t q6) { return (uint8_t)((q6 << 2) | (q6 >> 4)); }

static size_t zcvid_ec_delta_stride(uint32_t width, uint32_t cls) {
  if ((cls & 1U) == 0U) return width;
  return (width & 1U) ? 0U : width / 2U;
}

static uint32_t zcvid_ec_id_limit(uint32_t num_px, uint32_t cls) {
  if (cls < 6U) return num_px / 10U + 1U;
  return (cls == 6U) ? num_px : (num_px + 1U) / 2U;
}

static inline uint32_t zcvid_ec_trit_ctx(const zcrudp_tec_model_t *m, size_t idx, uint32_t *sctx) {
  const uint8_t *hist = m->hist;
  const size_t stride = m->stride;
  uint32_t l1 = hist[(idx - 1U) & ZCRUDP_TEC_HIST_MASK];
  uint32_t l2 = hist[(idx - 2U) & ZCRUDP_TEC_HIST_MASK];
  uint32_t u = 0U, ul = 0U, ur = 0U;
  if (stride != 0U) {
    u = hist[(idx - stride) & ZCRUDP_TEC_HIST_MASK];
    ul = hist[(idx - stride - 1U) & ZCRUDP_TEC_HIST_MASK];
    ur = hist[(idx - stride + 1U) & ZCRUDP_TEC_HIST_MASK];
  }
  *sctx = l1 * 3U + (u ? u : (ul ? ul : ur));
  return (uint32_t)(l1 != 0U) | ((uint32_t)(l2 != 0U) << 1) | ((uint32_t)(u != 0U) << 2) |
         (((uint32_t)(ul != 0U) + (uint32_t)(ur != 0U)) << 3);
}

static inline void zcvid_ec_put_trits(zcrudp_rc_enc_t *rc, zcrudp_tec_model_t *m, size_t base,
                                     uint32_t w1) {
  zcrudp_trit_t trits[10];
  zcrudp_unpack_10trits((uint16_t)w1, trits);
  for (size_t j = 0U; j < 10U; ++j) {
    size_t idx = base + j;
    int t = trits[j];
    uint32_t sctx = 0U;
    uint32_t zctx = zcvid_ec_trit_ctx(m, idx, &sctx);
    uint8_t h = 0U;
    zcrudp_rc_enc_bit(rc, &m->zero[zctx], (t != 0) ? 1U : 0U);
    if (t != 0) {
      zcrudp_rc_enc_bit(rc, &m->sign[sctx], (t < 0) ? 1U : 0U);
      h = (t < 0) ? 2U : 1U;
    }
    m->hist[idx & ZCRUDP_TEC_HIST_MASK] = h;
  }
}

static inline uint32_t zcvid_ec_get_trits(zcrudp_rc_dec_t *rc, zcrudp_tec_model_t *m, size_t base) {
  static const uint32_t pow3[10] = {1U, 3U, 9U, 27U, 81U, 243U, 729U, 2187U, 6561U, 19683U};
  uint32_t w1 = 0U;
  for (size_t j = 0U; j < 10U; ++j) {
    size_t idx = base + j;
    uint32_t sctx = 0U;
    uint32_t zctx = zcvid_ec_trit_ctx(m, idx, &sctx);
    uint32_t code = 1U;
    uint8_t h = 0U;
    if (zcrudp_rc_dec_bit(rc, &m->zero[zctx]) != 0U) {
      bool neg = zcrudp_rc_dec_bit(rc, &m->sign[sctx]) != 0U;
      code = neg ? 0U : 2U;
      h = neg ? 2U : 1U;
    }
    m->hist[idx & ZCRUDP_TEC_HIST_MASK] = h;
    w1 += code * pow3[j];
  }
  return w1;
}

/* ---------------------------------------------------------------------------
 * CABAC v3 class-8 Transform Entropy Coder
 * ------------------------------------------------------------------------- */
typedef struct {
  zcrudp_rc_prob_t is_8x8;
  zcrudp_rc_prob_t qmask[9];
  zcrudp_rc_prob_t pred[2];
  zcrudp_rc_prob_t present[4];
  zcrudp_rc_prob_t qp_same[2];
  zcrudp_rc_prob_t qp_is_alt[2];
  zcrudp_rc_prob_t has_ac[2][2];
  zcrudp_rc_prob_t last_k[2][2][14];
  zcrudp_rc_prob_t sig[2][2][15];
  zcrudp_rc_prob_t intra_dc_nz[2];
  zcrudp_rc_prob_t dc_sign[2][2];
  zcrudp_rc_prob_t gt1[2][2][4];
  zcrudp_rc_prob_t gt2[2][2][4];
  zcrudp_rc_prob_t mag_rem[2][4][7];
} zcvid_tq_model_t;

typedef struct {
  uint32_t last_qp[2];
  uint32_t alt_qp[2];
  int32_t prev_intra_dc[3];
  uint32_t prev_intra_qp[3];
} zcvid_tq_state_t;

static void zcvid_tq_state_reset(zcvid_tq_state_t *st) {
  st->last_qp[0] = st->last_qp[1] = 64U;
  st->alt_qp[0]  = st->alt_qp[1]  = 64U;
  st->prev_intra_dc[0] = st->prev_intra_dc[1] = st->prev_intra_dc[2] = 0;
  st->prev_intra_qp[0] = st->prev_intra_qp[1] = st->prev_intra_qp[2] = 64U;
}

static zcvid_tq_model_t g_tq_model_tpl;
static atomic_int g_tq_model_tpl_state = 0;

static void zcvid_tq_model_reset(zcvid_tq_model_t *m) {
  if (atomic_load_explicit(&g_tq_model_tpl_state, memory_order_acquire) != 2) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_tq_model_tpl_state, &expected, 1)) {
      zcvid_tq_model_t *t = &g_tq_model_tpl;
      zcrudp_rc_prob_init(&t->is_8x8, 22000U);
      for (int i = 0; i < 9; ++i) zcrudp_rc_prob_init(&t->qmask[i], 16384U);
      zcrudp_rc_prob_init(&t->qmask[6], 26000U);
      zcrudp_rc_prob_init(&t->qmask[8], 8000U);
      zcrudp_rc_prob_init(&t->pred[0], 6000U);
      zcrudp_rc_prob_init(&t->pred[1], 8000U);
      zcrudp_rc_prob_init(&t->present[0], 2500U);
      zcrudp_rc_prob_init(&t->present[1], 24000U);
      zcrudp_rc_prob_init(&t->present[2], 26000U);
      zcrudp_rc_prob_init(&t->present[3], 12000U);
      for (int i = 0; i < 2; ++i) {
        zcrudp_rc_prob_init(&t->qp_same[i], 30000U);
        zcrudp_rc_prob_init(&t->qp_is_alt[i], 16384U);
        zcrudp_rc_prob_init(&t->intra_dc_nz[i], 16384U);
        for (int j = 0; j < 2; ++j) {
          zcrudp_rc_prob_init(&t->has_ac[i][j], (j == 0) ? 12000U : 22000U);
          zcrudp_rc_prob_init(&t->dc_sign[i][j], 16384U);
          for (int c = 0; c < 14; ++c) zcrudp_rc_prob_init(&t->last_k[i][j][c], 16384U);
          for (int k = 0; k < 15; ++k) {
            uint16_t p0 = (k == 0) ? 9000U : (k < 4) ? 14000U : 20000U;
            zcrudp_rc_prob_init(&t->sig[i][j][k], p0);
          }
          for (int kb = 0; kb < 4; ++kb) {
            uint16_t p_gt1 = (kb == 0) ? ((i == 0) ? 20000U : 18000U) : (kb == 1) ? 23000U : 27000U;
            uint16_t p_gt2 = (kb == 0) ? ((i == 0) ? 22000U : 20000U) : 25000U;
            zcrudp_rc_prob_init(&t->gt1[i][j][kb], p_gt1);
            zcrudp_rc_prob_init(&t->gt2[i][j][kb], p_gt2);
          }
        }
        for (int kb = 0; kb < 4; ++kb) {
          for (int c = 0; c < 7; ++c) {
            int32_t p0 = 14000 + kb * 3000 + c * 2000;
            if (p0 > 28000) p0 = 28000;
            zcrudp_rc_prob_init(&t->mag_rem[i][kb][c], (uint16_t)p0);
          }
        }
      }
      atomic_store_explicit(&g_tq_model_tpl_state, 2, memory_order_release);
    } else {
      while (atomic_load_explicit(&g_tq_model_tpl_state, memory_order_acquire) != 2) {
      }
    }
  }
  memcpy(m, &g_tq_model_tpl, sizeof(*m));
}

static inline uint32_t zcvid_tq_kb(uint32_t k) {
  static const uint8_t lut[16] = {0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3};
  return lut[k & 15U];
}

static inline bool zcvid_tq_put_block(zcrudp_rc_enc_t *rc,
                                     zcvid_tq_model_t *m,
                                     zcvid_tq_state_t *st,
                                     const uint64_t *keys,
                                     uint32_t i, uint32_t e,
                                     uint32_t blk_base, uint32_t is_8x8) {
  uint32_t pred = ((uint32_t)keys[i] >> 24) & 0xFFU;
  if (pred > 3U || ((pred >> 1) & 1U) != is_8x8) return false;
  uint32_t is_inter = pred & 1U;
  zcrudp_rc_enc_t lrc = *rc;
  zcrudp_rc_enc_bit(&lrc, &m->pred[is_8x8], is_inter);

  uint32_t p_beg[3], p_end[3];
  uint32_t j = i;
  for (uint32_t pl = 0U; pl < 3U; ++pl) {
    uint32_t pl_hi = blk_base + (pl + 1U) * 16U;
    p_beg[pl] = j;
    while (j < e && (((uint32_t)(keys[j] >> 32) & 0x0FFFFFFFU) < pl_hi)) j++;
    p_end[pl] = j;
  }
  if (j != e) {
    *rc = lrc;
    return false;
  }

  uint32_t py = (p_end[0] > p_beg[0]) ? 1U : 0U;
  uint32_t pb = (p_end[1] > p_beg[1]) ? 1U : 0U;
  uint32_t pr = (p_end[2] > p_beg[2]) ? 1U : 0U;
  if ((py | pb | pr) == 0U) {
    *rc = lrc;
    return false;
  }
  zcrudp_rc_enc_bit(&lrc, &m->present[0], py);
  zcrudp_rc_enc_bit(&lrc, &m->present[1], pb);
  if ((py | pb) != 0U) zcrudp_rc_enc_bit(&lrc, &m->present[2U + pb], pr);

  for (uint32_t pl = 0U; pl < 3U; ++pl) {
    if (p_end[pl] == p_beg[pl]) continue;
    uint32_t chroma = (pl > 0U) ? 1U : 0U;
    uint32_t qp = ((uint32_t)keys[p_beg[pl]] >> 16) & 0xFFU;
    if (qp > 51U) {
      *rc = lrc;
      return false;
    }
    if (qp == st->last_qp[chroma]) {
      zcrudp_rc_enc_bit(&lrc, &m->qp_same[chroma], 0U);
    } else {
      zcrudp_rc_enc_bit(&lrc, &m->qp_same[chroma], 1U);
      if (qp == st->alt_qp[chroma]) {
        zcrudp_rc_enc_bit(&lrc, &m->qp_is_alt[chroma], 1U);
        st->alt_qp[chroma] = st->last_qp[chroma];
        st->last_qp[chroma] = qp;
      } else {
        zcrudp_rc_enc_bit(&lrc, &m->qp_is_alt[chroma], 0U);
        zcrudp_rc_enc_direct(&lrc, qp, 6U);
        st->alt_qp[chroma] = st->last_qp[chroma];
        st->last_qp[chroma] = qp;
      }
    }

    int16_t lv[16];
    memset(lv, 0, sizeof(lv));
    uint32_t sig_mask = 0U;
    int32_t prev_k = -1;
    uint32_t last_k = 0U;
    for (uint32_t t = p_beg[pl]; t < p_end[pl]; ++t) {
      uint64_t kv = keys[t];
      uint32_t w1 = (uint32_t)kv;
      uint32_t id = (uint32_t)(kv >> 32) & 0x0FFFFFFFU;
      uint32_t k = id & 15U;
      int32_t level = (int32_t)(int16_t)(w1 & 0xFFFFU);
      if (((w1 >> 16) & 0xFFU) != qp || ((w1 >> 24) & 0xFFU) != pred) {
        *rc = lrc;
        return false;
      }
      if ((int32_t)k <= prev_k || level < -255 || level > 255) {
        *rc = lrc;
        return false;
      }
      if ((k > 0U || is_inter != 0U) && level == 0) {
        *rc = lrc;
        return false;
      }
      lv[k] = (int16_t)level;
      sig_mask |= (1U << k);
      last_k = k;
      prev_k = (int32_t)k;
    }

    uint32_t has_ac = (last_k > 0U) ? 1U : 0U;
    zcrudp_rc_enc_bit(&lrc, &m->has_ac[is_inter][chroma], has_ac);
    if (has_ac) {
      uint32_t v = last_k - 1U;
      zcrudp_rc_prob_t *lk = m->last_k[is_inter][chroma];
      uint32_t b3 = (v >> 3) & 1U; zcrudp_rc_enc_bit(&lrc, &lk[0], b3);
      uint32_t n2 = 2U | b3;
      uint32_t b2 = (v >> 2) & 1U; zcrudp_rc_enc_bit(&lrc, &lk[n2 - 1U], b2);
      uint32_t n1 = (n2 << 1) | b2;
      uint32_t b1 = (v >> 1) & 1U; zcrudp_rc_enc_bit(&lrc, &lk[n1 - 1U], b1);
      uint32_t n0 = (n1 << 1) | b1;
      if (n0 != 15U) zcrudp_rc_enc_bit(&lrc, &lk[n0 - 1U], v & 1U);
      zcrudp_rc_prob_t *sp = m->sig[is_inter][chroma];
      for (uint32_t k = 0U; k < last_k; ++k) {
        zcrudp_rc_enc_bit(&lrc, &sp[k], (sig_mask >> k) & 1U);
      }
    }

    if (is_inter == 0U && (sig_mask & 1U) == 0U) {
      st->prev_intra_dc[pl] = 0;
      st->prev_intra_qp[pl] = qp;
    }

    uint32_t sm = sig_mask;
    while (sm != 0U) {
      uint32_t k = zcvid_ctz32(sm);
      sm &= sm - 1U;
      int32_t val = lv[k];
      uint32_t egk_max = 7U;
      if (k == 0U && is_inter == 0U) {
        int32_t pred_dc = (qp == st->prev_intra_qp[pl]) ? st->prev_intra_dc[pl] : 0;
        st->prev_intra_dc[pl] = val;
        st->prev_intra_qp[pl] = qp;
        val -= pred_dc;
        uint32_t nz = (val != 0) ? 1U : 0U;
        zcrudp_rc_enc_bit(&lrc, &m->intra_dc_nz[chroma], nz);
        if (!nz) continue;
        egk_max = 8U;
      }
      uint32_t neg = (val < 0) ? 1U : 0U;
      if (k == 0U) {
        zcrudp_rc_enc_bit(&lrc, &m->dc_sign[is_inter][chroma], neg);
      } else {
        zcrudp_rc_enc_bit_bypass(&lrc, neg);
      }
      uint32_t mag = (uint32_t)(neg ? -val : val);
      uint32_t kb = zcvid_tq_kb(k);
      uint32_t gt1 = (mag > 1U) ? 1U : 0U;
      zcrudp_rc_enc_bit(&lrc, &m->gt1[is_inter][chroma][kb], gt1);
      if (gt1) {
        uint32_t gt2 = (mag > 2U) ? 1U : 0U;
        zcrudp_rc_enc_bit(&lrc, &m->gt2[is_inter][chroma][kb], gt2);
        if (gt2) {
          zcrudp_rc_enc_egk(&lrc, m->mag_rem[chroma][kb], 7U, egk_max, (uint64_t)(mag - 3U));
        }
      }
    }
  }
  *rc = lrc;
  return !lrc.overflow;
}

static bool zcvid_tq_put(zcrudp_rc_enc_t *rc, zcrudp_tec_model_t *tm,
                        const uint64_t *keys, uint32_t num, uint32_t bw, uint32_t bh) {
  zcvid_tq_model_t m;
  zcvid_tq_model_reset(&m);
  zcvid_tq_state_t st;
  zcvid_tq_state_reset(&st);
  const uint32_t sbw = (bw + 1U) >> 1;
  const uint32_t sbh = (bh + 1U) >> 1;
  const uint32_t nsb = sbw * sbh;
  uint32_t prev_sb = UINT32_MAX;
  uint32_t i = 0U;
  while (i < num) {
    uint32_t key0 = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
    uint32_t sb = (uint32_t)(((uint64_t)key0 * 0xAAAAAAABULL) >> 39);
    if (sb >= nsb || sb >= (1U << 20) || (prev_sb != UINT32_MAX && sb <= prev_sb)) return false;
    if (prev_sb == UINT32_MAX) {
      zcrudp_rc_enc_direct(rc, sb, 20U);
    } else {
      zcrudp_tec_enc_run(rc, tm, (uint64_t)(sb - prev_sb - 1U));
    }
    prev_sb = sb;

    uint32_t sb_lo = sb * 192U;
    uint32_t sb_hi = sb_lo + 192U;
    uint32_t se = i + 1U;
    while (se < num && (((uint32_t)(keys[se] >> 32) & 0x0FFFFFFFU) < sb_hi)) se++;

    uint32_t pred0 = ((uint32_t)keys[i] >> 24) & 0xFFU;
    if (pred0 > 3U) return false;
    uint32_t is_8x8 = (pred0 >> 1) & 1U;
    zcrudp_rc_enc_bit(rc, &m.is_8x8, is_8x8);

    if (is_8x8) {
      uint32_t q0_last = ((uint32_t)(keys[se - 1U] >> 32) & 0x0FFFFFFFU) - sb_lo;
      if (q0_last >= 48U) return false;
      if (!zcvid_tq_put_block(rc, &m, &st, keys, i, se, sb_lo, 1U)) return false;
    } else {
      uint32_t q_beg[4], q_end[4];
      uint32_t j = i;
      for (uint32_t q = 0U; q < 4U; ++q) {
        uint32_t q_hi = sb_lo + (q + 1U) * 48U;
        q_beg[q] = j;
        while (j < se && (((uint32_t)(keys[j] >> 32) & 0x0FFFFFFFU) < q_hi)) j++;
        q_end[q] = j;
      }
      if (j != se) return false;
      uint32_t b0 = (q_end[0] > q_beg[0]) ? 1U : 0U;
      uint32_t b1 = (q_end[1] > q_beg[1]) ? 1U : 0U;
      uint32_t b2 = (q_end[2] > q_beg[2]) ? 1U : 0U;
      uint32_t b3 = (q_end[3] > q_beg[3]) ? 1U : 0U;
      if ((b0 | b1 | b2 | b3) == 0U) return false;
      zcrudp_rc_enc_bit(rc, &m.qmask[0], b0);
      zcrudp_rc_enc_bit(rc, &m.qmask[1U + b0], b1);
      zcrudp_rc_enc_bit(rc, &m.qmask[3U + b0 + b1], b2);
      if ((b0 | b1 | b2) != 0U) zcrudp_rc_enc_bit(rc, &m.qmask[5U + b0 + b1 + b2], b3);

      for (uint32_t q = 0U; q < 4U; ++q) {
        if (q_end[q] == q_beg[q]) continue;
        if (!zcvid_tq_put_block(rc, &m, &st, keys, q_beg[q], q_end[q], sb_lo + q * 48U, 0U)) {
          return false;
        }
      }
    }
    i = se;
  }
  return true;
}

static inline bool zcvid_tq_get_block(zcrudp_rc_dec_t *rc,
                                     zcvid_tq_model_t *m,
                                     zcvid_tq_state_t *st,
                                     uint32_t b, uint32_t is_8x8, uint32_t count,
                                     uint32_t *got_io,
                                     uint32_t *out) {
  zcrudp_rc_dec_t lrc = *rc;
  uint32_t is_inter = zcrudp_rc_dec_bit(&lrc, &m->pred[is_8x8]);
  uint32_t pred = is_inter | (is_8x8 << 1);
  uint32_t py = zcrudp_rc_dec_bit(&lrc, &m->present[0]);
  uint32_t pb = zcrudp_rc_dec_bit(&lrc, &m->present[1]);
  uint32_t pr = ((py | pb) != 0U) ? zcrudp_rc_dec_bit(&lrc, &m->present[2U + pb]) : 1U;
  uint32_t pmask = py | (pb << 1) | (pr << 2);
  uint32_t got = *got_io;

  for (uint32_t pl = 0U; pl < 3U; ++pl) {
    if (((pmask >> pl) & 1U) == 0U) continue;
    uint32_t chroma = (pl > 0U) ? 1U : 0U;
    if (zcrudp_rc_dec_bit(&lrc, &m->qp_same[chroma]) != 0U) {
      if (zcrudp_rc_dec_bit(&lrc, &m->qp_is_alt[chroma]) != 0U) {
        uint32_t tmp = st->last_qp[chroma];
        st->last_qp[chroma] = st->alt_qp[chroma];
        st->alt_qp[chroma] = tmp;
      } else {
        uint32_t nqp = (uint32_t)zcrudp_rc_dec_direct(&lrc, 6U);
        st->alt_qp[chroma] = st->last_qp[chroma];
        st->last_qp[chroma] = nqp;
      }
    }
    uint32_t qp = st->last_qp[chroma];
    if (qp > 51U) {
      *rc = lrc;
      return false;
    }

    uint32_t sig_mask = 1U;
    if (zcrudp_rc_dec_bit(&lrc, &m->has_ac[is_inter][chroma]) != 0U) {
      zcrudp_rc_prob_t *lk = m->last_k[is_inter][chroma];
      uint32_t node = 2U | zcrudp_rc_dec_bit(&lrc, &lk[0]);
      node = (node << 1) | zcrudp_rc_dec_bit(&lrc, &lk[node - 1U]);
      node = (node << 1) | zcrudp_rc_dec_bit(&lrc, &lk[node - 1U]);
      node = (node == 15U) ? 30U : ((node << 1) | zcrudp_rc_dec_bit(&lrc, &lk[node - 1U]));
      uint32_t last_k = node - 15U;
      sig_mask = 1U << last_k;
      zcrudp_rc_prob_t *sp = m->sig[is_inter][chroma];
      for (uint32_t k = 0U; k < last_k; ++k) {
        sig_mask |= zcrudp_rc_dec_bit(&lrc, &sp[k]) << k;
      }
    }
    if (zcvid_popcount32(sig_mask) > count - got) {
      *rc = lrc;
      return false;
    }

    if (is_inter == 0U && (sig_mask & 1U) == 0U) {
      st->prev_intra_dc[pl] = 0;
      st->prev_intra_qp[pl] = qp;
    }

    uint32_t w0_base = 0xF0000000U | (b * 48U + pl * 16U);
    uint32_t w1_hi = (qp << 16) | (pred << 24);
    uint32_t sm = sig_mask;
    while (sm != 0U) {
      uint32_t k = zcvid_ctz32(sm);
      sm &= sm - 1U;
      int32_t level = 0;
      if (k == 0U && is_inter == 0U) {
        int32_t pred_dc = (qp == st->prev_intra_qp[pl]) ? st->prev_intra_dc[pl] : 0;
        int32_t val = 0;
        if (zcrudp_rc_dec_bit(&lrc, &m->intra_dc_nz[chroma]) != 0U) {
          uint32_t neg = zcrudp_rc_dec_bit(&lrc, &m->dc_sign[0][chroma]);
          int32_t mag = 1;
          if (zcrudp_rc_dec_bit(&lrc, &m->gt1[0][chroma][0]) != 0U) {
            mag = 2;
            if (zcrudp_rc_dec_bit(&lrc, &m->gt2[0][chroma][0]) != 0U) {
              mag = (int32_t)zcrudp_rc_dec_egk(&lrc, m->mag_rem[chroma][0], 7U, 8U) + 3;
              if (mag > 510) {
                *rc = lrc;
                return false;
              }
            }
          }
          val = neg ? -mag : mag;
        }
        level = pred_dc + val;
        if (level < -255 || level > 255) {
          *rc = lrc;
          return false;
        }
        st->prev_intra_dc[pl] = level;
        st->prev_intra_qp[pl] = qp;
      } else {
        uint32_t neg = (k == 0U)
                           ? zcrudp_rc_dec_bit(&lrc, &m->dc_sign[is_inter][chroma])
                           : zcrudp_rc_dec_bit_bypass(&lrc);
        uint32_t kb = zcvid_tq_kb(k);
        int32_t mag = 1;
        if (zcrudp_rc_dec_bit(&lrc, &m->gt1[is_inter][chroma][kb]) != 0U) {
          mag = 2;
          if (zcrudp_rc_dec_bit(&lrc, &m->gt2[is_inter][chroma][kb]) != 0U) {
            mag = (int32_t)zcrudp_rc_dec_egk(&lrc, m->mag_rem[chroma][kb], 7U, 7U) + 3;
            if (mag > 255) {
              *rc = lrc;
              return false;
            }
          }
        }
        level = neg ? -mag : mag;
      }
      out[got * 2U] = w0_base + k;
      out[got * 2U + 1U] = ((uint32_t)level & 0xFFFFU) | w1_hi;
      got++;
    }
  }
  *rc = lrc;
  *got_io = got;
  return true;
}

static bool zcvid_tq_get(zcrudp_rc_dec_t *rc, zcrudp_tec_model_t *tm,
                        uint32_t count, uint32_t *out, uint32_t bw, uint32_t bh) {
  zcvid_tq_model_t m;
  zcvid_tq_model_reset(&m);
  zcvid_tq_state_t st;
  zcvid_tq_state_reset(&st);
  const uint32_t sbw = (bw + 1U) >> 1;
  const uint32_t sbh = (bh + 1U) >> 1;
  const uint32_t nsb = sbw * sbh;
  uint32_t prev_sb = UINT32_MAX;
  uint32_t sbx = 0U, sby = 0U;
  uint32_t got = 0U;
  zcrudp_rc_dec_t lrc = *rc;
  while (got < count) {
    uint32_t sb;
    if (prev_sb == UINT32_MAX) {
      sb = (uint32_t)zcrudp_rc_dec_direct(&lrc, 20U);
      if (sb >= nsb) {
        *rc = lrc;
        return false;
      }
      sby = sb / sbw;
      sbx = sb - sby * sbw;
    } else {
      uint64_t run = zcrudp_tec_dec_run(&lrc, tm);
      if (run >= nsb || prev_sb + 1U + (uint32_t)run >= nsb) {
        *rc = lrc;
        return false;
      }
      uint32_t step = 1U + (uint32_t)run;
      sb = prev_sb + step;
      sbx += step;
      if (sbx >= sbw) {
        if (sbx < sbw * 2U) {
          sbx -= sbw;
          sby++;
        } else {
          sby = sb / sbw;
          sbx = sb - sby * sbw;
        }
      }
    }
    prev_sb = sb;

    uint32_t is_8x8 = zcrudp_rc_dec_bit(&lrc, &m.is_8x8);
    if (is_8x8) {
      uint32_t bx = sbx * 2U, by = sby * 2U;
      if (bx >= bw || by >= bh) {
        *rc = lrc;
        return false;
      }
      if (!zcvid_tq_get_block(&lrc, &m, &st, by * bw + bx, 1U, count, &got, out)) {
        *rc = lrc;
        return false;
      }
    } else {
      uint32_t b0 = zcrudp_rc_dec_bit(&lrc, &m.qmask[0]);
      uint32_t b1 = zcrudp_rc_dec_bit(&lrc, &m.qmask[1U + b0]);
      uint32_t b2 = zcrudp_rc_dec_bit(&lrc, &m.qmask[3U + b0 + b1]);
      uint32_t b3 = ((b0 | b1 | b2) != 0U) ? zcrudp_rc_dec_bit(&lrc, &m.qmask[5U + b0 + b1 + b2]) : 1U;
      uint32_t qmask = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3);
      for (uint32_t q = 0U; q < 4U; ++q) {
        if (((qmask >> q) & 1U) == 0U) continue;
        uint32_t bx = sbx * 2U + (q & 1U);
        uint32_t by = sby * 2U + (q >> 1);
        if (bx >= bw || by >= bh) {
          *rc = lrc;
          return false;
        }
        if (!zcvid_tq_get_block(&lrc, &m, &st, by * bw + bx, 0U, count, &got, out)) {
          *rc = lrc;
          return false;
        }
      }
    }
  }
  *rc = lrc;
  return got == count;
}

/* ---------------------------------------------------------------------------
 * Motion Vector Pack / Range-Coded DPCM (wire-compatible)
 * ------------------------------------------------------------------------- */
static uint32_t zcvid_pack_mvs(const int32_t *mvs, uint32_t num_tiles, uint8_t *out, bool wide_mv) {
  bool any_nz = false;
  for (uint32_t i = 0U; i < num_tiles && !any_nz; ++i) {
    any_nz = (mvs[i * 2U] | mvs[i * 2U + 1U]) != 0;
  }
  if (!any_nz) return 0U;
  uint8_t *o = out;
  for (uint32_t i = 0U; i < num_tiles;) {
    int32_t dx = mvs[i * 2U], dy = mvs[i * 2U + 1U];
    uint32_t run = 1U;
    while (i + run < num_tiles && run < 256U && mvs[(i + run) * 2U] == dx &&
           mvs[(i + run) * 2U + 1U] == dy) {
      run++;
    }
    *o++ = (uint8_t)(run - 1U);
    if (wide_mv) {
      int16_t sdx = (int16_t)dx, sdy = (int16_t)dy;
      *o++ = (uint8_t)((uint16_t)sdx & 0xFFU);
      *o++ = (uint8_t)((uint16_t)sdx >> 8);
      *o++ = (uint8_t)((uint16_t)sdy & 0xFFU);
      *o++ = (uint8_t)((uint16_t)sdy >> 8);
    } else {
      *o++ = (uint8_t)(int8_t)dx;
      *o++ = (uint8_t)(int8_t)dy;
    }
    i += run;
  }
  return (uint32_t)(o - out);
}

static bool zcvid_unpack_mvs(const uint8_t *in, uint32_t in_len, int32_t *dst,
                            uint32_t num_tiles, bool wide_mv) {
  uint32_t step = wide_mv ? 5U : 3U;
  uint32_t t = 0U;
  for (uint32_t i = 0U; i + step <= in_len; i += step) {
    uint32_t run = (uint32_t)in[i] + 1U;
    if (t + run > num_tiles) return false;
    int32_t dx, dy;
    if (wide_mv) {
      dx = (int32_t)(int16_t)((uint16_t)in[i + 1U] | ((uint16_t)in[i + 2U] << 8));
      dy = (int32_t)(int16_t)((uint16_t)in[i + 3U] | ((uint16_t)in[i + 4U] << 8));
    } else {
      dx = (int32_t)(int8_t)in[i + 1U];
      dy = (int32_t)(int8_t)in[i + 2U];
    }
    for (uint32_t k = 0U; k < run; ++k, ++t) {
      dst[t * 2U] = dx;
      dst[t * 2U + 1U] = dy;
    }
  }
  return t == num_tiles;
}

static inline int32_t zcvid_mv_med(int32_t a, int32_t b, int32_t c) {
  int32_t mn = (a < b) ? a : b;
  int32_t mx = (a < b) ? b : a;
  if (c >= mx) return mn;
  if (c <= mn) return mx;
  return a + b - c;
}

#define ZCVID_MV_EC_MAG_CTX 8U
#define ZCVID_MV_EC_MAG_MAXLEN 9U

typedef struct {
  zcrudp_rc_prob_t nz_tile[4];
  zcrudp_rc_prob_t nz_comp[2];
  zcrudp_rc_prob_t sign[2];
  zcrudp_rc_prob_t mag[2][ZCVID_MV_EC_MAG_CTX];
} zcvid_mv_ec_model_t;

static void zcvid_mv_ec_model_reset(zcvid_mv_ec_model_t *m) {
  static const uint16_t nz_priors[4] = {28000U, 16384U, 16384U, 10000U};
  for (uint32_t i = 0U; i < 4U; ++i) zcrudp_rc_prob_init(&m->nz_tile[i], nz_priors[i]);
  for (uint32_t c = 0U; c < 2U; ++c) {
    zcrudp_rc_prob_init(&m->nz_comp[c], 16384U);
    zcrudp_rc_prob_init(&m->sign[c], 16384U);
    for (uint32_t k = 0U; k < ZCVID_MV_EC_MAG_CTX; ++k) {
      zcrudp_rc_prob_init(&m->mag[c][k], (k == 0U) ? 24000U : 16384U);
    }
  }
}

static inline void zcvid_ec_pack_mv_tile(zcrudp_rc_enc_t *rc, zcvid_mv_ec_model_t *m,
                                        int32_t rx, int32_t ry, uint32_t ctx, uint32_t *out_nz) {
  uint32_t nz = (rx != 0 || ry != 0) ? 1U : 0U;
  zcrudp_rc_enc_bit(rc, &m->nz_tile[ctx], nz);
  if (nz) {
    uint32_t nzx = (rx != 0) ? 1U : 0U;
    uint32_t nzy = (ry != 0) ? 1U : 0U;
    zcrudp_rc_enc_bit(rc, &m->nz_comp[0], nzx);
    if (nzx) {
      zcrudp_rc_enc_bit(rc, &m->sign[0], (rx < 0) ? 1U : 0U);
      uint32_t mag = (uint32_t)((rx < 0) ? -rx : rx) - 1U;
      zcrudp_rc_enc_egk(rc, m->mag[0], ZCVID_MV_EC_MAG_CTX, ZCVID_MV_EC_MAG_MAXLEN, mag);
      zcrudp_rc_enc_bit(rc, &m->nz_comp[1], nzy);
    }
    if (nzy) {
      zcrudp_rc_enc_bit(rc, &m->sign[1], (ry < 0) ? 1U : 0U);
      uint32_t mag = (uint32_t)((ry < 0) ? -ry : ry) - 1U;
      zcrudp_rc_enc_egk(rc, m->mag[1], ZCVID_MV_EC_MAG_CTX, ZCVID_MV_EC_MAG_MAXLEN, mag);
    }
  }
  *out_nz = nz;
}

static uint32_t zcvid_ec_pack_mvs(const int32_t *mvs, uint32_t tiles_x, uint32_t tiles_y,
                                 bool qpel, uint8_t *out, size_t out_cap) {
  if (out_cap < 2U || tiles_x == 0U || tiles_y == 0U) return 0U;
  out[0] = qpel ? 1U : 0U;
  zcrudp_rc_enc_t rc;
  zcrudp_rc_enc_init(&rc, out + 1U, out_cap - 1U);
  zcvid_mv_ec_model_t m;
  zcvid_mv_ec_model_reset(&m);
  uint8_t row_nz[512];
  memset(row_nz, 0, (tiles_x < 512U) ? tiles_x : 512U);

  {
    const int32_t *row = mvs;
    uint32_t left_nz = 0U;
    int32_t ax = 0, ay = 0;
    for (uint32_t tx = 0U; tx < tiles_x; ++tx) {
      int32_t dx = row[tx * 2U + 0U];
      int32_t dy = row[tx * 2U + 1U];
      uint32_t nz = 0U;
      zcvid_ec_pack_mv_tile(&rc, &m, dx - ax, dy - ay, left_nz, &nz);
      ax = dx;
      ay = dy;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
      if (rc.overflow) return 0U;
    }
  }
  for (uint32_t ty = 1U; ty < tiles_y; ++ty) {
    const int32_t *row = mvs + (size_t)ty * tiles_x * 2U;
    const int32_t *up = row - (size_t)tiles_x * 2U;
    int32_t cx = up[0], cy = up[1];
    int32_t ax = row[0], ay = row[1];
    uint32_t left_nz = 0U;
    zcvid_ec_pack_mv_tile(&rc, &m, ax - cx, ay - cy, (uint32_t)row_nz[0] << 1, &left_nz);
    row_nz[0] = (uint8_t)left_nz;
    if (rc.overflow) return 0U;
    for (uint32_t tx = 1U; tx < tiles_x; ++tx) {
      int32_t bx = up[tx * 2U + 0U];
      int32_t by = up[tx * 2U + 1U];
      int32_t dx = row[tx * 2U + 0U];
      int32_t dy = row[tx * 2U + 1U];
      int32_t px = zcvid_mv_med(ax, bx, cx);
      int32_t py = zcvid_mv_med(ay, by, cy);
      uint32_t nz = 0U;
      uint32_t ctx = left_nz | ((uint32_t)row_nz[tx & 511U] << 1);
      zcvid_ec_pack_mv_tile(&rc, &m, dx - px, dy - py, ctx, &nz);
      ax = dx;
      ay = dy;
      cx = bx;
      cy = by;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
      if (rc.overflow) return 0U;
    }
  }
  size_t len = zcrudp_rc_enc_finish(&rc);
  if (rc.overflow) return 0U;
  return (uint32_t)(1U + len);
}

static inline uint32_t zcvid_ec_unpack_mv_tile(zcrudp_rc_dec_t *rc, zcvid_mv_ec_model_t *m,
                                              uint32_t ctx, int32_t *out_rx, int32_t *out_ry) {
  uint32_t nz = zcrudp_rc_dec_bit(rc, &m->nz_tile[ctx]);
  if (!nz) {
    *out_rx = 0;
    *out_ry = 0;
    return 0U;
  }
  int32_t rx = 0, ry = 0;
  uint32_t nzx = zcrudp_rc_dec_bit(rc, &m->nz_comp[0]);
  uint32_t nzy = 1U;
  if (nzx) {
    uint32_t neg = zcrudp_rc_dec_bit(rc, &m->sign[0]);
    int32_t mag = (int32_t)zcrudp_rc_dec_egk(rc, m->mag[0], ZCVID_MV_EC_MAG_CTX,
                                             ZCVID_MV_EC_MAG_MAXLEN) + 1;
    rx = neg ? -mag : mag;
    nzy = zcrudp_rc_dec_bit(rc, &m->nz_comp[1]);
  }
  if (nzy) {
    uint32_t neg = zcrudp_rc_dec_bit(rc, &m->sign[1]);
    int32_t mag = (int32_t)zcrudp_rc_dec_egk(rc, m->mag[1], ZCVID_MV_EC_MAG_CTX,
                                             ZCVID_MV_EC_MAG_MAXLEN) + 1;
    ry = neg ? -mag : mag;
  }
  *out_rx = rx;
  *out_ry = ry;
  return 1U;
}

static bool zcvid_ec_unpack_mvs(const uint8_t *in, uint32_t in_len, int32_t *dst,
                               uint32_t tiles_x, uint32_t tiles_y, bool *out_qpel) {
  if (in_len < 2U || tiles_x == 0U || tiles_y == 0U) return false;
  if ((in[0] & ~1U) != 0U) return false;
  *out_qpel = (in[0] & 1U) != 0U;
  zcrudp_rc_dec_t rc;
  zcrudp_rc_dec_init(&rc, in + 1U, (size_t)(in_len - 1U));
  zcvid_mv_ec_model_t m;
  zcvid_mv_ec_model_reset(&m);
  uint8_t row_nz[512];
  memset(row_nz, 0, (tiles_x < 512U) ? tiles_x : 512U);

  {
    int32_t *row = dst;
    uint32_t left_nz = 0U;
    int32_t ax = 0, ay = 0;
    for (uint32_t tx = 0U; tx < tiles_x; ++tx) {
      int32_t rx, ry;
      uint32_t nz = zcvid_ec_unpack_mv_tile(&rc, &m, left_nz, &rx, &ry);
      if (nz) {
        ax += rx;
        ay += ry;
        if (ax < -512 || ax > 512 || ay < -512 || ay > 512) return false;
      }
      row[tx * 2U + 0U] = ax;
      row[tx * 2U + 1U] = ay;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
    }
  }
  for (uint32_t ty = 1U; ty < tiles_y; ++ty) {
    int32_t *row = dst + (size_t)ty * tiles_x * 2U;
    const int32_t *up = row - (size_t)tiles_x * 2U;
    int32_t cx = up[0], cy = up[1];
    int32_t rx, ry;
    uint32_t left_nz = zcvid_ec_unpack_mv_tile(&rc, &m, (uint32_t)row_nz[0] << 1, &rx, &ry);
    int32_t ax = cx + rx, ay = cy + ry;
    if (left_nz && (ax < -512 || ax > 512 || ay < -512 || ay > 512)) return false;
    row[0] = ax;
    row[1] = ay;
    row_nz[0] = (uint8_t)left_nz;
    for (uint32_t tx = 1U; tx < tiles_x; ++tx) {
      int32_t bx = up[tx * 2U + 0U];
      int32_t by = up[tx * 2U + 1U];
      int32_t px = zcvid_mv_med(ax, bx, cx);
      int32_t py = zcvid_mv_med(ay, by, cy);
      uint32_t ctx = left_nz | ((uint32_t)row_nz[tx & 511U] << 1);
      uint32_t nz = zcvid_ec_unpack_mv_tile(&rc, &m, ctx, &rx, &ry);
      int32_t dx = px + rx;
      int32_t dy = py + ry;
      if (nz && (dx < -512 || dx > 512 || dy < -512 || dy > 512)) return false;
      row[tx * 2U + 0U] = dx;
      row[tx * 2U + 1U] = dy;
      ax = dx;
      ay = dy;
      cx = bx;
      cy = by;
      left_nz = nz;
      row_nz[tx & 511U] = (uint8_t)nz;
    }
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Portable Pipeline State & Pack/Unpack
 * ------------------------------------------------------------------------- */
struct gpu_pipeline_s {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  uint32_t bitrate_bps;
  uint32_t threshold;
  uint32_t delta_step;
  uint32_t gamma_q8;
  uint32_t neutral_y;
  uint32_t bw;
  uint32_t bh;
  uint32_t sbw;
  uint32_t sbh;
  uint32_t nblocks;
  uint32_t tiles_x;
  uint32_t tiles_y;
  uint32_t num_tiles;

  bool show_window;
  bool force_synth;
  bool packed;
  bool entropy;
  bool transform;
  bool transform_inter;
  bool qpel;
  bool dither;
  bool kf_requested;
  bool dec_mvs_zero;

  uint32_t entropy_limit_words;
  uint32_t enc_nsw_q8;
  uint32_t dec_nsw_q8;
  int32_t qp_offset;

  uint8_t *src_yuv;
  zcvid_gpu_dec_state_t enc_ref;
  zcvid_gpu_dec_state_t dec_state;
  bool enc_initialized;
  bool dec_initialized;

  uint32_t *raw_rec_buf;
  uint32_t raw_rec_cap_words;
  int32_t *enc_mv_buf;
  int32_t *dec_mv_buf;
  uint64_t *sort_a;
  uint64_t *sort_b;
  uint32_t sort_cap;
  uint8_t *enc_out_buf;
  size_t enc_out_cap;
  uint8_t *ec_scratch;
  size_t ec_scratch_cap;
  uint32_t *dec_unpack_buf;
  uint32_t dec_unpack_cap;

  uint8_t *enc_ec_line;
  uint32_t *enc_ec_lrow;
  uint32_t enc_ec_lrow_gen;
  zcrudp_tec_model_t enc_tec_model;

  uint8_t *dec_ec_line;
  uint32_t *dec_ec_lrow;
  uint32_t dec_ec_lrow_gen;
  zcrudp_tec_model_t dec_tec_model;

  uint64_t hint_frames;
  uint64_t hint_move_rects;
  uint64_t hint_moved_tiles;
  uint64_t hint_static_tiles;

#ifdef ZC_PORTABLE_HAVE_X11
  void *x11_lib;
  Display *x11_dpy;
  Window x11_win;
  GC x11_gc;
  Atom x11_wm_del;
  XImage *x11_img;
  uint32_t x11_win_w;
  uint32_t x11_win_h;
  int x11_depth;
  Visual *x11_vis;
  uint8_t *disp_rgba;
  uint8_t *disp_bgrx;
  bool x11_win_open;
  bool x11_closed;

  Display *x11_cap_dpy;
  Window x11_cap_root;
  uint32_t x11_cap_w;
  uint32_t x11_cap_h;
  bool x11_cap_ok;
  bool x11_cap_failed;

  Display *(*p_XOpenDisplay)(const char *);
  int (*p_XCloseDisplay)(Display *);
  int (*p_XDefaultScreen)(Display *);
  Window (*p_XRootWindow)(Display *, int);
  Visual *(*p_XDefaultVisual)(Display *, int);
  int (*p_XDefaultDepth)(Display *, int);
  GC (*p_XDefaultGC)(Display *, int);
  Window (*p_XCreateSimpleWindow)(Display *, Window, int, int, unsigned int,
                                  unsigned int, unsigned int, unsigned long,
                                  unsigned long);
  int (*p_XStoreName)(Display *, Window, const char *);
  int (*p_XSelectInput)(Display *, Window, long);
  Atom (*p_XInternAtom)(Display *, const char *, Bool);
  Status (*p_XSetWMProtocols)(Display *, Window, Atom *, int);
  int (*p_XMapWindow)(Display *, Window);
  int (*p_XFlush)(Display *);
  int (*p_XPending)(Display *);
  int (*p_XNextEvent)(Display *, XEvent *);
  XImage *(*p_XCreateImage)(Display *, Visual *, unsigned int, int, int,
                            char *, unsigned int, unsigned int, int, int);
  int (*p_XPutImage)(Display *, Drawable, GC, XImage *, int, int, int, int,
                     unsigned int, unsigned int);
  XImage *(*p_XGetImage)(Display *, Drawable, int, int, unsigned int,
                         unsigned int, unsigned long, int);
  Status (*p_XGetWindowAttributes)(Display *, Window, XWindowAttributes *);
  int (*p_XDestroyWindow)(Display *, Window);
  XErrorHandler (*p_XSetErrorHandler)(XErrorHandler);
  int (*p_XSync)(Display *, Bool);
  KeySym (*p_XLookupKeysym)(XKeyEvent *, int);
#endif
};

static size_t port_pack_sorted(const uint32_t *rec, const uint64_t *a, uint32_t num,
                               uint32_t ncls, uint8_t *out) {
  uint32_t counts[ZCVID_CLASSES_MAX] = {0};
  for (uint32_t n = 0U; n < num; ++n) counts[a[n] >> 60]++;

  uint8_t *o = out;
  uint32_t i = 0U;
  for (uint32_t cls = 0U; cls < ncls; ++cls) {
    o = zcvid_put_varint(o, counts[cls]);
    uint32_t prev = 0U;
    for (uint32_t n = 0U; n < counts[cls]; ++n, ++i) {
      uint32_t lo = (uint32_t)a[i];
      uint32_t id = (uint32_t)(a[i] >> 32) & 0x0FFFFFFFU;
      uint32_t w0 = (cls == 7U) ? rec[lo * 2U] : 0U;
      uint32_t w1 = (cls == 7U) ? rec[lo * 2U + 1U] : lo;
      o = zcvid_put_varint(o, id - prev);
      prev = id;
      if (cls < 6U) {
        *o++ = (uint8_t)w1;
        *o++ = (uint8_t)(w1 >> 8);
      } else if (cls == 6U) {
        memcpy(o, &w1, 4);
        o += 4;
      } else if (cls == 7U) {
        uint64_t v = ((uint64_t)(w1 & 0x3FFFFFFFU) << 6) | ((w0 >> 22) & 0x3FU);
        for (unsigned k = 0U; k < 5U; ++k) *o++ = (uint8_t)(v >> (8U * k));
      } else {
        *o++ = (uint8_t)w1;
        *o++ = (uint8_t)(w1 >> 8);
        *o++ = (uint8_t)(((w1 >> 16) & 0x3FU) | (((w1 >> 24) & 3U) << 6));
      }
    }
  }
  return (size_t)(o - out);
}

static uint32_t port_unpack_records(const uint8_t *in, size_t in_len, uint32_t *out,
                                    uint32_t max_words, uint32_t ncls) {
  const uint8_t *i = in, *end = in + in_len;
  uint32_t words = 0U;
  for (uint32_t cls = 0U; cls < ncls; ++cls) {
    uint32_t count = 0U;
    i = zcvid_get_varint(i, end, &count);
    if (!i) return UINT32_MAX;
    uint32_t id = 0U;
    for (uint32_t n = 0U; n < count; ++n) {
      uint32_t gap = 0U;
      i = zcvid_get_varint(i, end, &gap);
      if (!i || words + 2U > max_words) return UINT32_MAX;
      id += gap;
      uint32_t w0, w1;
      if (cls < 6U) {
        if (end - i < 2) return UINT32_MAX;
        w0 = ((cls >> 1) << 30) | ((cls & 1U) << 29) | (id & 0x1FFFFFFFU);
        w1 = (uint32_t)i[0] | ((uint32_t)i[1] << 8);
        i += 2;
      } else if (cls == 6U) {
        if (end - i < 4) return UINT32_MAX;
        w0 = 0xC0000000U | (id & 0x1FFFFFFFU);
        memcpy(&w1, i, 4);
        i += 4;
      } else if (cls == 7U) {
        if (end - i < 5) return UINT32_MAX;
        uint64_t v = 0ULL;
        for (unsigned k = 0U; k < 5U; ++k) v |= (uint64_t)i[k] << (8U * k);
        i += 5;
        w0 = 0xE0000000U | ((uint32_t)(v & 0x3FU) << 22) | (id & 0x3FFFFFU);
        w1 = (uint32_t)(v >> 6) & 0x3FFFFFFFU;
      } else {
        if (end - i < 3) return UINT32_MAX;
        w0 = 0xF0000000U | (id & 0x0FFFFFFFU);
        w1 = (uint32_t)i[0] | ((uint32_t)i[1] << 8) | ((uint32_t)(i[2] & 0x3FU) << 16) |
             ((uint32_t)(i[2] >> 6) << 24);
        i += 3;
      }
      out[words++] = w0;
      out[words++] = w1;
    }
  }
  return words;
}

static size_t port_ec_put_chunk(gpu_pipeline_t *p,
                                const uint32_t *rec,
                                const uint64_t *keys, uint32_t num, uint32_t ncls,
                                uint8_t *out, size_t out_cap) {
  zcrudp_tec_model_t *tm = &p->enc_tec_model;
  const uint32_t num_px = p->width * p->height;
  uint32_t counts[ZCVID_CLASSES_MAX] = {0};
  for (uint32_t n = 0U; n < num; ++n) counts[keys[n] >> 60]++;

  zcrudp_rc_enc_t rc;
  zcrudp_rc_enc_init(&rc, out, out_cap);
  zcvid_ec_intra_t im;
  uint32_t i = 0U;
  for (uint32_t cls = 0U; cls < ncls; ++cls) {
    if (counts[cls] >> ZCVID_EC_COUNT_BITS) return 0U;
    zcrudp_rc_enc_direct(&rc, counts[cls], ZCVID_EC_COUNT_BITS);
    if (counts[cls] == 0U) continue;
    if (cls < 6U) {
      zcrudp_tec_model_reset(tm, zcvid_ec_delta_stride(p->width, cls), 1U);
    } else if (cls == 8U) {
      zcrudp_tec_run_reset(tm);
      if (!zcvid_tq_put(&rc, tm, keys + i, counts[cls], p->bw, p->bh)) return 0U;
      i += counts[cls];
      continue;
    } else {
      zcrudp_tec_run_reset(tm);
      zcvid_ec_intra_reset(&im, p->enc_ec_line, p->enc_ec_lrow,
                          &p->enc_ec_lrow_gen, p->width, p->height);
    }
    const uint32_t unit = (cls == 7U) ? 2U : 1U;
    const uint32_t px = (cls < 6U) ? 0U : (cls == 6U) ? 2U : 4U;
    const uint32_t span = (cls < 6U) ? 1U : px / unit;
    const uint32_t id_limit = zcvid_ec_id_limit(num_px, cls);
    uint32_t next = 0U;
    uint32_t cur_px = 0U, cur_row = 0U, cur_x = 0U;
    for (uint32_t n = 0U; n < counts[cls]; ++n, ++i) {
      uint32_t lo = (uint32_t)keys[i];
      uint32_t id = (uint32_t)(keys[i] >> 32) & 0x0FFFFFFFU;
      uint32_t w0 = (cls == 7U) ? rec[lo * 2U] : 0U;
      uint32_t w1 = (cls == 7U) ? rec[lo * 2U + 1U] : lo;
      if (id < next || id >= id_limit) return 0U;
      zcrudp_tec_enc_run(&rc, tm, id - next);
      if (cls < 6U) {
        if (w1 >= 59049U) return 0U;
        zcrudp_tec_hist_clear(tm, (size_t)next * 10U, (size_t)id * 10U);
        zcvid_ec_put_trits(&rc, tm, (size_t)id * 10U, w1);
      } else {
        const unsigned shift = (cls == 6U) ? 0U : 2U;
        uint32_t y[4] = {0U, 0U, 0U, 0U}, cb, cr;
        if (cls == 6U) {
          y[0] = w1 & 0xFFU;
          cb = (w1 >> 8) & 0xFFU;
          y[1] = (w1 >> 16) & 0xFFU;
          cr = w1 >> 24;
        } else {
          if ((w1 >> 30) != 0U || ((w0 >> 28) & 1U) != 0U) return 0U;
          for (unsigned k = 0U; k < 4U; ++k) y[k] = (w1 >> (6U * k)) & 0x3FU;
          cb = (w1 >> 24) & 0x3FU;
          cr = (w0 >> 22) & 0x3FU;
        }
        uint8_t cb8 = (cls == 6U) ? (uint8_t)cb : (uint8_t)(cb << 2);
        uint8_t cr8 = (cls == 6U) ? (uint8_t)cr : (uint8_t)(cr << 2);
        uint32_t pidx = id * unit;
        if (pidx != cur_px) {
          zcvid_ec_idx_to_xy(&im, pidx, &cur_row, &cur_x);
          cur_px = pidx;
        }
        zcvid_ec_nb_t nb;
        uint32_t grad = 0U, pred;
        zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
        pred = zcvid_ec_predict(&nb, 1U, &grad);
        zcvid_ec_put_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift, cb);
        pred = zcvid_ec_predict(&nb, 2U, &grad);
        zcvid_ec_put_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift, cr);
        for (unsigned k = 0U; k < px; ++k) {
          if (k > 0U) zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
          pred = zcvid_ec_predict(&nb, 0U, &grad);
          zcvid_ec_put_sample(&rc, &im.luma, &im.act_y, pred, grad, shift, y[k]);
          zcvid_ec_store_xy(&im, cur_px, cur_row, cur_x,
                           (cls == 6U) ? (uint8_t)y[k] : zcvid_ec_expand6(y[k]), cb8, cr8);
          cur_px++;
          if (++cur_x == im.width) { cur_x = 0U; cur_row++; }
        }
      }
      next = id + span;
      if (rc.overflow) return 0U;
    }
  }
  size_t len = zcrudp_rc_enc_finish(&rc);
  if (rc.overflow) return 0U;
  return (len > 0U) ? len : 1U;
}

static uint32_t port_ec_get_chunk(gpu_pipeline_t *p,
                                  const uint8_t *in, size_t in_len,
                                  uint32_t *out, uint32_t max_words,
                                  uint32_t ncls, bool *out_has_tq) {
  zcrudp_tec_model_t *tm = &p->dec_tec_model;
  const uint32_t num_px = p->width * p->height;
  zcrudp_rc_dec_t rc;
  zcrudp_rc_dec_init(&rc, in, in_len);
  zcvid_ec_intra_t im;
  uint32_t words = 0U;
  bool has_tq = false;
  for (uint32_t cls = 0U; cls < ncls; ++cls) {
    uint32_t count = (uint32_t)zcrudp_rc_dec_direct(&rc, ZCVID_EC_COUNT_BITS);
    if (count == 0U) continue;
    if (count > (max_words - words) / 2U) return UINT32_MAX;
    if (cls < 6U) {
      zcrudp_tec_model_reset(tm, zcvid_ec_delta_stride(p->width, cls), 1U);
    } else if (cls == 8U) {
      zcrudp_tec_run_reset(tm);
      if (!zcvid_tq_get(&rc, tm, count, out + words, p->bw, p->bh)) return UINT32_MAX;
      has_tq = true;
      words += count * 2U;
      continue;
    } else {
      zcrudp_tec_run_reset(tm);
      zcvid_ec_intra_reset(&im, p->dec_ec_line, p->dec_ec_lrow,
                          &p->dec_ec_lrow_gen, p->width, p->height);
    }
    const uint32_t unit = (cls == 7U) ? 2U : 1U;
    const uint32_t px = (cls < 6U) ? 0U : (cls == 6U) ? 2U : 4U;
    const uint32_t span = (cls < 6U) ? 1U : px / unit;
    const uint32_t id_limit = zcvid_ec_id_limit(num_px, cls);
    uint32_t next = 0U;
    uint32_t cur_px = 0U, cur_row = 0U, cur_x = 0U;
    for (uint32_t n = 0U; n < count; ++n) {
      uint64_t run = zcrudp_tec_dec_run(&rc, tm);
      if (run >= id_limit || next + (uint32_t)run >= id_limit) return UINT32_MAX;
      uint32_t id = next + (uint32_t)run;
      uint32_t w0, w1;
      if (cls < 6U) {
        zcrudp_tec_hist_clear(tm, (size_t)next * 10U, (size_t)id * 10U);
        w1 = zcvid_ec_get_trits(&rc, tm, (size_t)id * 10U);
        w0 = ((cls >> 1) << 30) | ((cls & 1U) << 29) | id;
      } else {
        const unsigned shift = (cls == 6U) ? 0U : 2U;
        uint32_t y[4] = {0U, 0U, 0U, 0U}, cb, cr;
        uint32_t pidx = id * unit;
        if (pidx != cur_px) {
          zcvid_ec_idx_to_xy(&im, pidx, &cur_row, &cur_x);
          cur_px = pidx;
        }
        zcvid_ec_nb_t nb;
        uint32_t grad = 0U, pred;
        zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
        pred = zcvid_ec_predict(&nb, 1U, &grad);
        cb = zcvid_ec_get_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift);
        pred = zcvid_ec_predict(&nb, 2U, &grad);
        cr = zcvid_ec_get_sample(&rc, &im.chroma, &im.act_c, pred, grad, shift);
        uint8_t cb8 = (cls == 6U) ? (uint8_t)cb : (uint8_t)(cb << 2);
        uint8_t cr8 = (cls == 6U) ? (uint8_t)cr : (uint8_t)(cr << 2);
        for (unsigned k = 0U; k < px; ++k) {
          if (k > 0U) zcvid_ec_neighbours_xy(&im, cur_px, cur_row, cur_x, &nb);
          pred = zcvid_ec_predict(&nb, 0U, &grad);
          y[k] = zcvid_ec_get_sample(&rc, &im.luma, &im.act_y, pred, grad, shift);
          zcvid_ec_store_xy(&im, cur_px, cur_row, cur_x,
                           (cls == 6U) ? (uint8_t)y[k] : zcvid_ec_expand6(y[k]), cb8, cr8);
          cur_px++;
          if (++cur_x == im.width) { cur_x = 0U; cur_row++; }
        }
        if (cls == 6U) {
          w0 = 0xC0000000U | id;
          w1 = y[0] | (cb << 8) | (y[1] << 16) | (cr << 24);
        } else {
          w0 = 0xE0000000U | (cr << 22) | (id & 0x3FFFFFU);
          w1 = y[0] | (y[1] << 6) | (y[2] << 12) | (y[3] << 18) | (cb << 24);
        }
      }
      out[words++] = w0;
      out[words++] = w1;
      next = id + span;
    }
  }
  if (out_has_tq) *out_has_tq = has_tq;
  return words;
}

static uint32_t port_ec_unpack_records(gpu_pipeline_t *p, const uint8_t *in, size_t in_len,
                                       uint32_t *out, uint32_t max_words, uint32_t ncls) {
  if (in_len < 1U) return UINT32_MAX;
  const uint32_t chunks = in[0];
  if (chunks < 1U || chunks > ZCVID_EC_MAX_CHUNKS || in_len < 1U + (size_t)chunks * 8U) {
    return UINT32_MAX;
  }
  const uint8_t *data = in + 1U + (size_t)chunks * 8U;
  size_t left = in_len - 1U - (size_t)chunks * 8U;
  uint32_t total_words = 0U;
  for (uint32_t c = 0U; c < chunks; ++c) {
    uint32_t words = 0U, bytes = 0U;
    memcpy(&words, in + 1U + (size_t)c * 8U, 4);
    memcpy(&bytes, in + 5U + (size_t)c * 8U, 4);
    if ((words & 1U) != 0U || words > max_words - total_words || bytes > left) {
      return UINT32_MAX;
    }
    bool chunk_has_tq = false;
    uint32_t got = port_ec_get_chunk(p, data, bytes, out + total_words, words, ncls, &chunk_has_tq);
    (void)chunk_has_tq;
    if (got != words) {
      fprintf(stderr, "port_ec_unpack_records: chunk %u got=%u != words=%u (bytes=%u ncls=%u)\n",
              c, got, words, bytes, ncls);
      return UINT32_MAX;
    }
    data += bytes;
    left -= bytes;
    total_words += words;
  }
  if (total_words < max_words) {
    uint32_t plain = port_unpack_records(data, left, out + total_words,
                                         max_words - total_words, ncls);
    if (plain == UINT32_MAX) return UINT32_MAX;
    total_words += plain;
  }
  return total_words;
}

static uint32_t port_sort_records(gpu_pipeline_t *p, const uint32_t *rec,
                                  uint32_t num_records, bool sb_tq) {
  if (num_records > p->sort_cap) {
    uint32_t cap = num_records + num_records / 2U + 1024U;
    uint64_t *a = (uint64_t *)realloc(p->sort_a, (size_t)cap * sizeof(uint64_t));
    if (a) p->sort_a = a;
    uint64_t *b = (uint64_t *)realloc(p->sort_b, (size_t)cap * sizeof(uint64_t));
    if (b) p->sort_b = b;
    if (!a || !b) return 0U;
    p->sort_cap = cap;
  }
  const uint32_t bw = p->bw;
  const uint32_t sbw = p->sbw;
  const uint64_t bw_recip = (bw > 0U) ? ((1ULL << 32) / (uint64_t)bw) : 0ULL;
  uint32_t kept = 0U;
  for (uint32_t i = 0U; i < num_records; ++i) {
    uint32_t w0 = rec[i * 2U];
    if (w0 == ZCVID_NO_RECORD) continue;
    uint32_t w1 = rec[i * 2U + 1U];
    uint32_t id = 0U;
    uint32_t cls = zcvid_record_class(w0, &id);
    if (sb_tq && cls == 8U) {
      id = zcvid_tq_id_to_sb_key(id, bw, sbw, bw_recip);
    }
    uint32_t k32 = (cls << 28) | (id & 0x0FFFFFFFU);
    uint32_t lo = (cls == 7U) ? i : w1;
    p->sort_a[kept++] = ((uint64_t)k32 << 32) | (uint64_t)lo;
  }
  zcvid_radix_sort_256(p->sort_a, p->sort_b, kept);
  return kept;
}

/* ---------------------------------------------------------------------------
 * Portable Synthetic Pattern Generator & Forward 4x4 H.264 Transform Encoder
 * ------------------------------------------------------------------------- */
static void port_generate_synth_yuv(uint8_t *dst_yuv, uint32_t w, uint32_t h, uint32_t frame_idx) {
  int32_t box_w = (int32_t)(w / 5U);
  int32_t box_h = (int32_t)(h / 4U);
  if (box_w < 32) box_w = 32;
  if (box_h < 32) box_h = 32;
  int32_t span_x = (w > (uint32_t)box_w + 16U) ? (int32_t)(w - (uint32_t)box_w - 8U) : 1;
  int32_t span_y = (h > (uint32_t)box_h + 16U) ? (int32_t)(h - (uint32_t)box_h - 8U) : 1;
  int32_t bx0 = (int32_t)((frame_idx * 4U) % (uint32_t)span_x);
  int32_t by0 = (int32_t)((frame_idx * 2U + 20U) % (uint32_t)span_y);
  uint32_t pan_x = frame_idx * 2U;

  for (uint32_t y = 0U; y < h; ++y) {
    uint8_t *row = dst_yuv + (size_t)y * w * 3U;
    uint32_t gy = y + frame_idx;
    for (uint32_t x = 0U; x < w; ++x) {
      uint32_t gx = x + pan_x;
      /* Smooth textured background (gentle diagonal waves + subtle tile grid) */
      int32_t yv = 40 + (int32_t)(((x * 160U) / (w ? w : 1U)) + ((y * 40U) / (h ? h : 1U)));
      if (((gx >> 5) ^ (gy >> 5)) & 1U) yv += 18;
      if ((x & 31U) == 0U || (y & 31U) == 0U) yv -= 10;

      int32_t cb = 110 + (int32_t)((x * 36U) / (w ? w : 1U));
      int32_t cr = 112 + (int32_t)((y * 34U) / (h ? h : 1U));

      /* Moving sharp foreground panel (desktop/UI-like card with internal texture) */
      if ((int32_t)x >= bx0 && (int32_t)x < bx0 + box_w &&
          (int32_t)y >= by0 && (int32_t)y < by0 + box_h) {
        int32_t lx = (int32_t)x - bx0;
        int32_t ly = (int32_t)y - by0;
        if (lx < 3 || ly < 3 || lx >= box_w - 3 || ly >= box_h - 3) {
          yv = 235;
          cb = 128;
          cr = 128;
        } else if (ly < 18) {
          yv = 68 + (lx / 8) * 3;
          cb = 196;
          cr = 92;
        } else {
          int32_t check = (((lx / 16) + (ly / 16)) & 1) ? 34 : -18;
          yv = zcvid_dec_clamp_i32(138 + check + (lx & 7) * 2, 16, 240);
          cb = 96;
          cr = 180;
        }
      }

      row[x * 3U + 0U] = (uint8_t)zcvid_dec_clamp_i32(yv, 0, 255);
      row[x * 3U + 1U] = (uint8_t)zcvid_dec_clamp_i32(cb, 0, 255);
      row[x * 3U + 2U] = (uint8_t)zcvid_dec_clamp_i32(cr, 0, 255);
    }
  }
}

static void port_motion_decay_pass1(zcvid_gpu_dec_state_t *st,
                                    const zcvid_gpu_dec_params_t *p,
                                    const zcvid_mv_t *tile_mvs) {
  uint32_t w = st->width;
  uint32_t h = st->height;
  uint32_t tiles_x = (w + 31U) >> 5;
  for (uint32_t py = 0U; py < h; ++py) {
    for (uint32_t px = 0U; px < w; ++px) {
      uint32_t idx = py * w + px;
      uint32_t rw = zcvid_dec_prev_shifted(st, p, tile_mvs, tiles_x, idx);
      int32_t y  = zcvid_dec_leaky((int32_t)(rw & 0xFFU), (int32_t)p->neutral_y, p->gamma_q8);
      int32_t cb = zcvid_dec_leaky((int32_t)((rw >> 8) & 0xFFU), 128, p->gamma_q8);
      int32_t cr = zcvid_dec_leaky((int32_t)((rw >> 16) & 0xFFU), 128, p->gamma_q8);
      int32_t d  = zcvid_dec_step_of(rw, p->delta_min) >> 1;
      if (d < (int32_t)p->delta_min) d = (int32_t)p->delta_min;
      st->cur_yuvd[idx] = (uint32_t)y | ((uint32_t)cb << 8) |
                          ((uint32_t)cr << 16) | ((uint32_t)d << 24);
    }
  }
}

static void port_estimate_mvs(const gpu_pipeline_t *p, const uint8_t *src_yuv,
                              const zcvid_gpu_dec_state_t *ref, int32_t *out_mvs,
                              bool qpel, uint64_t *out_moved, uint64_t *out_static) {
  const uint32_t w = p->width, h = p->height;
  const int32_t shift_mul = qpel ? 4 : 1;
  uint64_t moved = 0ULL, stat = 0ULL;
  /* Subsampled diamond + local raster motion search per 32x32 tile */
  for (uint32_t ty = 0U; ty < p->tiles_y; ++ty) {
    for (uint32_t tx = 0U; tx < p->tiles_x; ++tx) {
      uint32_t ti = ty * p->tiles_x + tx;
      uint32_t x0 = tx * 32U, y0 = ty * 32U;
      uint32_t x1 = (x0 + 32U <= w) ? (x0 + 32U) : w;
      uint32_t y1 = (y0 + 32U <= h) ? (y0 + 32U) : h;

      int32_t best_dx = 0, best_dy = 0;
      uint32_t best_sad = 0U;
      for (uint32_t py = y0 + 2U; py < y1; py += 8U) {
        for (uint32_t px = x0 + 2U; px < x1; px += 8U) {
          int32_t sy = (int32_t)src_yuv[(py * w + px) * 3U];
          int32_t ry = (int32_t)(ref->prev_yuvd[py * w + px] & 0xFFU);
          best_sad += (uint32_t)abs(sy - ry);
        }
      }
      uint32_t zero_sad = best_sad;
      if (zero_sad > 24U) {
        int32_t cand[10][2] = {
          {0, 0}, {-2, 0}, {2, 0}, {-4, -2}, {4, 2},
          {-4, 0}, {4, 0}, {0, -2}, {0, 2}, {0, 0}
        };
        if (tx > 0U) {
          cand[9][0] = out_mvs[(ti - 1U) * 2U + 0U] / shift_mul;
          cand[9][1] = out_mvs[(ti - 1U) * 2U + 1U] / shift_mul;
        }
        for (int c = 1; c < 10; ++c) {
          int32_t dx = cand[c][0], dy = cand[c][1];
          if (dx == 0 && dy == 0) continue;
          if (dx < -60 || dx > 60 || dy < -60 || dy > 60) continue;
          uint32_t sad = 0U;
          for (uint32_t py = y0 + 2U; py < y1; py += 8U) {
            int32_t ry_coord = zcvid_dec_clamp_i32((int32_t)py - dy, 0, (int32_t)h - 1);
            for (uint32_t px = x0 + 2U; px < x1; px += 8U) {
              int32_t rx_coord = zcvid_dec_clamp_i32((int32_t)px - dx, 0, (int32_t)w - 1);
              int32_t sy = (int32_t)src_yuv[(py * w + px) * 3U];
              int32_t ry = (int32_t)(ref->prev_yuvd[(uint32_t)ry_coord * w + (uint32_t)rx_coord] & 0xFFU);
              sad += (uint32_t)abs(sy - ry);
            }
          }
          if (sad + 16U < best_sad) {
            best_sad = sad;
            best_dx = dx;
            best_dy = dy;
          }
        }
      }
      out_mvs[ti * 2U + 0U] = best_dx * shift_mul;
      out_mvs[ti * 2U + 1U] = best_dy * shift_mul;
      if (best_dx != 0 || best_dy != 0 || zero_sad > 32U) moved++;
      else stat++;
    }
  }
  *out_moved = moved;
  *out_static = stat;
}

static void port_tq_forward_4x4(const int32_t res[16], int32_t out_w[16]) {
  int32_t t[16];
  for (uint32_t i = 0U; i < 4U; ++i) {
    int32_t s0 = res[i * 4U + 0U], s1 = res[i * 4U + 1U];
    int32_t s2 = res[i * 4U + 2U], s3 = res[i * 4U + 3U];
    int32_t p0 = s0 + s3, p1 = s1 + s2;
    int32_t p2 = s1 - s2, p3 = s0 - s3;
    t[i * 4U + 0U] = p0 + p1;
    t[i * 4U + 1U] = (p3 * 2) + p2;
    t[i * 4U + 2U] = p0 - p1;
    t[i * 4U + 3U] = p3 - (p2 * 2);
  }
  for (uint32_t j = 0U; j < 4U; ++j) {
    int32_t s0 = t[0U + j], s1 = t[4U + j];
    int32_t s2 = t[8U + j], s3 = t[12U + j];
    int32_t p0 = s0 + s3, p1 = s1 + s2;
    int32_t p2 = s1 - s2, p3 = s0 - s3;
    out_w[0U + j]  = p0 + p1;
    out_w[4U + j]  = (p3 * 2) + p2;
    out_w[8U + j]  = p0 - p1;
    out_w[12U + j] = p3 - (p2 * 2);
  }
}

static void port_tq_quant_4x4(const int32_t w[16], uint32_t qp, bool is_inter, int16_t lv[16]) {
  static const int32_t TQ_MF[18] = {
    13107, 5243, 8066,  11916, 4660, 7490,  10082, 4194, 6554,
     9362, 3647, 5825,   8192, 3355, 5243,   7282, 2893, 4559
  };
  static const uint32_t TQ_ZZ[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
  };
  const uint32_t qbits = 15U + (qp / 6U);
  const int64_t dz = (int64_t)((1ULL << qbits) / (is_inter ? 6ULL : 3ULL));
  const uint32_t qp_mod = qp % 6U;
  for (uint32_t k = 0U; k < 16U; ++k) {
    uint32_t r = TQ_ZZ[k];
    int32_t c = w[r];
    int32_t ac = (c < 0) ? -c : c;
    int32_t mf = TQ_MF[qp_mod * 3U + zcvid_dec_tq_class(r)];
    int32_t mag = (int32_t)(((int64_t)ac * (int64_t)mf + dz) >> qbits);
    int32_t s = (c < 0) ? -mag : mag;
    lv[k] = (int16_t)s;
  }
}

/* ---------------------------------------------------------------------------
 * Public API Implementation (gpu_pipeline_win32.h)
 * ------------------------------------------------------------------------- */
void gpu_pipeline_set_log_file(void *stdio_file) {
  g_log_file = (FILE *)stdio_file;
}

static void port_log_err(const char *msg) {
  fprintf(stderr, "%s", msg);
  if (g_log_file) {
    fprintf(g_log_file, "%s", msg);
    fflush(g_log_file);
  }
}

#ifdef ZC_PORTABLE_HAVE_X11
static volatile int g_x11_err_flag = 0;

static int port_x11_error_handler(Display *dpy, XErrorEvent *ev) {
  (void)dpy;
  (void)ev;
  g_x11_err_flag = 1;
  return 0;
}

#define ZC_X11_DLSYM(lib, dst, name)                                           \
  do {                                                                         \
    void *sym_ = dlsym((lib), (name));                                         \
    if (!sym_) return false;                                                   \
    memcpy(&(dst), &sym_, sizeof(dst));                                        \
  } while (0)

static bool port_x11_load(gpu_pipeline_t *p) {
  if (p->x11_lib) return true;
  void *lib = dlopen("libX11.so.6", RTLD_LAZY);
  if (!lib) lib = dlopen("libX11.so", RTLD_LAZY);
  if (!lib) return false;
  p->x11_lib = lib;
  ZC_X11_DLSYM(lib, p->p_XOpenDisplay, "XOpenDisplay");
  ZC_X11_DLSYM(lib, p->p_XCloseDisplay, "XCloseDisplay");
  ZC_X11_DLSYM(lib, p->p_XDefaultScreen, "XDefaultScreen");
  ZC_X11_DLSYM(lib, p->p_XRootWindow, "XRootWindow");
  ZC_X11_DLSYM(lib, p->p_XDefaultVisual, "XDefaultVisual");
  ZC_X11_DLSYM(lib, p->p_XDefaultDepth, "XDefaultDepth");
  ZC_X11_DLSYM(lib, p->p_XDefaultGC, "XDefaultGC");
  ZC_X11_DLSYM(lib, p->p_XCreateSimpleWindow, "XCreateSimpleWindow");
  ZC_X11_DLSYM(lib, p->p_XStoreName, "XStoreName");
  ZC_X11_DLSYM(lib, p->p_XSelectInput, "XSelectInput");
  ZC_X11_DLSYM(lib, p->p_XInternAtom, "XInternAtom");
  ZC_X11_DLSYM(lib, p->p_XSetWMProtocols, "XSetWMProtocols");
  ZC_X11_DLSYM(lib, p->p_XMapWindow, "XMapWindow");
  ZC_X11_DLSYM(lib, p->p_XFlush, "XFlush");
  ZC_X11_DLSYM(lib, p->p_XPending, "XPending");
  ZC_X11_DLSYM(lib, p->p_XNextEvent, "XNextEvent");
  ZC_X11_DLSYM(lib, p->p_XCreateImage, "XCreateImage");
  ZC_X11_DLSYM(lib, p->p_XPutImage, "XPutImage");
  ZC_X11_DLSYM(lib, p->p_XGetImage, "XGetImage");
  ZC_X11_DLSYM(lib, p->p_XGetWindowAttributes, "XGetWindowAttributes");
  ZC_X11_DLSYM(lib, p->p_XDestroyWindow, "XDestroyWindow");
  ZC_X11_DLSYM(lib, p->p_XSetErrorHandler, "XSetErrorHandler");
  ZC_X11_DLSYM(lib, p->p_XSync, "XSync");
  ZC_X11_DLSYM(lib, p->p_XLookupKeysym, "XLookupKeysym");
  return true;
}

static bool port_x11_alloc_image(gpu_pipeline_t *p, uint32_t win_w, uint32_t win_h) {
  if (win_w < 64U) win_w = 64U;
  if (win_h < 64U) win_h = 64U;
  if (p->x11_img) {
    p->x11_img->data = NULL;
    XDestroyImage(p->x11_img);
    p->x11_img = NULL;
  }
  free(p->disp_bgrx);
  p->disp_bgrx = (uint8_t *)calloc((size_t)win_w * (size_t)win_h, 4U);
  if (!p->disp_bgrx) return false;
  p->x11_img = p->p_XCreateImage(p->x11_dpy, p->x11_vis, (unsigned int)p->x11_depth,
                                 ZPixmap, 0, (char *)p->disp_bgrx, win_w, win_h, 32,
                                 (int)(win_w * 4U));
  if (!p->x11_img) {
    free(p->disp_bgrx);
    p->disp_bgrx = NULL;
    return false;
  }
  p->x11_win_w = win_w;
  p->x11_win_h = win_h;
  return true;
}

static bool port_x11_open_window(gpu_pipeline_t *p) {
  if (!port_x11_load(p)) return false;
  Display *dpy = p->p_XOpenDisplay(NULL);
  if (!dpy) return false;
  int scr = p->p_XDefaultScreen(dpy);
  int depth = p->p_XDefaultDepth(dpy, scr);
  if (depth != 24 && depth != 32) {
    p->p_XCloseDisplay(dpy);
    return false;
  }
  Visual *vis = p->p_XDefaultVisual(dpy, scr);
  Window root = p->p_XRootWindow(dpy, scr);
  uint32_t win_w = p->width;
  uint32_t win_h = p->height;
  if (win_w > 1280U) {
    win_h = (uint32_t)(((uint64_t)win_h * 1280ULL) / win_w);
    win_w = 1280U;
  }
  if (win_h > 720U && win_w > 0U) {
    win_w = (uint32_t)(((uint64_t)win_w * 720ULL) / win_h);
    win_h = 720U;
  }
  if (win_w < 64U) win_w = 64U;
  if (win_h < 64U) win_h = 64U;

  p->disp_rgba = (uint8_t *)malloc((size_t)p->width * (size_t)p->height * 4U);
  if (!p->disp_rgba) {
    p->p_XCloseDisplay(dpy);
    return false;
  }

  Window win = p->p_XCreateSimpleWindow(dpy, root, 0, 0, win_w, win_h, 0, 0, 0);
  p->p_XStoreName(dpy, win, "zcstream — Zcvid Live Receiver");
  p->p_XSelectInput(dpy, win, StructureNotifyMask | ExposureMask | KeyPressMask);
  Atom wm_del = p->p_XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  p->p_XSetWMProtocols(dpy, win, &wm_del, 1);

  p->x11_dpy = dpy;
  p->x11_win = win;
  p->x11_gc = p->p_XDefaultGC(dpy, scr);
  p->x11_wm_del = wm_del;
  p->x11_depth = depth;
  p->x11_vis = vis;
  if (!port_x11_alloc_image(p, win_w, win_h)) {
    p->p_XDestroyWindow(dpy, win);
    p->p_XCloseDisplay(dpy);
    p->x11_dpy = NULL;
    free(p->disp_rgba);
    p->disp_rgba = NULL;
    return false;
  }

  p->p_XMapWindow(dpy, win);
  p->p_XFlush(dpy);
  p->x11_win_open = true;
  return true;
}

static void port_rgba_to_bgrx(const uint8_t *rgba, uint32_t sw, uint32_t sh,
                              uint8_t *bgrx, uint32_t dw, uint32_t dh) {
  if (sw == dw && sh == dh) {
    size_t n = (size_t)dw * (size_t)dh;
    for (size_t i = 0U; i < n; ++i) {
      bgrx[i * 4U + 0U] = rgba[i * 4U + 2U];
      bgrx[i * 4U + 1U] = rgba[i * 4U + 1U];
      bgrx[i * 4U + 2U] = rgba[i * 4U + 0U];
      bgrx[i * 4U + 3U] = 0xFFU;
    }
    return;
  }
  for (uint32_t dy = 0U; dy < dh; ++dy) {
    uint32_t sy = (uint32_t)(((uint64_t)dy * sh) / dh);
    const uint8_t *srow = rgba + (size_t)sy * sw * 4U;
    uint8_t *drow = bgrx + (size_t)dy * dw * 4U;
    for (uint32_t dx = 0U; dx < dw; ++dx) {
      uint32_t sx = (uint32_t)(((uint64_t)dx * sw) / dw);
      drow[dx * 4U + 0U] = srow[sx * 4U + 2U];
      drow[dx * 4U + 1U] = srow[sx * 4U + 1U];
      drow[dx * 4U + 2U] = srow[sx * 4U + 0U];
      drow[dx * 4U + 3U] = 0xFFU;
    }
  }
}

static bool port_x11_open_capture(gpu_pipeline_t *p) {
  if (!port_x11_load(p)) return false;
  Display *dpy = p->p_XOpenDisplay(NULL);
  if (!dpy) return false;
  int scr = p->p_XDefaultScreen(dpy);
  Window root = p->p_XRootWindow(dpy, scr);
  XWindowAttributes attr;
  memset(&attr, 0, sizeof(attr));
  if (!p->p_XGetWindowAttributes(dpy, root, &attr) || attr.width < 16 || attr.height < 16) {
    p->p_XCloseDisplay(dpy);
    return false;
  }
  unsigned int probe_w = (attr.width > 16) ? 16U : (unsigned int)attr.width;
  unsigned int probe_h = (attr.height > 16) ? 16U : (unsigned int)attr.height;
  g_x11_err_flag = 0;
  XErrorHandler prev_h = p->p_XSetErrorHandler(port_x11_error_handler);
  XImage *probe = p->p_XGetImage(dpy, root, 0, 0, probe_w, probe_h, AllPlanes, ZPixmap);
  p->p_XSync(dpy, False);
  p->p_XSetErrorHandler(prev_h);
  if (probe) XDestroyImage(probe);
  if (!probe || g_x11_err_flag != 0) {
    p->p_XCloseDisplay(dpy);
    return false;
  }
  p->x11_cap_dpy = dpy;
  p->x11_cap_root = root;
  p->x11_cap_w = (uint32_t)attr.width;
  p->x11_cap_h = (uint32_t)attr.height;
  p->x11_cap_ok = true;
  return true;
}

static bool port_x11_capture_yuv(gpu_pipeline_t *p) {
  if (!p->x11_cap_ok || !p->x11_cap_dpy) return false;
  g_x11_err_flag = 0;
  XErrorHandler prev_h = p->p_XSetErrorHandler(port_x11_error_handler);
  XImage *img = p->p_XGetImage(p->x11_cap_dpy, p->x11_cap_root, 0, 0,
                               p->x11_cap_w, p->x11_cap_h, AllPlanes, ZPixmap);
  p->p_XSync(p->x11_cap_dpy, False);
  p->p_XSetErrorHandler(prev_h);
  if (!img || !img->data || g_x11_err_flag != 0 || img->bits_per_pixel < 24) {
    if (img) XDestroyImage(img);
    return false;
  }

  uint32_t sw = (uint32_t)img->width;
  uint32_t sh = (uint32_t)img->height;
  uint32_t dw = p->width;
  uint32_t dh = p->height;
  uint32_t step = (uint32_t)(img->bits_per_pixel / 8);
  const uint8_t *base = (const uint8_t *)img->data;

  for (uint32_t dy = 0U; dy < dh; ++dy) {
    uint32_t sy = (uint32_t)(((uint64_t)dy * sh) / dh);
    const uint8_t *srow = base + (size_t)sy * (size_t)img->bytes_per_line;
    uint8_t *drow = p->src_yuv + (size_t)dy * dw * 3U;
    for (uint32_t dx = 0U; dx < dw; ++dx) {
      uint32_t sx = (uint32_t)(((uint64_t)dx * sw) / dw);
      const uint8_t *px = srow + (size_t)sx * step;
      int32_t b = (int32_t)px[0];
      int32_t g = (int32_t)px[1];
      int32_t r = (int32_t)px[2];
      int32_t y  = (77 * r + 150 * g + 29 * b + 128) >> 8;
      int32_t cb = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
      int32_t cr = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
      drow[dx * 3U + 0U] = (uint8_t)zcvid_dec_clamp_i32(y, 0, 255);
      drow[dx * 3U + 1U] = (uint8_t)zcvid_dec_clamp_i32(cb, 0, 255);
      drow[dx * 3U + 2U] = (uint8_t)zcvid_dec_clamp_i32(cr, 0, 255);
    }
  }
  XDestroyImage(img);
  return true;
}

static void port_x11_cleanup(gpu_pipeline_t *p) {
  if (p->x11_img) {
    p->x11_img->data = NULL;
    XDestroyImage(p->x11_img);
    p->x11_img = NULL;
  }
  free(p->disp_bgrx);
  p->disp_bgrx = NULL;
  free(p->disp_rgba);
  p->disp_rgba = NULL;
  if (p->x11_dpy) {
    if (p->x11_win) p->p_XDestroyWindow(p->x11_dpy, p->x11_win);
    p->p_XCloseDisplay(p->x11_dpy);
    p->x11_dpy = NULL;
    p->x11_win = 0;
  }
  if (p->x11_cap_dpy) {
    p->p_XCloseDisplay(p->x11_cap_dpy);
    p->x11_cap_dpy = NULL;
  }
  if (p->x11_lib) {
    dlclose(p->x11_lib);
    p->x11_lib = NULL;
  }
}
#endif

int gpu_pipeline_create(gpu_pipeline_t **out_pipe, uint32_t width,
                        uint32_t height, uint32_t fps, uint32_t bitrate_bps,
                        bool show_window, gpu_pipeline_info_t *out_info) {
  (void)width; (void)height; (void)fps; (void)bitrate_bps; (void)show_window; (void)out_info;
  if (out_pipe) *out_pipe = NULL;
  port_log_err("gpu_pipeline_create: NVENC / D3D11 hardware pipeline requires Windows x64.\n");
  return -1;
}

int gpu_pipeline_create_pyrowave(gpu_pipeline_t **out_pipe, uint32_t width,
                                 uint32_t height, uint32_t fps,
                                 uint32_t bitrate_bps, bool show_window,
                                 gpu_pipeline_info_t *out_info) {
  (void)width; (void)height; (void)fps; (void)bitrate_bps; (void)show_window; (void)out_info;
  if (out_pipe) *out_pipe = NULL;
  port_log_err("gpu_pipeline_create_pyrowave: Pyrowave GPU bridge not available in portable C11 build.\n");
  return -1;
}

int gpu_pipeline_create_zcvid(gpu_pipeline_t **out_pipe, uint32_t width,
                                uint32_t height, uint32_t fps,
                                uint32_t threshold, uint32_t delta_step,
                                uint32_t gamma_q8, uint32_t neutral_y,
                                bool show_window, gpu_pipeline_info_t *out_info) {
  if (!out_pipe || width < 16U || height < 16U) return -1;
  *out_pipe = NULL;

  gpu_pipeline_t *p = (gpu_pipeline_t *)calloc(1, sizeof(*p));
  if (!p) return -1;

  p->width = width;
  p->height = height;
  p->fps = fps ? fps : 60U;
  p->bitrate_bps = 25000000U;
  p->threshold = threshold ? threshold : 6U;
  p->delta_step = delta_step ? delta_step : 4U;
  p->gamma_q8 = gamma_q8 ? gamma_q8 : 256U;
  p->neutral_y = neutral_y ? neutral_y : 128U;
  p->bw = (width + 3U) >> 2;
  p->bh = (height + 3U) >> 2;
  p->sbw = (p->bw + 1U) >> 1;
  p->sbh = (p->bh + 1U) >> 1;
  p->nblocks = p->bw * p->bh;
  p->tiles_x = (width + 31U) >> 5;
  p->tiles_y = (height + 31U) >> 5;
  p->num_tiles = p->tiles_x * p->tiles_y;

  p->show_window = show_window;
  p->force_synth = true;
  p->packed = true;
  p->entropy = true;
  p->transform = true;
  p->transform_inter = true;
  p->qpel = true;
  p->dither = false;
  p->kf_requested = false;
  p->enc_nsw_q8 = 4U << 8;
  p->dec_nsw_q8 = 4U << 8;

  size_t num_px = (size_t)width * (size_t)height;
  uint32_t max_rec = p->nblocks * 48U + num_px + 256U;
  p->raw_rec_cap_words = max_rec * 2U;
  p->enc_out_cap = 64U + (size_t)p->num_tiles * 16U + (size_t)max_rec * 12U;
  p->ec_scratch_cap = 64U + (size_t)max_rec * 12U;

  p->src_yuv = (uint8_t *)malloc(num_px * 3U);
  p->raw_rec_buf = (uint32_t *)malloc((size_t)p->raw_rec_cap_words * sizeof(uint32_t));
  p->enc_mv_buf = (int32_t *)calloc((size_t)p->num_tiles * 2U, sizeof(int32_t));
  p->dec_mv_buf = (int32_t *)calloc((size_t)p->num_tiles * 2U, sizeof(int32_t));
  p->sort_a = (uint64_t *)malloc((size_t)max_rec * sizeof(uint64_t));
  p->sort_b = (uint64_t *)malloc((size_t)max_rec * sizeof(uint64_t));
  p->sort_cap = max_rec;
  p->enc_out_buf = (uint8_t *)malloc(p->enc_out_cap);
  p->ec_scratch = (uint8_t *)malloc(p->ec_scratch_cap);
  p->dec_unpack_cap = p->raw_rec_cap_words;
  p->dec_unpack_buf = (uint32_t *)malloc((size_t)p->dec_unpack_cap * sizeof(uint32_t));
  p->enc_ec_line = (uint8_t *)malloc((size_t)width * 3U);
  p->enc_ec_lrow = (uint32_t *)calloc(width, sizeof(uint32_t));
  p->dec_ec_line = (uint8_t *)malloc((size_t)width * 3U);
  p->dec_ec_lrow = (uint32_t *)calloc(width, sizeof(uint32_t));

  if (!p->src_yuv || !p->raw_rec_buf || !p->enc_mv_buf || !p->dec_mv_buf ||
      !p->sort_a || !p->sort_b || !p->enc_out_buf || !p->ec_scratch ||
      !p->dec_unpack_buf || !p->enc_ec_line || !p->enc_ec_lrow ||
      !p->dec_ec_line || !p->dec_ec_lrow ||
      zcvid_gpu_dec_init(&p->enc_ref, width, height) != 0 ||
      zcvid_gpu_dec_init(&p->dec_state, width, height) != 0) {
    gpu_pipeline_destroy(p);
    return -1;
  }

  p->enc_initialized = true;
  p->dec_initialized = true;
  zcvid_gpu_dec_reset(&p->enc_ref, p->neutral_y, p->delta_step);
  zcvid_gpu_dec_reset(&p->dec_state, p->neutral_y, p->delta_step);

  if (out_info) {
    memset(out_info, 0, sizeof(*out_info));
    snprintf(out_info->adapter_name, sizeof(out_info->adapter_name),
             "Portable C11 CPU (OS/Arch Agnostic)");
    snprintf(out_info->encoder_name, sizeof(out_info->encoder_name),
             "Zcvid Unified Transform+CABAC (C11)");
    snprintf(out_info->decoder_name, sizeof(out_info->decoder_name),
             "Zcvid Portable C11 Decoder (zcvid_gpu_cpu_dec)");
    out_info->using_desktop_dup = false;
    out_info->tearing_supported = false;
    out_info->width = width;
    out_info->height = height;
  }

#ifdef ZC_PORTABLE_HAVE_X11
  if (show_window) {
    if (!port_x11_open_window(p)) {
      port_log_err("[DISPLAY] X11 window unavailable (no DISPLAY / libX11) — decoding headless.\n");
    }
  } else {
    if (port_x11_open_capture(p)) {
      p->force_synth = false;
      if (out_info) {
        out_info->using_desktop_dup = true;
        snprintf(out_info->adapter_name, sizeof(out_info->adapter_name),
                 "Linux X11 Desktop Capture (%ux%u -> %ux%u)",
                 p->x11_cap_w, p->x11_cap_h, width, height);
      }
    } else {
      port_log_err("[CAPTURE] X11 desktop capture unavailable (Wayland/headless) — falling back to synthetic test pattern.\n");
    }
  }
#endif

  *out_pipe = p;
  return 0;
}

void gpu_pipeline_destroy(gpu_pipeline_t *p) {
  if (!p) return;
#ifdef ZC_PORTABLE_HAVE_X11
  port_x11_cleanup(p);
#endif
  zcvid_gpu_dec_destroy(&p->enc_ref);
  zcvid_gpu_dec_destroy(&p->dec_state);
  free(p->src_yuv);
  free(p->raw_rec_buf);
  free(p->enc_mv_buf);
  free(p->dec_mv_buf);
  free(p->sort_a);
  free(p->sort_b);
  free(p->enc_out_buf);
  free(p->ec_scratch);
  free(p->dec_unpack_buf);
  free(p->enc_ec_line);
  free(p->enc_ec_lrow);
  free(p->dec_ec_line);
  free(p->dec_ec_lrow);
  free(p);
}

void gpu_pipeline_set_bitrate(gpu_pipeline_t *p, uint32_t bitrate_bps) {
  if (p && bitrate_bps > 0U) p->bitrate_bps = bitrate_bps;
}

void gpu_pipeline_force_synth(gpu_pipeline_t *p, bool enable) {
  if (p) p->force_synth = enable;
}

void gpu_pipeline_zcvid_request_keyframe(gpu_pipeline_t *p) {
  if (p) p->kf_requested = true;
}

void gpu_pipeline_zcvid_set_packed(gpu_pipeline_t *p, bool enable) {
  if (p) p->packed = enable;
}

void gpu_pipeline_zcvid_set_entropy(gpu_pipeline_t *p, bool enable) {
  if (p) p->entropy = enable;
}

void gpu_pipeline_zcvid_set_transform(gpu_pipeline_t *p, bool enable) {
  if (p) p->transform = enable;
}

void gpu_pipeline_zcvid_set_transform_inter(gpu_pipeline_t *p, bool enable) {
  if (p) p->transform_inter = enable;
}

void gpu_pipeline_zcvid_set_qpel(gpu_pipeline_t *p, bool enable) {
  if (p) p->qpel = enable;
}

void gpu_pipeline_zcvid_set_dither(gpu_pipeline_t *p, bool enable) {
  if (p) p->dither = enable;
}

void gpu_pipeline_zcvid_hint_stats(gpu_pipeline_t *p, uint64_t *frames, uint64_t *move_rects,
                                     uint64_t *moved_tiles, uint64_t *static_tiles) {
  if (!p) return;
  if (frames) *frames = p->hint_frames;
  if (move_rects) *move_rects = p->hint_move_rects;
  if (moved_tiles) *moved_tiles = p->hint_moved_tiles;
  if (static_tiles) *static_tiles = p->hint_static_tiles;
}

uint32_t gpu_pipeline_zcvid_entropy_budget(gpu_pipeline_t *p) {
  if (!p) return 0U;
  uint32_t fps = p->fps ? p->fps : 60U;
  uint32_t pct = (fps <= 60U) ? 35U : (fps <= 144U) ? 25U : 18U;
  uint64_t budget_ns = (1000000000ULL / fps) * pct / 100U;
  uint64_t words = (budget_ns << 8) / (p->dec_nsw_q8 ? p->dec_nsw_q8 : (4U << 8));
  uint64_t floor_w = (fps >= 240U) ? 2048U : 8192U;
  if (words < floor_w) words = floor_w;
  if (words > 0x07FFFFFEU) words = 0x07FFFFFEU;
  return (uint32_t)words & ~1U;
}

void gpu_pipeline_zcvid_set_entropy_limit(gpu_pipeline_t *p, uint32_t record_words) {
  if (p) p->entropy_limit_words = record_words & ~1U;
}

bool gpu_pipeline_pump_events(gpu_pipeline_t *p) {
  if (!p) return false;
#ifdef ZC_PORTABLE_HAVE_X11
  if (p->x11_win_open && p->x11_dpy) {
    while (p->p_XPending(p->x11_dpy) > 0) {
      XEvent ev;
      p->p_XNextEvent(p->x11_dpy, &ev);
      if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == p->x11_wm_del) {
        p->x11_closed = true;
      } else if (ev.type == KeyPress && p->p_XLookupKeysym(&ev.xkey, 0) == XK_Escape) {
        p->x11_closed = true;
      } else if (ev.type == ConfigureNotify) {
        uint32_t nw = (ev.xconfigure.width > 64) ? (uint32_t)ev.xconfigure.width : 64U;
        uint32_t nh = (ev.xconfigure.height > 64) ? (uint32_t)ev.xconfigure.height : 64U;
        p->x11_win_w = nw;
        p->x11_win_h = nh;
      }
    }
    return !p->x11_closed;
  }
#endif
  return true;
}

int gpu_pipeline_capture_and_encode(gpu_pipeline_t *p, uint32_t frame_idx,
                                    const uint8_t **out_bitstream,
                                    size_t *out_len, bool *out_keyframe,
                                    double *out_cap_us, double *out_enc_us) {
  if (!p || !out_bitstream || !out_len || !p->enc_initialized) return -1;

  uint64_t tc0 = qpc_now_us();
  bool captured = false;
#ifdef ZC_PORTABLE_HAVE_X11
  if (!p->force_synth && p->x11_cap_ok) {
    captured = port_x11_capture_yuv(p);
    if (!captured && !p->x11_cap_failed) {
      p->x11_cap_failed = true;
      p->x11_cap_ok = false;
      port_log_err("[CAPTURE] X11 XGetImage failed — falling back to synthetic pattern.\n");
    }
  }
#endif
  if (!captured) {
    port_generate_synth_yuv(p->src_yuv, p->width, p->height, frame_idx);
  }
  uint64_t tc1 = qpc_now_us();
  if (out_cap_us) *out_cap_us = (double)(tc1 - tc0);

  uint64_t te0 = qpc_now_us();
  bool is_kf = (frame_idx == 0U) || p->kf_requested;
  p->kf_requested = false;

  const uint32_t w = p->width, h = p->height;
  zcvid_gpu_dec_params_t dp;
  dp.width = w;
  dp.height = h;
  dp.delta_min = p->delta_step;
  dp.delta_max = 64U;
  dp.gamma_q8 = p->gamma_q8;
  dp.neutral_y = p->neutral_y;
  dp.mv_qpel = p->qpel ? 1U : 0U;
  dp.post_thr = 0U;

  uint64_t mv_moved = 0ULL, mv_stat = 0ULL;
  if (is_kf) {
    memset(p->enc_mv_buf, 0, (size_t)p->num_tiles * 2U * sizeof(int32_t));
    mv_stat = p->num_tiles;
  } else {
    port_estimate_mvs(p, p->src_yuv, &p->enc_ref, p->enc_mv_buf, p->qpel, &mv_moved, &mv_stat);
  }
  p->hint_frames++;
  p->hint_moved_tiles += mv_moved;
  p->hint_static_tiles += mv_stat;
  if (mv_moved > 0ULL) p->hint_move_rects++;

  port_motion_decay_pass1(&p->enc_ref, &dp, (const zcvid_mv_t *)p->enc_mv_buf);

  double bpp = (double)p->bitrate_bps / ((double)p->fps * (double)w * (double)h);
  int32_t base_qp = 18;
  if (bpp >= 1.5) base_qp = 12;
  else if (bpp >= 0.7) base_qp = 16;
  else if (bpp >= 0.3) base_qp = 20;
  else if (bpp >= 0.15) base_qp = 26;
  else base_qp = 32;
  if (is_kf && base_qp > 18) base_qp = 18;
  base_qp = zcvid_dec_clamp_i32(base_qp + p->qp_offset, 10, 44);

  uint32_t rec_words = 0U;
  bool use_cls8 = p->transform;
  const uint32_t max_words_budget = p->raw_rec_cap_words;

  if (use_cls8) {
    for (uint32_t sby = 0U; sby < p->sbh; ++sby) {
      for (uint32_t sbx = 0U; sbx < p->sbw; ++sbx) {
        for (uint32_t quad = 0U; quad < 4U; ++quad) {
          uint32_t bx = sbx * 2U + (quad & 1U);
          uint32_t by = sby * 2U + (quad >> 1);
          if (bx >= p->bw || by >= p->bh) continue;
          uint32_t x0 = bx * 4U, y0 = by * 4U;
          if (x0 + 4U > w || y0 + 4U > h) continue;
          uint32_t b = by * p->bw + bx;

          int32_t intra_res[3][16];
          int32_t inter_res[3][16];
          uint32_t intra_sad = 0U, inter_sad = 0U;
          for (uint32_t j = 0U; j < 16U; ++j) {
            uint32_t px = x0 + (j & 3U);
            uint32_t py = y0 + (j >> 2);
            size_t idx = (size_t)py * w + px;
            int32_t sy  = (int32_t)p->src_yuv[idx * 3U + 0U];
            int32_t scb = (int32_t)p->src_yuv[idx * 3U + 1U];
            int32_t scr = (int32_t)p->src_yuv[idx * 3U + 2U];

            uint32_t pw = p->enc_ref.cur_yuvd[idx];
            int32_t ry  = (int32_t)(pw & 0xFFU);
            int32_t rcb = (int32_t)((pw >> 8) & 0xFFU);
            int32_t rcr = (int32_t)((pw >> 16) & 0xFFU);

            intra_res[0][j] = sy - 128;
            intra_res[1][j] = scb - 128;
            intra_res[2][j] = scr - 128;
            inter_res[0][j] = sy - ry;
            inter_res[1][j] = scb - rcb;
            inter_res[2][j] = scr - rcr;

            intra_sad += (uint32_t)abs(intra_res[0][j]);
            inter_sad += (uint32_t)abs(inter_res[0][j]) +
                         ((uint32_t)abs(inter_res[1][j]) >> 1) +
                         ((uint32_t)abs(inter_res[2][j]) >> 1);
          }

          if (!is_kf && !p->transform_inter) {
            /* Inter transform disabled: skip unless significant change -> intra */
            if (inter_sad < p->threshold * 16U) continue;
          }

          bool inter_mode = (!is_kf) && p->transform_inter && (inter_sad <= intra_sad + 32U);
          if (inter_mode && inter_sad < (p->threshold * 6U)) {
            continue;
          }

          uint32_t qp = (uint32_t)base_qp;
          int32_t fwd_w[3][16];
          int16_t lv[3][16];
          for (uint32_t pl = 0U; pl < 3U; ++pl) {
            port_tq_forward_4x4(inter_mode ? inter_res[pl] : intra_res[pl], fwd_w[pl]);
          }

          uint32_t nz_cnt = 0U;
          for (;;) {
            nz_cnt = 0U;
            bool overflow = false;
            for (uint32_t pl = 0U; pl < 3U; ++pl) {
              uint32_t pqp = (pl > 0U && qp + 2U <= 51U) ? (qp + 2U) : qp;
              port_tq_quant_4x4(fwd_w[pl], pqp, inter_mode, lv[pl]);
              for (uint32_t k = 0U; k < 16U; ++k) {
                if (lv[pl][k] < -255 || lv[pl][k] > 255) overflow = true;
                if (lv[pl][k] != 0) nz_cnt++;
              }
            }
            if (!overflow || qp >= 51U) break;
            qp += 3U;
            if (qp > 51U) qp = 51U;
          }
          if (nz_cnt == 0U) {
            if (!is_kf) continue;
            /* On keyframe flat neutral block: omit zero block since canvas starts at 128,128,128 */
            continue;
          }
          if (rec_words + nz_cnt * 2U > max_words_budget) break;

          uint32_t pred = inter_mode ? 1U : 0U;
          for (uint32_t pl = 0U; pl < 3U; ++pl) {
            uint32_t pqp = (pl > 0U && qp + 2U <= 51U) ? (qp + 2U) : qp;
            for (uint32_t k = 0U; k < 16U; ++k) {
              int32_t val = zcvid_dec_clamp_i32(lv[pl][k], -255, 255);
              if (val == 0) continue;
              p->raw_rec_buf[rec_words++] = 0xF0000000U | (b * 48U + pl * 16U + k);
              p->raw_rec_buf[rec_words++] =
                  ((uint32_t)val & 0xFFFFU) | (pqp << 16) | (pred << 24);
            }
          }
        }
      }
    }
  } else {
    /* Fallback intra 2-px path when transform is disabled */
    const int32_t thr = (int32_t)p->threshold;
    for (uint32_t y = 0U; y < h; ++y) {
      bool refresh_row = is_kf || ((y & 3U) == (frame_idx & 3U));
      for (uint32_t x = 0U; x + 1U < w; x += 2U) {
        if (rec_words + 2U > max_words_budget) break;
        size_t id = (size_t)y * w + x;
        uint32_t y0  = p->src_yuv[id * 3U + 0U];
        uint32_t cb0 = p->src_yuv[id * 3U + 1U];
        uint32_t cr0 = p->src_yuv[id * 3U + 2U];
        uint32_t y1  = p->src_yuv[(id + 1U) * 3U + 0U];
        if (!refresh_row) {
          uint32_t pw0 = p->enc_ref.cur_yuvd[id];
          uint32_t pw1 = p->enc_ref.cur_yuvd[id + 1U];
          int32_t dy0 = abs((int32_t)y0 - (int32_t)(pw0 & 0xFFU));
          int32_t dy1 = abs((int32_t)y1 - (int32_t)(pw1 & 0xFFU));
          int32_t dcb = abs((int32_t)cb0 - (int32_t)((pw0 >> 8) & 0xFFU));
          int32_t dcr = abs((int32_t)cr0 - (int32_t)((pw0 >> 16) & 0xFFU));
          if (dy0 <= thr && dy1 <= thr && dcb <= thr && dcr <= thr) continue;
        }
        p->raw_rec_buf[rec_words++] = 0xC0000000U | ((uint32_t)id & 0x1FFFFFFFU);
        p->raw_rec_buf[rec_words++] = y0 | (cb0 << 8) | (y1 << 16) | (cr0 << 24);
      }
    }
  }

  /* Update encoder closed-loop reference via zcvid_gpu_dec_decode_frame for exact parity */
  zcvid_gpu_dec_decode_frame(&p->enc_ref, &dp, (const zcvid_mv_t *)p->enc_mv_buf,
                             p->raw_rec_buf, rec_words);

  /* Serialize header + motion vectors + records */
  uint32_t hdr_word = rec_words & ZCVID_RECORD_WORDS_MASK;

  uint32_t mv_bytes = 0U;
  int32_t max_mv = 0;
  for (uint32_t i = 0U; i < p->num_tiles; ++i) {
    int32_t ax = (p->enc_mv_buf[i * 2U] < 0) ? -p->enc_mv_buf[i * 2U] : p->enc_mv_buf[i * 2U];
    int32_t ay = (p->enc_mv_buf[i * 2U + 1U] < 0) ? -p->enc_mv_buf[i * 2U + 1U] : p->enc_mv_buf[i * 2U + 1U];
    int32_t m = (ax > ay) ? ax : ay;
    if (m > max_mv) max_mv = m;
  }
  bool any_nz = (max_mv != 0);
  bool wide_mv = (max_mv > 127);
  if (p->packed) {
    hdr_word |= ZCVID_PACKED_FLAG;
    if (use_cls8) hdr_word |= ZCVID_V2_FLAG;
    if (any_nz) {
      mv_bytes = zcvid_pack_mvs(p->enc_mv_buf, p->num_tiles, p->enc_out_buf + 8U, wide_mv);
      if (mv_bytes > 0U && wide_mv) hdr_word |= ZCVID_WIDE_MV_FLAG;
      if (p->qpel && mv_bytes > 0U) p->enc_out_buf[8U + mv_bytes++] = 0x51U;
      if (p->entropy && mv_bytes > 16U && max_mv <= 512) {
        uint8_t *ec_tmp = p->enc_out_buf + 8U + mv_bytes;
        size_t ec_cap = (p->enc_out_cap > 8U + mv_bytes) ? (p->enc_out_cap - 8U - mv_bytes) : 0U;
        if (ec_cap > mv_bytes) ec_cap = mv_bytes;
        uint32_t ec_bytes = zcvid_ec_pack_mvs(p->enc_mv_buf, p->tiles_x, p->tiles_y,
                                             p->qpel, ec_tmp, ec_cap);
        if (ec_bytes >= 2U && ec_bytes < mv_bytes) {
          memmove(p->enc_out_buf + 8U, ec_tmp, ec_bytes);
          mv_bytes = ec_bytes;
          hdr_word |= ZCVID_EC_MV_FLAG | ZCVID_WIDE_MV_FLAG;
        }
      }
    }
  } else if (any_nz) {
    if (wide_mv) {
      int16_t *dst = (int16_t *)(p->enc_out_buf + 8U);
      for (uint32_t i = 0U; i < p->num_tiles * 2U; ++i) dst[i] = (int16_t)p->enc_mv_buf[i];
      mv_bytes = p->num_tiles * 4U;
    } else {
      int8_t *dst = (int8_t *)(p->enc_out_buf + 8U);
      for (uint32_t i = 0U; i < p->num_tiles * 2U; ++i) dst[i] = (int8_t)p->enc_mv_buf[i];
      mv_bytes = p->num_tiles * 2U;
    }
    if (p->qpel) p->enc_out_buf[8U + mv_bytes++] = 0x51U;
  }

  size_t out_bytes = 8U + mv_bytes;
  if (rec_words > 0U) {
    size_t rec_bytes = (size_t)rec_words * sizeof(uint32_t);
    uint32_t num_records = rec_words / 2U;
    uint32_t ncls = use_cls8 ? 9U : 8U;
    bool coded = false;
    size_t packed = 0U;

    if (p->packed && out_bytes + zcvid_pack_bound(num_records) <= p->enc_out_cap) {
      bool use_sb_tq = use_cls8 && p->entropy;
      uint32_t kept = port_sort_records(p, p->raw_rec_buf, num_records, use_sb_tq);
      num_records = kept;
      hdr_word = (hdr_word & ~ZCVID_RECORD_WORDS_MASK) | ((num_records * 2U) & ZCVID_RECORD_WORDS_MASK);

      if (p->entropy && num_records > 0U) {
        uint32_t budget_words = num_records * 2U;
        if (p->entropy_limit_words != 0U && budget_words > p->entropy_limit_words) {
          budget_words = p->entropy_limit_words;
        }
        uint32_t ec_records = budget_words / 2U;
        if (ec_records > num_records) ec_records = num_records;
        uint32_t plain_records = num_records - ec_records;
        if (plain_records == 0U && num_records > 0U) {
          size_t chunk_len = port_ec_put_chunk(p, p->raw_rec_buf, p->sort_a,
                                               num_records, ncls,
                                               p->ec_scratch, p->ec_scratch_cap);
          if (chunk_len > 0U && out_bytes + 9U + chunk_len <= p->enc_out_cap) {
            uint8_t *o = p->enc_out_buf + out_bytes;
            uint32_t cw = num_records * 2U;
            uint32_t cb = (uint32_t)chunk_len;
            o[0] = 1U;
            memcpy(o + 1U, &cw, 4);
            memcpy(o + 5U, &cb, 4);
            memcpy(o + 9U, p->ec_scratch, chunk_len);
            packed = 9U + chunk_len;
            coded = true;
          }
        }
      }
      if (!coded) {
        if (use_sb_tq) {
          /* Re-sort in raster order for plain packing if superblock key was active */
          num_records = port_sort_records(p, p->raw_rec_buf, rec_words / 2U, false);
        }
        packed = port_pack_sorted(p->raw_rec_buf, p->sort_a, num_records, ncls,
                                  p->enc_out_buf + out_bytes);
      }
    }

    if (packed > 0U) {
      out_bytes += packed;
      if (coded) {
        hdr_word |= ZCVID_EC_FLAG;
        if (use_cls8) hdr_word |= ZCVID_TQ_V3_FLAG;
      }
    } else {
      if (p->packed) hdr_word |= ZCVID_RAW_RECORDS_FLAG;
      memcpy(p->enc_out_buf + out_bytes, p->raw_rec_buf, rec_bytes);
      out_bytes += rec_bytes;
    }
  }

  memcpy(p->enc_out_buf + 0U, &hdr_word, sizeof(uint32_t));
  memcpy(p->enc_out_buf + 4U, &mv_bytes, sizeof(uint32_t));

  /* Mild integral rate control adjustment */
  double target_bytes = ((double)p->bitrate_bps / 8.0) / (double)(p->fps ? p->fps : 60U);
  if (!is_kf && target_bytes > 1000.0) {
    if ((double)out_bytes > target_bytes * 1.35 && p->qp_offset < 12) p->qp_offset++;
    else if ((double)out_bytes < target_bytes * 0.65 && p->qp_offset > -6) p->qp_offset--;
  }

  uint64_t te1 = qpc_now_us();
  if (out_enc_us) *out_enc_us = (double)(te1 - te0);
  if (out_keyframe) *out_keyframe = is_kf;
  *out_bitstream = p->enc_out_buf;
  *out_len = out_bytes;
  return 0;
}

int gpu_pipeline_decode_and_present(gpu_pipeline_t *p,
                                    const uint8_t *bitstream, size_t len,
                                    double *out_dec_us, double *out_present_us) {
  if (!p || !bitstream || len < 8U || !p->dec_initialized) return -1;

  uint64_t t0 = qpc_now_us();
  uint32_t hdr_word = 0U, mv_bytes = 0U;
  memcpy(&hdr_word, bitstream + 0U, sizeof(uint32_t));
  memcpy(&mv_bytes, bitstream + 4U, sizeof(uint32_t));

  bool packed = (hdr_word & ZCVID_PACKED_FLAG) != 0U;
  bool packed_records = packed && ((hdr_word & ZCVID_RAW_RECORDS_FLAG) == 0U);
  uint32_t ncls = (packed_records && ((hdr_word & ZCVID_V2_FLAG) != 0U)) ? 9U : 8U;
  uint32_t record_words = packed ? (hdr_word & ZCVID_RECORD_WORDS_MASK) : hdr_word;
  if (8ULL + (uint64_t)mv_bytes > (uint64_t)len) {
    fprintf(stderr, "decode: bad mv_bytes=%u > len=%zu\n", mv_bytes, len);
    return -1;
  }
  if (!packed_records && 8ULL + (uint64_t)mv_bytes + (uint64_t)record_words * 4ULL > (uint64_t)len) {
    fprintf(stderr, "decode: raw trunc mv_bytes=%u rec_words=%u len=%zu\n", mv_bytes, record_words, len);
    return -1;
  }

  bool mv_qpel = false;
  const uint32_t num_tiles = p->num_tiles;
  if (mv_bytes == 0U) {
    memset(p->dec_mv_buf, 0, (size_t)num_tiles * 2U * sizeof(int32_t));
    p->dec_mvs_zero = true;
  } else {
    bool ec_mv = packed && ((hdr_word & ZCVID_EC_MV_FLAG) != 0U);
    bool wide_mv = packed ? ((hdr_word & ZCVID_WIDE_MV_FLAG) != 0U)
                          : (mv_bytes == num_tiles * 4U || mv_bytes == num_tiles * 4U + 1U);
    if (ec_mv) {
      if (!zcvid_ec_unpack_mvs(bitstream + 8U, mv_bytes, p->dec_mv_buf,
                              p->tiles_x, p->tiles_y, &mv_qpel)) {
        fprintf(stderr, "decode: zcvid_ec_unpack_mvs failed (mv_bytes=%u tiles=%ux%u len=%zu)\n",
                mv_bytes, p->tiles_x, p->tiles_y, len);
        return -1;
      }
    } else {
      uint32_t run_step = wide_mv ? 5U : 3U;
      mv_qpel = packed ? (mv_bytes % run_step == 1U)
                       : (mv_bytes == num_tiles * (wide_mv ? 4U : 2U) + 1U);
      if (mv_qpel && bitstream[8U + mv_bytes - 1U] != 0x51U) {
        fprintf(stderr, "decode: mv_qpel marker mismatch (got=0x%02x mv_bytes=%u)\n",
                (unsigned)bitstream[8U + mv_bytes - 1U], mv_bytes);
        return -1;
      }
      if (packed) {
        uint32_t payload_len = mv_qpel ? (mv_bytes - 1U) : mv_bytes;
        if (!zcvid_unpack_mvs(bitstream + 8U, payload_len, p->dec_mv_buf, num_tiles, wide_mv)) {
          fprintf(stderr, "decode: zcvid_unpack_mvs failed (payload=%u num_tiles=%u wide=%d)\n",
                  payload_len, num_tiles, wide_mv ? 1 : 0);
          return -1;
        }
      } else if (wide_mv) {
        const int16_t *src_mv = (const int16_t *)(bitstream + 8U);
        for (uint32_t i = 0U; i < num_tiles; ++i) {
          p->dec_mv_buf[i * 2U + 0U] = (int32_t)src_mv[i * 2U + 0U];
          p->dec_mv_buf[i * 2U + 1U] = (int32_t)src_mv[i * 2U + 1U];
        }
      } else if (mv_bytes == num_tiles * 2U || mv_qpel) {
        const int8_t *src_mv = (const int8_t *)(bitstream + 8U);
        for (uint32_t i = 0U; i < num_tiles; ++i) {
          p->dec_mv_buf[i * 2U + 0U] = (int32_t)src_mv[i * 2U + 0U];
          p->dec_mv_buf[i * 2U + 1U] = (int32_t)src_mv[i * 2U + 1U];
        }
      } else if (mv_bytes == num_tiles * 8U) {
        memcpy(p->dec_mv_buf, bitstream + 8U, (size_t)num_tiles * 8U);
      } else {
        fprintf(stderr, "decode: unexpected mv_bytes=%u (num_tiles=%u)\n", mv_bytes, num_tiles);
        return -1;
      }
    }
    p->dec_mvs_zero = false;
  }

  const uint32_t *rec_src = NULL;
  uint32_t got_words = record_words;
  if (record_words > 0U) {
    if (packed_records) {
      if (record_words > p->dec_unpack_cap) {
        uint32_t *nb = (uint32_t *)realloc(p->dec_unpack_buf, (size_t)record_words * sizeof(uint32_t));
        if (!nb) return -1;
        p->dec_unpack_buf = nb;
        p->dec_unpack_cap = record_words;
      }
      if ((hdr_word & ZCVID_EC_FLAG) != 0U) {
        if (ncls >= 9U && (hdr_word & ZCVID_TQ_V3_FLAG) == 0U) {
          port_log_err("decode: missing ZCVID_TQ_V3_FLAG for cls8 EC\n");
          return -1;
        }
        got_words = port_ec_unpack_records(p, bitstream + 8U + mv_bytes,
                                           len - 8U - mv_bytes, p->dec_unpack_buf,
                                           record_words, ncls);
      } else {
        got_words = port_unpack_records(bitstream + 8U + mv_bytes, len - 8U - mv_bytes,
                                        p->dec_unpack_buf, record_words, ncls);
      }
      if (got_words == UINT32_MAX) {
        port_log_err("decode: unpack records failed\n");
        return -1;
      }
      rec_src = p->dec_unpack_buf;
    } else {
      rec_src = (const uint32_t *)(bitstream + 8U + mv_bytes);
    }
  }

  zcvid_gpu_dec_params_t dp;
  dp.width = p->width;
  dp.height = p->height;
  dp.delta_min = p->delta_step;
  dp.delta_max = 64U;
  dp.gamma_q8 = p->gamma_q8;
  dp.neutral_y = p->neutral_y;
  dp.mv_qpel = mv_qpel ? 1U : 0U;
  dp.post_thr = 0U;

  int rc = zcvid_gpu_dec_decode_frame(&p->dec_state, &dp,
                                      p->dec_mvs_zero ? NULL : (const zcvid_mv_t *)p->dec_mv_buf,
                                      rec_src, got_words);
  double present_us = 0.0;
#ifdef ZC_PORTABLE_HAVE_X11
  if (rc == 0 && p->x11_win_open && !p->x11_closed && p->disp_rgba) {
    uint64_t tp0 = qpc_now_us();
    if (p->x11_img && ((uint32_t)p->x11_img->width != p->x11_win_w ||
                       (uint32_t)p->x11_img->height != p->x11_win_h)) {
      (void)port_x11_alloc_image(p, p->x11_win_w, p->x11_win_h);
    }
    if (p->x11_img && p->disp_bgrx) {
      zcvid_gpu_dec_to_rgba(&p->dec_state, &dp, p->disp_rgba);
      port_rgba_to_bgrx(p->disp_rgba, p->width, p->height,
                         p->disp_bgrx, p->x11_win_w, p->x11_win_h);
      p->p_XPutImage(p->x11_dpy, p->x11_win, p->x11_gc, p->x11_img,
                     0, 0, 0, 0, p->x11_win_w, p->x11_win_h);
      p->p_XFlush(p->x11_dpy);
    }
    present_us = (double)(qpc_now_us() - tp0);
  }
#endif
  uint64_t t1 = qpc_now_us();
  if (out_dec_us) *out_dec_us = (double)(t1 - t0);
  if (out_present_us) *out_present_us = present_us;
  return rc;
}

int gpu_pipeline_zcvid_compute_psnr(gpu_pipeline_t *p, double *out_psnr_db) {
  if (!p || !out_psnr_db || !p->enc_initialized || !p->dec_initialized) return -1;
  size_t num_px = (size_t)p->width * (size_t)p->height;
  if (num_px == 0U) return -1;
  double mse = 0.0;
  for (size_t i = 0U; i < num_px; ++i) {
    int32_t orig_y = (int32_t)p->src_yuv[i * 3U];
    int32_t dec_y = (int32_t)(p->dec_state.prev_yuvd[i] & 0xFFU);
    double d = (double)(orig_y - dec_y);
    mse += d * d;
  }
  mse /= (double)num_px;
  if (mse <= 1e-12) {
    *out_psnr_db = 99.0;
  } else {
    *out_psnr_db = 10.0 * log10((255.0 * 255.0) / mse);
  }
  return 0;
}

int gpu_pipeline_zcvid_ref_mismatch(gpu_pipeline_t *p, uint64_t *out_mismatch_px) {
  if (!p || !out_mismatch_px || !p->enc_initialized || !p->dec_initialized) return -1;
  size_t num_px = (size_t)p->width * (size_t)p->height;
  uint64_t diff = 0ULL;
  for (size_t i = 0U; i < num_px; ++i) {
    if (p->enc_ref.prev_yuvd[i] != p->dec_state.prev_yuvd[i]) diff++;
  }
  *out_mismatch_px = diff;
  return 0;
}
