#ifdef _WIN32
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <xinput.h>
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <linux/joystick.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#endif
#endif

#include "zc_periph.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Endian & Wire Pack/Unpack Helpers
 * ========================================================================= */

static inline void wr_u16_le(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static inline void wr_u32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
  p[2] = (uint8_t)((v >> 16) & 0xFFU);
  p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static inline uint16_t rd_u16_le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int zc_input_pack(const zc_input_event_t *ev, uint8_t out[ZC_INPUT_PKT_SIZE]) {
  if (!ev || !out) return -1;
  memcpy(out, ZC_INPUT_MAGIC, 4);
  out[4] = ev->kind;
  out[5] = ev->flags;
  wr_u16_le(out + 6, ev->code);
  wr_u32_le(out + 8, (uint32_t)ev->x);
  wr_u32_le(out + 12, (uint32_t)ev->y);
  wr_u32_le(out + 16, ev->seq);
  return (int)ZC_INPUT_PKT_SIZE;
}

int zc_input_unpack(const uint8_t *buf, size_t len, zc_input_event_t *out) {
  if (!buf || !out || len < ZC_INPUT_PKT_SIZE) return -1;
  if (memcmp(buf, ZC_INPUT_MAGIC, 4) != 0) return -1;
  out->kind = buf[4];
  out->flags = buf[5];
  out->code = rd_u16_le(buf + 6);
  out->x = (int32_t)rd_u32_le(buf + 8);
  out->y = (int32_t)rd_u32_le(buf + 12);
  out->seq = rd_u32_le(buf + 16);
  return 0;
}

int zc_gamepad_pack(const zc_gamepad_state_t *gp, uint8_t out[ZC_GAMEPAD_PKT_SIZE]) {
  if (!gp || !out) return -1;
  memcpy(out, ZC_GAMEPAD_MAGIC, 4);
  out[4] = gp->index;
  out[5] = gp->connected;
  wr_u16_le(out + 6, gp->buttons);
  out[8] = gp->lt;
  out[9] = gp->rt;
  wr_u16_le(out + 10, (uint16_t)gp->lx);
  wr_u16_le(out + 12, (uint16_t)gp->ly);
  wr_u16_le(out + 14, (uint16_t)gp->rx);
  wr_u16_le(out + 16, (uint16_t)gp->ry);
  wr_u32_le(out + 20, gp->seq);
  return (int)ZC_GAMEPAD_PKT_SIZE;
}

int zc_gamepad_unpack(const uint8_t *buf, size_t len, zc_gamepad_state_t *out) {
  if (!buf || !out || len < ZC_GAMEPAD_PKT_SIZE) return -1;
  if (memcmp(buf, ZC_GAMEPAD_MAGIC, 4) != 0) return -1;
  out->index = buf[4];
  out->connected = buf[5];
  out->buttons = rd_u16_le(buf + 6);
  out->lt = buf[8];
  out->rt = buf[9];
  out->lx = (int16_t)rd_u16_le(buf + 10);
  out->ly = (int16_t)rd_u16_le(buf + 12);
  out->rx = (int16_t)rd_u16_le(buf + 14);
  out->ry = (int16_t)rd_u16_le(buf + 16);
  out->seq = rd_u32_le(buf + 20);
  return 0;
}

/* =========================================================================
 * Scancode Translation Table (Linux evdev <-> AT Set 1)
 * ========================================================================= */

typedef struct {
  uint16_t evdev;
  uint8_t set1;
  uint8_t ext;
} zc_scancode_map_t;

static const zc_scancode_map_t k_scancode_table[] = {
    {1, 0x01, 0},   {2, 0x02, 0},   {3, 0x03, 0},   {4, 0x04, 0},
    {5, 0x05, 0},   {6, 0x06, 0},   {7, 0x07, 0},   {8, 0x08, 0},
    {9, 0x09, 0},   {10, 0x0A, 0},  {11, 0x0B, 0},  {12, 0x0C, 0},
    {13, 0x0D, 0},  {14, 0x0E, 0},  {15, 0x0F, 0},  {16, 0x10, 0},
    {17, 0x11, 0},  {18, 0x12, 0},  {19, 0x13, 0},  {20, 0x14, 0},
    {21, 0x15, 0},  {22, 0x16, 0},  {23, 0x17, 0},  {24, 0x18, 0},
    {25, 0x19, 0},  {26, 0x1A, 0},  {27, 0x1B, 0},  {28, 0x1C, 0},
    {29, 0x1D, 0},  {30, 0x1E, 0},  {31, 0x1F, 0},  {32, 0x20, 0},
    {33, 0x21, 0},  {34, 0x22, 0},  {35, 0x23, 0},  {36, 0x24, 0},
    {37, 0x25, 0},  {38, 0x26, 0},  {39, 0x27, 0},  {40, 0x28, 0},
    {41, 0x29, 0},  {42, 0x2A, 0},  {43, 0x2B, 0},  {44, 0x2C, 0},
    {45, 0x2D, 0},  {46, 0x2E, 0},  {47, 0x2F, 0},  {48, 0x30, 0},
    {49, 0x31, 0},  {50, 0x32, 0},  {51, 0x33, 0},  {52, 0x34, 0},
    {53, 0x35, 0},  {54, 0x36, 0},  {55, 0x37, 0},  {56, 0x38, 0},
    {57, 0x39, 0},  {58, 0x3A, 0},  {59, 0x3B, 0},  {60, 0x3C, 0},
    {61, 0x3D, 0},  {62, 0x3E, 0},  {63, 0x3F, 0},  {64, 0x40, 0},
    {65, 0x41, 0},  {66, 0x42, 0},  {67, 0x43, 0},  {68, 0x44, 0},
    {69, 0x45, 0},  {70, 0x46, 0},  {71, 0x47, 0},  {72, 0x48, 0},
    {73, 0x49, 0},  {74, 0x4A, 0},  {75, 0x4B, 0},  {76, 0x4C, 0},
    {77, 0x4D, 0},  {78, 0x4E, 0},  {79, 0x4F, 0},  {80, 0x50, 0},
    {81, 0x51, 0},  {82, 0x52, 0},  {83, 0x53, 0},  {86, 0x56, 0},
    {87, 0x57, 0},  {88, 0x58, 0},
    /* Extended keys (0xE0 prefix in AT Set 1) */
    {96, 0x1C, 1},  /* KPENTER */
    {97, 0x1D, 1},  /* RIGHTCTRL */
    {98, 0x35, 1},  /* KPSLASH */
    {100, 0x38, 1}, /* RIGHTALT */
    {102, 0x47, 1}, /* HOME */
    {103, 0x48, 1}, /* UP */
    {104, 0x49, 1}, /* PAGEUP */
    {105, 0x4B, 1}, /* LEFT */
    {106, 0x4D, 1}, /* RIGHT */
    {107, 0x4F, 1}, /* END */
    {108, 0x50, 1}, /* DOWN */
    {109, 0x51, 1}, /* PAGEDOWN */
    {110, 0x52, 1}, /* INSERT */
    {111, 0x53, 1}, /* DELETE */
    {125, 0x5B, 1}, /* LEFTMETA */
    {126, 0x5C, 1}, /* RIGHTMETA */
    {127, 0x5D, 1}  /* COMPOSE / MENU */
};

bool zc_scancode_evdev_to_set1(uint16_t evdev, uint16_t *out_set1, bool *out_ext) {
  for (size_t i = 0; i < sizeof(k_scancode_table) / sizeof(k_scancode_table[0]); ++i) {
    if (k_scancode_table[i].evdev == evdev) {
      if (out_set1) *out_set1 = k_scancode_table[i].set1;
      if (out_ext) *out_ext = (k_scancode_table[i].ext != 0);
      return true;
    }
  }
  return false;
}

bool zc_scancode_set1_to_evdev(uint16_t set1, bool ext, uint16_t *out_evdev) {
  uint8_t e = ext ? 1U : 0U;
  for (size_t i = 0; i < sizeof(k_scancode_table) / sizeof(k_scancode_table[0]); ++i) {
    if (k_scancode_table[i].set1 == set1 && k_scancode_table[i].ext == e) {
      if (out_evdev) *out_evdev = k_scancode_table[i].evdev;
      return true;
    }
  }
  return false;
}

/* =========================================================================
 * IMA-ADPCM Audio Codec (48 kHz 16-bit, 1 or 2 channels)
 * ========================================================================= */

static const int8_t k_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static const int16_t k_ima_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};

static inline uint8_t ima_encode_sample(int16_t sample, zc_adpcm_state_t *st) {
  int32_t pred = st->prev_sample;
  int32_t idx = st->index;
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  int32_t step = k_ima_step_table[idx];
  int32_t diff = (int32_t)sample - pred;
  uint8_t code = 0;
  if (diff < 0) {
    code = 8;
    diff = -diff;
  }
  int32_t vpdiff = step >> 3;
  if (diff >= step) {
    code |= 4;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 2;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    code |= 1;
    vpdiff += step;
  }
  if (code & 8) pred -= vpdiff;
  else pred += vpdiff;
  if (pred > 32767) pred = 32767;
  if (pred < -32768) pred = -32768;
  idx += k_ima_index_table[code];
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  st->prev_sample = (int16_t)pred;
  st->index = (uint8_t)idx;
  return (uint8_t)(code & 0x0FU);
}

static inline int16_t ima_decode_nibble(uint8_t code, zc_adpcm_state_t *st) {
  int32_t pred = st->prev_sample;
  int32_t idx = st->index;
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  int32_t step = k_ima_step_table[idx];
  int32_t vpdiff = step >> 3;
  if (code & 4) vpdiff += step;
  if (code & 2) vpdiff += (step >> 1);
  if (code & 1) vpdiff += (step >> 2);
  if (code & 8) pred -= vpdiff;
  else pred += vpdiff;
  if (pred > 32767) pred = 32767;
  if (pred < -32768) pred = -32768;
  idx += k_ima_index_table[code & 0x0FU];
  if (idx < 0) idx = 0;
  if (idx > 88) idx = 88;
  st->prev_sample = (int16_t)pred;
  st->index = (uint8_t)idx;
  return (int16_t)pred;
}

int zc_audio_encode_packet(const char magic[4], uint32_t seq, uint8_t channels,
                           uint16_t samples_per_ch, const int16_t *pcm,
                           zc_adpcm_state_t state[2], uint8_t *out_pkt,
                           size_t out_cap) {
  if (!magic || !pcm || !state || !out_pkt) return -1;
  if ((channels != 1U && channels != 2U) || samples_per_ch == 0U) return -1;
  size_t nibble_bytes = ((size_t)samples_per_ch * channels + 1U) / 2U;
  size_t state_bytes = (size_t)channels * 4U;
  size_t payload_bytes = state_bytes + nibble_bytes;
  size_t total_len = ZC_AUDIO_HDR_SIZE + payload_bytes;
  if (total_len > out_cap || payload_bytes > UINT16_MAX) return -1;

  memcpy(out_pkt, magic, 4);
  wr_u32_le(out_pkt + 4, seq);
  wr_u32_le(out_pkt + 8, ZC_AUDIO_SAMPLE_RATE);
  out_pkt[12] = channels;
  out_pkt[13] = ZC_AUDIO_CODEC_ADPCM;
  wr_u16_le(out_pkt + 14, samples_per_ch);

  /* Store payload_bytes? Recompute cleanly from samples_per_ch/channels on decode,
   * OR put per-channel IMA block headers directly in payload so pkt fits 16B hdr! */
  uint8_t *p = out_pkt + ZC_AUDIO_HDR_SIZE;
  for (uint8_t c = 0; c < channels; ++c) {
    /* First sample initializes predictor in block header so each packet is self-contained */
    state[c].prev_sample = pcm[c];
    if (state[c].index > 88U) state[c].index = 0U;
    wr_u16_le(p + (size_t)c * 4U, (uint16_t)state[c].prev_sample);
    p[(size_t)c * 4U + 2U] = state[c].index;
    p[(size_t)c * 4U + 3U] = 0U;
  }

  uint8_t *dst = p + state_bytes;
  memset(dst, 0, nibble_bytes);
  size_t nib_idx = 0;
  for (uint16_t s = 0; s < samples_per_ch; ++s) {
    for (uint8_t c = 0; c < channels; ++c) {
      int16_t samp = pcm[(size_t)s * channels + c];
      uint8_t code = (s == 0U) ? 0U : ima_encode_sample(samp, &state[c]);
      if ((nib_idx & 1U) == 0U) {
        dst[nib_idx >> 1] = code;
      } else {
        dst[nib_idx >> 1] |= (uint8_t)(code << 4);
      }
      nib_idx++;
    }
  }
  return (int)total_len;
}

int zc_audio_decode_packet(const uint8_t *pkt, size_t pkt_len,
                           zc_audio_hdr_t *out_hdr, int16_t *out_pcm,
                           size_t max_samples_total) {
  if (!pkt || pkt_len < ZC_AUDIO_HDR_SIZE || !out_pcm) return -1;
  if (memcmp(pkt, ZC_AUDIO_OUT_MAGIC, 4) != 0 &&
      memcmp(pkt, ZC_AUDIO_MIC_MAGIC, 4) != 0) {
    return -1;
  }
  uint8_t channels = pkt[12];
  uint8_t codec = pkt[13];
  uint16_t samples_per_ch = rd_u16_le(pkt + 14);
  if ((channels != 1U && channels != 2U) || codec != ZC_AUDIO_CODEC_ADPCM ||
      samples_per_ch == 0U) {
    return -1;
  }
  size_t total_samples = (size_t)samples_per_ch * channels;
  if (total_samples > max_samples_total) return -1;
  size_t state_bytes = (size_t)channels * 4U;
  size_t nibble_bytes = (total_samples + 1U) / 2U;
  if (pkt_len < ZC_AUDIO_HDR_SIZE + state_bytes + nibble_bytes) return -1;

  if (out_hdr) {
    memcpy(out_hdr->magic, pkt, 4);
    out_hdr->seq = rd_u32_le(pkt + 4);
    out_hdr->sample_rate = rd_u32_le(pkt + 8);
    out_hdr->channels = channels;
    out_hdr->codec = codec;
    out_hdr->samples_per_ch = samples_per_ch;
    out_hdr->payload_bytes = (uint16_t)(state_bytes + nibble_bytes);
  }

  const uint8_t *p = pkt + ZC_AUDIO_HDR_SIZE;
  zc_adpcm_state_t st[2];
  memset(st, 0, sizeof(st));
  for (uint8_t c = 0; c < channels; ++c) {
    st[c].prev_sample = (int16_t)rd_u16_le(p + (size_t)c * 4U);
    st[c].index = p[(size_t)c * 4U + 2U];
    if (st[c].index > 88U) st[c].index = 88U;
  }

  const uint8_t *src = p + state_bytes;
  size_t nib_idx = 0;
  for (uint16_t s = 0; s < samples_per_ch; ++s) {
    for (uint8_t c = 0; c < channels; ++c) {
      uint8_t byte = src[nib_idx >> 1];
      uint8_t code = (uint8_t)(((nib_idx & 1U) == 0U) ? (byte & 0x0FU) : ((byte >> 4) & 0x0FU));
      nib_idx++;
      out_pcm[(size_t)s * channels + c] =
          (s == 0U) ? st[c].prev_sample : ima_decode_nibble(code, &st[c]);
    }
  }
  return (int)samples_per_ch;
}

/* =========================================================================
 * Lock-Free SPSC Audio Ring Buffer
 * ========================================================================= */

void zc_audio_ring_init(zc_audio_ring_t *ring) {
  if (!ring) return;
  memset(ring->data, 0, sizeof(ring->data));
#ifdef _WIN32
  InterlockedExchange(&ring->write_pos, 0);
  InterlockedExchange(&ring->read_pos, 0);
#else
  atomic_init(&ring->write_pos, 0U);
  atomic_init(&ring->read_pos, 0U);
#endif
}

void zc_audio_ring_reset(zc_audio_ring_t *ring) {
  if (!ring) return;
#ifdef _WIN32
  long wp = InterlockedCompareExchange(&ring->write_pos, 0, 0);
  InterlockedExchange(&ring->read_pos, wp);
#else
  uint32_t wp = atomic_load_explicit(&ring->write_pos, memory_order_acquire);
  atomic_store_explicit(&ring->read_pos, wp, memory_order_release);
#endif
}

size_t zc_audio_ring_available(const zc_audio_ring_t *ring) {
  if (!ring) return 0U;
#ifdef _WIN32
  uint32_t wp = (uint32_t)InterlockedCompareExchange((volatile LONG *)&ring->write_pos, 0, 0);
  uint32_t rp = (uint32_t)InterlockedCompareExchange((volatile LONG *)&ring->read_pos, 0, 0);
#else
  uint32_t wp = atomic_load_explicit(&ring->write_pos, memory_order_acquire);
  uint32_t rp = atomic_load_explicit(&ring->read_pos, memory_order_acquire);
#endif
  return (size_t)(wp - rp);
}

size_t zc_audio_ring_push(zc_audio_ring_t *ring, const int16_t *samples, size_t count) {
  if (!ring || !samples || count == 0U) return 0U;
#ifdef _WIN32
  uint32_t wp = (uint32_t)InterlockedCompareExchange(&ring->write_pos, 0, 0);
  uint32_t rp = (uint32_t)InterlockedCompareExchange(&ring->read_pos, 0, 0);
#else
  uint32_t wp = atomic_load_explicit(&ring->write_pos, memory_order_relaxed);
  uint32_t rp = atomic_load_explicit(&ring->read_pos, memory_order_acquire);
#endif
  uint32_t used = wp - rp;
  uint32_t free_sp = (used >= ZC_AUDIO_RING_SAMPLES) ? 0U : (ZC_AUDIO_RING_SAMPLES - used);
  size_t n = (count < free_sp) ? count : free_sp;
  uint32_t mask = ZC_AUDIO_RING_SAMPLES - 1U;
  for (size_t i = 0; i < n; ++i) {
    ring->data[(wp + (uint32_t)i) & mask] = samples[i];
  }
#ifdef _WIN32
  MemoryBarrier();
  InterlockedExchange(&ring->write_pos, (LONG)(wp + (uint32_t)n));
#else
  atomic_store_explicit(&ring->write_pos, wp + (uint32_t)n, memory_order_release);
#endif
  return n;
}

size_t zc_audio_ring_pop(zc_audio_ring_t *ring, int16_t *out_samples, size_t count) {
  if (!ring || !out_samples || count == 0U) return 0U;
#ifdef _WIN32
  uint32_t wp = (uint32_t)InterlockedCompareExchange(&ring->write_pos, 0, 0);
  uint32_t rp = (uint32_t)InterlockedCompareExchange(&ring->read_pos, 0, 0);
  MemoryBarrier();
#else
  uint32_t wp = atomic_load_explicit(&ring->write_pos, memory_order_acquire);
  uint32_t rp = atomic_load_explicit(&ring->read_pos, memory_order_relaxed);
#endif
  uint32_t avail = wp - rp;
  size_t n = (count < avail) ? count : avail;
  uint32_t mask = ZC_AUDIO_RING_SAMPLES - 1U;
  for (size_t i = 0; i < n; ++i) {
    out_samples[i] = ring->data[(rp + (uint32_t)i) & mask];
  }
#ifdef _WIN32
  MemoryBarrier();
  InterlockedExchange(&ring->read_pos, (LONG)(rp + (uint32_t)n));
#else
  atomic_store_explicit(&ring->read_pos, rp + (uint32_t)n, memory_order_release);
#endif
  return n;
}

/* =========================================================================
 * Shared Windows / POSIX Helpers
 * ========================================================================= */

#ifdef _WIN32

static const CLSID ZC_CLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID ZC_IID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID ZC_IID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID ZC_IID_IAudioCaptureClient = {
    0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const IID ZC_IID_IAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const PROPERTYKEY ZC_PKEY_Device_FriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000U
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000U
#endif
#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000U
#endif

static void win_sanitize_field(char *s) {
  for (; *s; ++s) {
    if (*s == '|' || *s == '\r' || *s == '\n') *s = ' ';
  }
}

typedef struct {
  uint32_t target_idx;
  uint32_t cur_idx;
  bool found;
  RECT rect;
} zc_mon_enum_t;

static BOOL CALLBACK win_mon_enum_cb(HMONITOR hmon, HDC hdc, LPRECT lprc, LPARAM data) {
  (void)hdc;
  (void)lprc;
  zc_mon_enum_t *st = (zc_mon_enum_t *)data;
  MONITORINFO mi;
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoA(hmon, &mi)) {
    if (st->cur_idx == st->target_idx) {
      st->rect = mi.rcMonitor;
      st->found = true;
      return FALSE;
    }
    st->cur_idx++;
  }
  return TRUE;
}

typedef struct {
  FILE *out;
  uint32_t idx;
} zc_mon_print_t;

static BOOL CALLBACK win_mon_print_cb(HMONITOR hmon, HDC hdc, LPRECT lprc, LPARAM data) {
  (void)hdc;
  (void)lprc;
  zc_mon_print_t *st = (zc_mon_print_t *)data;
  MONITORINFOEXA mi;
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoA(hmon, (LPMONITORINFO)&mi)) {
    char name[64];
    snprintf(name, sizeof(name), "%s", mi.szDevice);
    win_sanitize_field(name);
    LONG w = mi.rcMonitor.right - mi.rcMonitor.left;
    LONG h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    fprintf(st->out, "DISPLAY|%u|%s%s|%ldx%ld|%ld,%ld\n",
            (unsigned)st->idx, name,
            (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (Principal)" : "",
            (long)w, (long)h, (long)mi.rcMonitor.left, (long)mi.rcMonitor.top);
    st->idx++;
  }
  return TRUE;
}

/* Dynamic ViGEmClient definitions */
typedef void *PVIGEM_CLIENT;
typedef void *PVIGEM_TARGET;

typedef struct {
  USHORT wButtons;
  BYTE bLeftTrigger;
  BYTE bRightTrigger;
  SHORT sThumbLX;
  SHORT sThumbLY;
  SHORT sThumbRX;
  SHORT sThumbRY;
} zc_xusb_report_t;

typedef PVIGEM_CLIENT (*pfn_vigem_alloc_t)(void);
typedef void (*pfn_vigem_free_t)(PVIGEM_CLIENT);
typedef ULONG (*pfn_vigem_connect_t)(PVIGEM_CLIENT);
typedef void (*pfn_vigem_disconnect_t)(PVIGEM_CLIENT);
typedef PVIGEM_TARGET (*pfn_vigem_target_x360_alloc_t)(void);
typedef void (*pfn_vigem_target_free_t)(PVIGEM_TARGET);
typedef ULONG (*pfn_vigem_target_add_t)(PVIGEM_CLIENT, PVIGEM_TARGET);
typedef ULONG (*pfn_vigem_target_remove_t)(PVIGEM_CLIENT, PVIGEM_TARGET);
typedef ULONG (*pfn_vigem_target_x360_update_t)(PVIGEM_CLIENT, PVIGEM_TARGET, zc_xusb_report_t);

/* Dynamic XInput definition */
typedef DWORD (WINAPI *pfn_xinput_get_state_t)(DWORD, XINPUT_STATE *);

#else /* POSIX / Linux */

typedef struct _XDisplay zc_x11_dpy_t;
typedef unsigned long zc_x11_win_t;
typedef struct {
  int type;
} zc_x11_err_ev_t;
typedef int (*zc_x11_err_cb_t)(zc_x11_dpy_t *, zc_x11_err_ev_t *);

typedef zc_x11_dpy_t *(*pfn_xopen_dpy_t)(const char *);
typedef int (*pfn_xclose_dpy_t)(zc_x11_dpy_t *);
typedef int (*pfn_xdef_scr_t)(zc_x11_dpy_t *);
typedef int (*pfn_xdisp_w_t)(zc_x11_dpy_t *, int);
typedef int (*pfn_xdisp_h_t)(zc_x11_dpy_t *, int);
typedef zc_x11_win_t (*pfn_xroot_win_t)(zc_x11_dpy_t *, int);
typedef int (*pfn_xflush_t)(zc_x11_dpy_t *);
typedef int (*pfn_xsync_t)(zc_x11_dpy_t *, int);
typedef zc_x11_err_cb_t (*pfn_xset_err_t)(zc_x11_err_cb_t);
typedef unsigned int (*pfn_xkeysym_to_kc_t)(zc_x11_dpy_t *, unsigned long);

typedef int (*pfn_xtest_query_t)(zc_x11_dpy_t *, int *, int *, int *, int *);
typedef int (*pfn_xtest_key_t)(zc_x11_dpy_t *, unsigned int, int, unsigned long);
typedef int (*pfn_xtest_btn_t)(zc_x11_dpy_t *, unsigned int, int, unsigned long);
typedef int (*pfn_xtest_motion_t)(zc_x11_dpy_t *, int, int, int, unsigned long);
typedef int (*pfn_xtest_rel_t)(zc_x11_dpy_t *, int, int, unsigned long);

#define ZC_PA_STREAM_PLAYBACK 1
#define ZC_PA_STREAM_RECORD 2
#define ZC_PA_SAMPLE_S16LE 3

typedef struct {
  int format;
  uint32_t rate;
  uint8_t channels;
} zc_pa_sample_spec_t;

typedef struct {
  uint32_t maxlength;
  uint32_t tlength;
  uint32_t prebuf;
  uint32_t minreq;
  uint32_t fragsize;
} zc_pa_buffer_attr_t;

typedef void zc_pa_simple_t;
typedef zc_pa_simple_t *(*pfn_pa_simple_new_t)(const char *, const char *, int,
                                               const char *, const char *,
                                               const zc_pa_sample_spec_t *,
                                               const void *,
                                               const zc_pa_buffer_attr_t *, int *);
typedef void (*pfn_pa_simple_free_t)(zc_pa_simple_t *);
typedef int (*pfn_pa_simple_write_t)(zc_pa_simple_t *, const void *, size_t, int *);
typedef int (*pfn_pa_simple_read_t)(zc_pa_simple_t *, void *, size_t, int *);
typedef int (*pfn_pa_simple_drain_t)(zc_pa_simple_t *, int *);

static void zc_dlsym_copy(void *lib, void *dst, size_t dst_sz, const char *name) {
  void *sym = dlsym(lib, name);
  if (sym) memcpy(dst, &sym, dst_sz);
}

static volatile int g_periph_x11_err = 0;
static int periph_x11_err_handler(zc_x11_dpy_t *d, zc_x11_err_ev_t *e) {
  (void)d;
  (void)e;
  g_periph_x11_err = 1;
  return 0;
}

#endif

/* =========================================================================
 * Host Input & Gamepad Injector
 * ========================================================================= */

struct zc_input_injector_s {
  bool enable_kb_mouse;
  bool enable_gamepad;
  uint32_t display_idx;
#ifdef _WIN32
  RECT mon_rect;
  bool have_mon;
  HMODULE vigem_dll;
  PVIGEM_CLIENT vigem_client;
  PVIGEM_TARGET vigem_pad[ZC_MAX_GAMEPADS];
  pfn_vigem_free_t p_vigem_free;
  pfn_vigem_disconnect_t p_vigem_disconnect;
  pfn_vigem_target_x360_alloc_t p_vigem_target_x360_alloc;
  pfn_vigem_target_free_t p_vigem_target_free;
  pfn_vigem_target_add_t p_vigem_target_add;
  pfn_vigem_target_remove_t p_vigem_target_remove;
  pfn_vigem_target_x360_update_t p_vigem_target_x360_update;
  bool vigem_warned;
#else
  void *x11_lib;
  void *xtst_lib;
  zc_x11_dpy_t *dpy;
  int screen_w;
  int screen_h;
  pfn_xclose_dpy_t p_XCloseDisplay;
  pfn_xflush_t p_XFlush;
  pfn_xtest_key_t p_XTestFakeKeyEvent;
  pfn_xtest_btn_t p_XTestFakeButtonEvent;
  pfn_xtest_motion_t p_XTestFakeMotionEvent;
  pfn_xtest_rel_t p_XTestFakeRelativeMotionEvent;
  int uinput_fd[ZC_MAX_GAMEPADS];
  bool uinput_warned;
#endif
};

zc_input_injector_t *zc_input_injector_create(uint32_t display_idx,
                                              bool enable_kb_mouse,
                                              bool enable_gamepad) {
  zc_input_injector_t *inj = (zc_input_injector_t *)calloc(1, sizeof(*inj));
  if (!inj) return NULL;
  inj->display_idx = display_idx;
  inj->enable_kb_mouse = enable_kb_mouse;
  inj->enable_gamepad = enable_gamepad;

#ifdef _WIN32
  zc_mon_enum_t me;
  memset(&me, 0, sizeof(me));
  me.target_idx = display_idx;
  EnumDisplayMonitors(NULL, NULL, win_mon_enum_cb, (LPARAM)&me);
  if (me.found) {
    inj->mon_rect = me.rect;
    inj->have_mon = true;
  } else {
    inj->mon_rect.left = 0;
    inj->mon_rect.top = 0;
    inj->mon_rect.right = GetSystemMetrics(SM_CXSCREEN);
    inj->mon_rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    inj->have_mon = (inj->mon_rect.right > 0 && inj->mon_rect.bottom > 0);
  }

  if (enable_gamepad) {
    HMODULE dll = LoadLibraryA("ViGEmClient.dll");
    if (dll) {
      pfn_vigem_alloc_t p_alloc = (pfn_vigem_alloc_t)(void *)GetProcAddress(dll, "vigem_alloc");
      pfn_vigem_connect_t p_conn = (pfn_vigem_connect_t)(void *)GetProcAddress(dll, "vigem_connect");
      inj->p_vigem_free = (pfn_vigem_free_t)(void *)GetProcAddress(dll, "vigem_free");
      inj->p_vigem_disconnect = (pfn_vigem_disconnect_t)(void *)GetProcAddress(dll, "vigem_disconnect");
      inj->p_vigem_target_x360_alloc =
          (pfn_vigem_target_x360_alloc_t)(void *)GetProcAddress(dll, "vigem_target_x360_alloc");
      inj->p_vigem_target_free =
          (pfn_vigem_target_free_t)(void *)GetProcAddress(dll, "vigem_target_free");
      inj->p_vigem_target_add =
          (pfn_vigem_target_add_t)(void *)GetProcAddress(dll, "vigem_target_add");
      inj->p_vigem_target_remove =
          (pfn_vigem_target_remove_t)(void *)GetProcAddress(dll, "vigem_target_remove");
      inj->p_vigem_target_x360_update =
          (pfn_vigem_target_x360_update_t)(void *)GetProcAddress(dll, "vigem_target_x360_update");
      if (p_alloc && p_conn && inj->p_vigem_free && inj->p_vigem_disconnect &&
          inj->p_vigem_target_x360_alloc && inj->p_vigem_target_free &&
          inj->p_vigem_target_add && inj->p_vigem_target_remove &&
          inj->p_vigem_target_x360_update) {
        PVIGEM_CLIENT cli = p_alloc();
        if (cli && (p_conn(cli) == 0x20000000UL /* VIGEM_ERROR_NONE */ || p_conn(cli) == 0UL)) {
          inj->vigem_dll = dll;
          inj->vigem_client = cli;
        } else {
          if (cli) inj->p_vigem_free(cli);
          FreeLibrary(dll);
        }
      } else {
        FreeLibrary(dll);
      }
    }
    if (!inj->vigem_client) {
      fprintf(stderr, "[PERIPH] ViGEmClient.dll not found — virtual Xbox 360 gamepad injection disabled (optional driver).\n");
      inj->vigem_warned = true;
    }
  }
#else
  for (size_t i = 0; i < ZC_MAX_GAMEPADS; ++i) inj->uinput_fd[i] = -1;
  if (enable_kb_mouse) {
    inj->x11_lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!inj->x11_lib) inj->x11_lib = dlopen("libX11.so", RTLD_LAZY);
    inj->xtst_lib = dlopen("libXtst.so.6", RTLD_LAZY);
    if (!inj->xtst_lib) inj->xtst_lib = dlopen("libXtst.so", RTLD_LAZY);
    if (inj->x11_lib && inj->xtst_lib) {
      pfn_xopen_dpy_t p_open = NULL;
      pfn_xdef_scr_t p_scr = NULL;
      pfn_xdisp_w_t p_dw = NULL;
      pfn_xdisp_h_t p_dh = NULL;
      pfn_xset_err_t p_err = NULL;
      pfn_xtest_query_t p_q = NULL;
      zc_dlsym_copy(inj->x11_lib, &p_open, sizeof(p_open), "XOpenDisplay");
      zc_dlsym_copy(inj->x11_lib, &inj->p_XCloseDisplay, sizeof(inj->p_XCloseDisplay), "XCloseDisplay");
      zc_dlsym_copy(inj->x11_lib, &p_scr, sizeof(p_scr), "XDefaultScreen");
      zc_dlsym_copy(inj->x11_lib, &p_dw, sizeof(p_dw), "XDisplayWidth");
      zc_dlsym_copy(inj->x11_lib, &p_dh, sizeof(p_dh), "XDisplayHeight");
      zc_dlsym_copy(inj->x11_lib, &inj->p_XFlush, sizeof(inj->p_XFlush), "XFlush");
      zc_dlsym_copy(inj->x11_lib, &p_err, sizeof(p_err), "XSetErrorHandler");
      zc_dlsym_copy(inj->xtst_lib, &p_q, sizeof(p_q), "XTestQueryExtension");
      zc_dlsym_copy(inj->xtst_lib, &inj->p_XTestFakeKeyEvent, sizeof(inj->p_XTestFakeKeyEvent), "XTestFakeKeyEvent");
      zc_dlsym_copy(inj->xtst_lib, &inj->p_XTestFakeButtonEvent, sizeof(inj->p_XTestFakeButtonEvent), "XTestFakeButtonEvent");
      zc_dlsym_copy(inj->xtst_lib, &inj->p_XTestFakeMotionEvent, sizeof(inj->p_XTestFakeMotionEvent), "XTestFakeMotionEvent");
      zc_dlsym_copy(inj->xtst_lib, &inj->p_XTestFakeRelativeMotionEvent, sizeof(inj->p_XTestFakeRelativeMotionEvent), "XTestFakeRelativeMotionEvent");
      if (p_open && inj->p_XCloseDisplay && p_scr && p_dw && p_dh && inj->p_XFlush &&
          p_q && inj->p_XTestFakeKeyEvent && inj->p_XTestFakeButtonEvent &&
          inj->p_XTestFakeMotionEvent && inj->p_XTestFakeRelativeMotionEvent) {
        if (p_err) p_err(periph_x11_err_handler);
        inj->dpy = p_open(NULL);
        if (inj->dpy) {
          int evb = 0, erb = 0, maj = 0, minv = 0;
          if (p_q(inj->dpy, &evb, &erb, &maj, &minv)) {
            int scr = p_scr(inj->dpy);
            inj->screen_w = p_dw(inj->dpy, scr);
            inj->screen_h = p_dh(inj->dpy, scr);
          } else {
            inj->p_XCloseDisplay(inj->dpy);
            inj->dpy = NULL;
          }
        }
      }
    }
    if (!inj->dpy) {
      fprintf(stderr, "[PERIPH] X11/XTest unavailable — host HID injection disabled.\n");
    }
  }
#endif
  return inj;
}

void zc_input_injector_destroy(zc_input_injector_t *inj) {
  if (!inj) return;
#ifdef _WIN32
  if (inj->vigem_client) {
    for (size_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
      if (inj->vigem_pad[i]) {
        inj->p_vigem_target_remove(inj->vigem_client, inj->vigem_pad[i]);
        inj->p_vigem_target_free(inj->vigem_pad[i]);
        inj->vigem_pad[i] = NULL;
      }
    }
    inj->p_vigem_disconnect(inj->vigem_client);
    inj->p_vigem_free(inj->vigem_client);
    inj->vigem_client = NULL;
  }
  if (inj->vigem_dll) {
    FreeLibrary(inj->vigem_dll);
    inj->vigem_dll = NULL;
  }
#else
  for (size_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
    if (inj->uinput_fd[i] >= 0) {
#ifdef __linux__
      ioctl(inj->uinput_fd[i], UI_DEV_DESTROY);
#endif
      close(inj->uinput_fd[i]);
      inj->uinput_fd[i] = -1;
    }
  }
  if (inj->dpy && inj->p_XCloseDisplay) {
    inj->p_XCloseDisplay(inj->dpy);
    inj->dpy = NULL;
  }
  if (inj->xtst_lib) dlclose(inj->xtst_lib);
  if (inj->x11_lib) dlclose(inj->x11_lib);
#endif
  free(inj);
}

int zc_input_injector_apply_input(zc_input_injector_t *inj, const zc_input_event_t *ev) {
  if (!inj || !ev || !inj->enable_kb_mouse) return -1;

#ifdef _WIN32
  INPUT in;
  memset(&in, 0, sizeof(in));
  if (ev->kind == ZC_INPUT_KEY) {
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = ev->code;
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (ev->flags & ZC_INPUT_FLAG_EXTENDED) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!(ev->flags & ZC_INPUT_FLAG_DOWN)) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
    return 0;
  }
  in.type = INPUT_MOUSE;
  if (ev->kind == ZC_INPUT_MOUSE_ABS) {
    int32_t nx = ev->x < 0 ? 0 : (ev->x > 65535 ? 65535 : ev->x);
    int32_t ny = ev->y < 0 ? 0 : (ev->y > 65535 ? 65535 : ev->y);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (inj->have_mon && vw > 1 && vh > 1) {
      int mw = inj->mon_rect.right - inj->mon_rect.left;
      int mh = inj->mon_rect.bottom - inj->mon_rect.top;
      int px = inj->mon_rect.left + (int)(((int64_t)nx * (mw > 1 ? mw - 1 : 0)) / 65535);
      int py = inj->mon_rect.top + (int)(((int64_t)ny * (mh > 1 ? mh - 1 : 0)) / 65535);
      in.mi.dx = (LONG)(((int64_t)(px - vx) * 65535) / (vw - 1));
      in.mi.dy = (LONG)(((int64_t)(py - vy) * 65535) / (vh - 1));
      in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    } else {
      in.mi.dx = nx;
      in.mi.dy = ny;
      in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    }
    SendInput(1, &in, sizeof(in));
    return 0;
  }
  if (ev->kind == ZC_INPUT_MOUSE_REL) {
    in.mi.dx = (LONG)ev->x;
    in.mi.dy = (LONG)ev->y;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
    return 0;
  }
  if (ev->kind == ZC_INPUT_MOUSE_BTN) {
    bool down = (ev->flags & ZC_INPUT_FLAG_DOWN) != 0U;
    switch (ev->code) {
      case ZC_MOUSE_BTN_LEFT:
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
      case ZC_MOUSE_BTN_RIGHT:
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
      case ZC_MOUSE_BTN_MIDDLE:
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
      case ZC_MOUSE_BTN_X1:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON1;
        break;
      case ZC_MOUSE_BTN_X2:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON2;
        break;
      default:
        return -1;
    }
    SendInput(1, &in, sizeof(in));
    return 0;
  }
  if (ev->kind == ZC_INPUT_MOUSE_WHEEL) {
    if (ev->flags & ZC_INPUT_FLAG_HORIZ) {
      in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
      in.mi.mouseData = (DWORD)ev->x;
    } else {
      in.mi.dwFlags = MOUSEEVENTF_WHEEL;
      in.mi.mouseData = (DWORD)ev->y;
    }
    SendInput(1, &in, sizeof(in));
    return 0;
  }
  return -1;
#else
  if (!inj->dpy) return -1;
  g_periph_x11_err = 0;
  if (ev->kind == ZC_INPUT_KEY) {
    uint16_t evdev = 0;
    if (!zc_scancode_set1_to_evdev(ev->code, &evdev,
                                   (ev->flags & ZC_INPUT_FLAG_EXTENDED) != 0U)) {
      return -1;
    }
    unsigned int kc = (unsigned int)evdev + 8U;
    inj->p_XTestFakeKeyEvent(inj->dpy, kc, (ev->flags & ZC_INPUT_FLAG_DOWN) ? 1 : 0, 0UL);
    inj->p_XFlush(inj->dpy);
    return 0;
  }
  if (ev->kind == ZC_INPUT_MOUSE_ABS) {
    int32_t nx = ev->x < 0 ? 0 : (ev->x > 65535 ? 65535 : ev->x);
    int32_t ny = ev->y < 0 ? 0 : (ev->y > 65535 ? 65535 : ev->y);
    int px = (int)(((int64_t)nx * (inj->screen_w > 1 ? inj->screen_w - 1 : 0)) / 65535);
    int py = (int)(((int64_t)ny * (inj->screen_h > 1 ? inj->screen_h - 1 : 0)) / 65535);
    inj->p_XTestFakeMotionEvent(inj->dpy, -1, px, py, 0UL);
    inj->p_XFlush(inj->dpy);
    return 0;
  }
  if (ev->kind == ZC_INPUT_MOUSE_REL) {
    inj->p_XTestFakeRelativeMotionEvent(inj->dpy, (int)ev->x, (int)ev->y, 0UL);
    inj->p_XFlush(inj->dpy);
    return 0;
  }
  if (ev->kind == ZC_INPUT_MOUSE_BTN) {
    unsigned int xbtn = 1;
    switch (ev->code) {
      case ZC_MOUSE_BTN_LEFT: xbtn = 1; break;
      case ZC_MOUSE_BTN_MIDDLE: xbtn = 2; break;
      case ZC_MOUSE_BTN_RIGHT: xbtn = 3; break;
      case ZC_MOUSE_BTN_X1: xbtn = 8; break;
      case ZC_MOUSE_BTN_X2: xbtn = 9; break;
      default: return -1;
    }
    inj->p_XTestFakeButtonEvent(inj->dpy, xbtn,
                                (ev->flags & ZC_INPUT_FLAG_DOWN) ? 1 : 0, 0UL);
    inj->p_XFlush(inj->dpy);
    return 0;
  }
  if (ev->kind == ZC_INPUT_MOUSE_WHEEL) {
    unsigned int xbtn = 0;
    if (ev->flags & ZC_INPUT_FLAG_HORIZ) {
      xbtn = (ev->x > 0) ? 7U : 6U;
    } else {
      xbtn = (ev->y > 0) ? 4U : 5U;
    }
    inj->p_XTestFakeButtonEvent(inj->dpy, xbtn, 1, 0UL);
    inj->p_XTestFakeButtonEvent(inj->dpy, xbtn, 0, 0UL);
    inj->p_XFlush(inj->dpy);
    return 0;
  }
  return -1;
#endif
}

#if !defined(_WIN32) && defined(__linux__)
static int open_uinput_gamepad(uint8_t idx) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) return -1;

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  ioctl(fd, UI_SET_EVBIT, EV_SYN);

  static const int keys[] = {
      BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
      BTN_THUMBL, BTN_THUMBR, BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    ioctl(fd, UI_SET_KEYBIT, keys[i]);
  }
  static const int abs_axes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};
  for (size_t i = 0; i < sizeof(abs_axes) / sizeof(abs_axes[0]); ++i) {
    ioctl(fd, UI_SET_ABSBIT, abs_axes[i]);
  }

  struct uinput_user_dev uidev;
  memset(&uidev, 0, sizeof(uidev));
  snprintf(uidev.name, sizeof(uidev.name), "zcstream Virtual X360 #%u", (unsigned)idx);
  uidev.id.bustype = BUS_USB;
  uidev.id.vendor = 0x045E;
  uidev.id.product = 0x028E;
  uidev.id.version = 1;

  uidev.absmin[ABS_X] = -32768; uidev.absmax[ABS_X] = 32767;
  uidev.absmin[ABS_Y] = -32768; uidev.absmax[ABS_Y] = 32767;
  uidev.absmin[ABS_RX] = -32768; uidev.absmax[ABS_RX] = 32767;
  uidev.absmin[ABS_RY] = -32768; uidev.absmax[ABS_RY] = 32767;
  uidev.absmin[ABS_Z] = 0; uidev.absmax[ABS_Z] = 255;
  uidev.absmin[ABS_RZ] = 0; uidev.absmax[ABS_RZ] = 255;

  if (write(fd, &uidev, sizeof(uidev)) != (ssize_t)sizeof(uidev) ||
      ioctl(fd, UI_DEV_CREATE) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void uinput_emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  struct input_event ie;
  memset(&ie, 0, sizeof(ie));
  ie.type = type;
  ie.code = code;
  ie.value = value;
  (void)write(fd, &ie, sizeof(ie));
}
#endif

int zc_input_injector_apply_gamepad(zc_input_injector_t *inj, const zc_gamepad_state_t *gp) {
  if (!inj || !gp || !inj->enable_gamepad || gp->index >= ZC_MAX_GAMEPADS) return -1;

#ifdef _WIN32
  if (!inj->vigem_client) return -1;
  uint8_t idx = gp->index;
  if (!gp->connected) {
    if (inj->vigem_pad[idx]) {
      inj->p_vigem_target_remove(inj->vigem_client, inj->vigem_pad[idx]);
      inj->p_vigem_target_free(inj->vigem_pad[idx]);
      inj->vigem_pad[idx] = NULL;
    }
    return 0;
  }
  if (!inj->vigem_pad[idx]) {
    PVIGEM_TARGET tgt = inj->p_vigem_target_x360_alloc();
    if (!tgt) return -1;
    ULONG rc = inj->p_vigem_target_add(inj->vigem_client, tgt);
    if (rc != 0x20000000UL && rc != 0UL) {
      inj->p_vigem_target_free(tgt);
      return -1;
    }
    inj->vigem_pad[idx] = tgt;
  }
  zc_xusb_report_t rep;
  rep.wButtons = gp->buttons;
  rep.bLeftTrigger = gp->lt;
  rep.bRightTrigger = gp->rt;
  rep.sThumbLX = gp->lx;
  rep.sThumbLY = gp->ly;
  rep.sThumbRX = gp->rx;
  rep.sThumbRY = gp->ry;
  inj->p_vigem_target_x360_update(inj->vigem_client, inj->vigem_pad[idx], rep);
  return 0;
#elif defined(__linux__)
  uint8_t idx = gp->index;
  if (!gp->connected) {
    if (inj->uinput_fd[idx] >= 0) {
      ioctl(inj->uinput_fd[idx], UI_DEV_DESTROY);
      close(inj->uinput_fd[idx]);
      inj->uinput_fd[idx] = -1;
    }
    return 0;
  }
  if (inj->uinput_fd[idx] < 0) {
    inj->uinput_fd[idx] = open_uinput_gamepad(idx);
    if (inj->uinput_fd[idx] < 0) {
      if (!inj->uinput_warned) {
        inj->uinput_warned = true;
        fprintf(stderr, "[PERIPH] Cannot open /dev/uinput (%s) — virtual Linux gamepad disabled.\n",
                strerror(errno));
      }
      return -1;
    }
  }
  int fd = inj->uinput_fd[idx];
  uinput_emit(fd, EV_KEY, BTN_A, (gp->buttons & ZC_GP_A) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_B, (gp->buttons & ZC_GP_B) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_X, (gp->buttons & ZC_GP_X) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_Y, (gp->buttons & ZC_GP_Y) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_TL, (gp->buttons & ZC_GP_LEFT_SHOULDER) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_TR, (gp->buttons & ZC_GP_RIGHT_SHOULDER) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_SELECT, (gp->buttons & ZC_GP_BACK) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_START, (gp->buttons & ZC_GP_START) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_THUMBL, (gp->buttons & ZC_GP_LEFT_THUMB) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_THUMBR, (gp->buttons & ZC_GP_RIGHT_THUMB) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_DPAD_UP, (gp->buttons & ZC_GP_DPAD_UP) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_DPAD_DOWN, (gp->buttons & ZC_GP_DPAD_DOWN) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_DPAD_LEFT, (gp->buttons & ZC_GP_DPAD_LEFT) ? 1 : 0);
  uinput_emit(fd, EV_KEY, BTN_DPAD_RIGHT, (gp->buttons & ZC_GP_DPAD_RIGHT) ? 1 : 0);
  uinput_emit(fd, EV_ABS, ABS_X, gp->lx);
  uinput_emit(fd, EV_ABS, ABS_Y, -((int32_t)gp->ly + 1) < -32768 ? -32768 : -gp->ly);
  uinput_emit(fd, EV_ABS, ABS_RX, gp->rx);
  uinput_emit(fd, EV_ABS, ABS_RY, -((int32_t)gp->ry + 1) < -32768 ? -32768 : -gp->ry);
  uinput_emit(fd, EV_ABS, ABS_Z, gp->lt);
  uinput_emit(fd, EV_ABS, ABS_RZ, gp->rt);
  uinput_emit(fd, EV_SYN, SYN_REPORT, 0);
  return 0;
#else
  (void)gp;
  return -1;
#endif
}

/* =========================================================================
 * Client Gamepad Poller (Windows XInput, Linux /dev/input/js*)
 * ========================================================================= */

struct zc_gamepad_poller_s {
  zc_gamepad_state_t prev[ZC_MAX_GAMEPADS];
  uint32_t seq;
#ifdef _WIN32
  HMODULE xinput_dll;
  pfn_xinput_get_state_t p_XInputGetState;
#elif defined(__linux__)
  int js_fd[ZC_MAX_GAMEPADS];
  zc_gamepad_state_t cur[ZC_MAX_GAMEPADS];
#endif
};

zc_gamepad_poller_t *zc_gamepad_poller_create(void) {
  zc_gamepad_poller_t *p = (zc_gamepad_poller_t *)calloc(1, sizeof(*p));
  if (!p) return NULL;
#ifdef _WIN32
  static const char *const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
  for (size_t i = 0; i < sizeof(dlls) / sizeof(dlls[0]); ++i) {
    HMODULE h = LoadLibraryA(dlls[i]);
    if (h) {
      pfn_xinput_get_state_t fn =
          (pfn_xinput_get_state_t)(void *)GetProcAddress(h, "XInputGetState");
      if (fn) {
        p->xinput_dll = h;
        p->p_XInputGetState = fn;
        break;
      }
      FreeLibrary(h);
    }
  }
#elif defined(__linux__)
  for (size_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
    p->js_fd[i] = -1;
    p->cur[i].index = (uint8_t)i;
  }
#endif
  return p;
}

void zc_gamepad_poller_destroy(zc_gamepad_poller_t *poller) {
  if (!poller) return;
#ifdef _WIN32
  if (poller->xinput_dll) FreeLibrary(poller->xinput_dll);
#elif defined(__linux__)
  for (size_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
    if (poller->js_fd[i] >= 0) close(poller->js_fd[i]);
  }
#endif
  free(poller);
}

static bool gp_state_differs(const zc_gamepad_state_t *a, const zc_gamepad_state_t *b) {
  return a->connected != b->connected || a->buttons != b->buttons ||
         a->lt != b->lt || a->rt != b->rt || a->lx != b->lx || a->ly != b->ly ||
         a->rx != b->rx || a->ry != b->ry;
}

int zc_gamepad_poller_poll(zc_gamepad_poller_t *poller,
                           zc_gamepad_state_t out_states[ZC_MAX_GAMEPADS],
                           bool out_changed[ZC_MAX_GAMEPADS]) {
  if (!poller || !out_states || !out_changed) return -1;
  int active = 0;

#ifdef _WIN32
  for (uint32_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
    zc_gamepad_state_t st;
    memset(&st, 0, sizeof(st));
    st.index = (uint8_t)i;
    if (poller->p_XInputGetState) {
      XINPUT_STATE xs;
      memset(&xs, 0, sizeof(xs));
      if (poller->p_XInputGetState(i, &xs) == ERROR_SUCCESS) {
        st.connected = 1U;
        st.buttons = xs.Gamepad.wButtons;
        st.lt = xs.Gamepad.bLeftTrigger;
        st.rt = xs.Gamepad.bRightTrigger;
        st.lx = xs.Gamepad.sThumbLX;
        st.ly = xs.Gamepad.sThumbLY;
        st.rx = xs.Gamepad.sThumbRX;
        st.ry = xs.Gamepad.sThumbRY;
        active++;
      }
    }
    bool diff = gp_state_differs(&st, &poller->prev[i]);
    if (diff) st.seq = ++poller->seq;
    else st.seq = poller->prev[i].seq;
    poller->prev[i] = st;
    out_states[i] = st;
    out_changed[i] = diff;
  }
#elif defined(__linux__)
  for (uint32_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
    if (poller->js_fd[i] < 0) {
      char devpath[32];
      snprintf(devpath, sizeof(devpath), "/dev/input/js%u", (unsigned)i);
      int fd = open(devpath, O_RDONLY | O_NONBLOCK);
      if (fd >= 0) {
        poller->js_fd[i] = fd;
        poller->cur[i].connected = 1U;
      }
    }
    if (poller->js_fd[i] >= 0) {
      struct js_event jse;
      for (;;) {
        ssize_t n = read(poller->js_fd[i], &jse, sizeof(jse));
        if (n != (ssize_t)sizeof(jse)) {
          if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(poller->js_fd[i]);
            poller->js_fd[i] = -1;
            memset(&poller->cur[i], 0, sizeof(poller->cur[i]));
            poller->cur[i].index = (uint8_t)i;
          }
          break;
        }
        uint8_t t = (uint8_t)(jse.type & ~JS_EVENT_INIT);
        if (t == JS_EVENT_BUTTON) {
          static const uint16_t btn_bits[11] = {
              ZC_GP_A, ZC_GP_B, ZC_GP_X, ZC_GP_Y, ZC_GP_LEFT_SHOULDER,
              ZC_GP_RIGHT_SHOULDER, ZC_GP_BACK, ZC_GP_START, 0U,
              ZC_GP_LEFT_THUMB, ZC_GP_RIGHT_THUMB};
          if (jse.number < 11U && btn_bits[jse.number] != 0U) {
            if (jse.value) poller->cur[i].buttons |= btn_bits[jse.number];
            else poller->cur[i].buttons &= (uint16_t)~btn_bits[jse.number];
          }
        } else if (t == JS_EVENT_AXIS) {
          switch (jse.number) {
            case 0: poller->cur[i].lx = jse.value; break;
            case 1: poller->cur[i].ly = (int16_t)(-jse.value - 1); break;
            case 2: poller->cur[i].lt = (uint8_t)(((int32_t)jse.value + 32768) >> 8); break;
            case 3: poller->cur[i].rx = jse.value; break;
            case 4: poller->cur[i].ry = (int16_t)(-jse.value - 1); break;
            case 5: poller->cur[i].rt = (uint8_t)(((int32_t)jse.value + 32768) >> 8); break;
            case 6:
              poller->cur[i].buttons &= (uint16_t)~(ZC_GP_DPAD_LEFT | ZC_GP_DPAD_RIGHT);
              if (jse.value < -8000) poller->cur[i].buttons |= ZC_GP_DPAD_LEFT;
              else if (jse.value > 8000) poller->cur[i].buttons |= ZC_GP_DPAD_RIGHT;
              break;
            case 7:
              poller->cur[i].buttons &= (uint16_t)~(ZC_GP_DPAD_UP | ZC_GP_DPAD_DOWN);
              if (jse.value < -8000) poller->cur[i].buttons |= ZC_GP_DPAD_UP;
              else if (jse.value > 8000) poller->cur[i].buttons |= ZC_GP_DPAD_DOWN;
              break;
            default: break;
          }
        }
      }
    }
    if (poller->cur[i].connected) active++;
    bool diff = gp_state_differs(&poller->cur[i], &poller->prev[i]);
    if (diff) poller->cur[i].seq = ++poller->seq;
    poller->prev[i] = poller->cur[i];
    out_states[i] = poller->cur[i];
    out_changed[i] = diff;
  }
#else
  for (uint32_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
    memset(&out_states[i], 0, sizeof(out_states[i]));
    out_states[i].index = (uint8_t)i;
    out_changed[i] = false;
  }
#endif
  return active;
}

/* =========================================================================
 * Bidirectional Audio Capture & Playback Engine
 * ========================================================================= */

struct zc_audio_engine_s {
  bool is_capture;
  bool loopback;
  uint8_t channels;
  char magic[4];
  char device_id[256];
  zc_audio_pkt_cb_t cb;
  void *userdata;
  zc_audio_ring_t *ring;
#ifdef _WIN32
  volatile LONG stop_flag;
  HANDLE thread;
#else
  _Atomic bool stop_flag;
  pthread_t thread;
  bool thread_started;
#endif
};

static inline bool aud_should_stop(zc_audio_engine_t *e) {
#ifdef _WIN32
  return InterlockedCompareExchange(&e->stop_flag, 0, 0) != 0;
#else
  return atomic_load_explicit(&e->stop_flag, memory_order_acquire);
#endif
}

#ifdef _WIN32

static IMMDevice *wasapi_pick_device(IMMDeviceEnumerator *en, EDataFlow flow,
                                     const char *device_id) {
  IMMDevice *dev = NULL;
  if (device_id && device_id[0] != '\0' && strcmp(device_id, "default") != 0) {
    IMMDeviceCollection *coll = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, flow, DEVICE_STATE_ACTIVE, &coll)) &&
        coll) {
      UINT cnt = 0;
      IMMDeviceCollection_GetCount(coll, &cnt);
      long req_idx = -1;
      if (device_id[0] >= '0' && device_id[0] <= '9') {
        req_idx = strtol(device_id, NULL, 10);
      }
      for (UINT i = 0; i < cnt; ++i) {
        IMMDevice *cand = NULL;
        if (FAILED(IMMDeviceCollection_Item(coll, i, &cand)) || !cand) continue;
        bool match = ((long)i == req_idx);
        if (!match) {
          LPWSTR wid = NULL;
          if (SUCCEEDED(IMMDevice_GetId(cand, &wid)) && wid) {
            char uid[256];
            WideCharToMultiByte(CP_UTF8, 0, wid, -1, uid, sizeof(uid), NULL, NULL);
            CoTaskMemFree(wid);
            if (strcmp(uid, device_id) == 0) match = true;
          }
        }
        if (match) {
          dev = cand;
          break;
        }
        IMMDevice_Release(cand);
      }
      IMMDeviceCollection_Release(coll);
    }
  }
  if (!dev) {
    ERole role = (flow == eCapture) ? eCommunications : eConsole;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, role, &dev)) &&
        flow == eCapture) {
      IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, eConsole, &dev);
    }
  }
  return dev;
}

static bool wasapi_format_is_float(const WAVEFORMATEX *wfx) {
  if (!wfx) return false;
  if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
    /* First 4 bytes of SubFormat after WAVEFORMATEX+6 bytes == 0x00000003 for IEEE_FLOAT */
    const uint8_t *raw = (const uint8_t *)wfx;
    uint32_t sub0 = rd_u32_le(raw + sizeof(WAVEFORMATEX) + 6);
    if (sub0 == 0x00000003U) return true;
  }
  return false;
}

static DWORD WINAPI wasapi_capture_thread(LPVOID arg) {
  zc_audio_engine_t *e = (zc_audio_engine_t *)arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  IMMDeviceEnumerator *en = NULL;
  if (FAILED(CoCreateInstance(&ZC_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                              &ZC_IID_IMMDeviceEnumerator, (void **)&en)) ||
      !en) {
    CoUninitialize();
    return 0;
  }
  EDataFlow flow = e->loopback ? eRender : eCapture;
  IMMDevice *dev = wasapi_pick_device(en, flow, e->device_id);
  IMMDeviceEnumerator_Release(en);
  if (!dev) {
    fprintf(stderr, "[AUDIO] No WASAPI %s device available.\n",
            e->loopback ? "loopback" : "capture");
    CoUninitialize();
    return 0;
  }

  IAudioClient *cli = NULL;
  if (FAILED(IMMDevice_Activate(dev, &ZC_IID_IAudioClient, CLSCTX_ALL, NULL,
                                (void **)&cli)) ||
      !cli) {
    IMMDevice_Release(dev);
    CoUninitialize();
    return 0;
  }
  IMMDevice_Release(dev);

  WAVEFORMATEX want;
  memset(&want, 0, sizeof(want));
  want.wFormatTag = WAVE_FORMAT_PCM;
  want.nChannels = e->channels;
  want.nSamplesPerSec = ZC_AUDIO_SAMPLE_RATE;
  want.wBitsPerSample = 16;
  want.nBlockAlign = (WORD)(want.nChannels * 2);
  want.nAvgBytesPerSec = want.nSamplesPerSec * want.nBlockAlign;

  DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (e->loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

  REFERENCE_TIME dur = 200000; /* 20 ms */
  WAVEFORMATEX *mix_fmt = NULL;
  bool native_ok = SUCCEEDED(
      IAudioClient_Initialize(cli, AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, &want, NULL));
  if (!native_ok) {
    /* Fallback to shared mix format without AUTOCONVERTPCM */
    if (FAILED(IAudioClient_GetMixFormat(cli, &mix_fmt)) || !mix_fmt) {
      IAudioClient_Release(cli);
      CoUninitialize();
      return 0;
    }
    DWORD fb_flags = e->loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0U;
    if (FAILED(IAudioClient_Initialize(cli, AUDCLNT_SHAREMODE_SHARED, fb_flags, dur, 0,
                                       mix_fmt, NULL))) {
      CoTaskMemFree(mix_fmt);
      IAudioClient_Release(cli);
      CoUninitialize();
      return 0;
    }
  }

  IAudioCaptureClient *cap = NULL;
  if (FAILED(IAudioClient_GetService(cli, &ZC_IID_IAudioCaptureClient, (void **)&cap)) ||
      !cap) {
    if (mix_fmt) CoTaskMemFree(mix_fmt);
    IAudioClient_Release(cli);
    CoUninitialize();
    return 0;
  }

  if (FAILED(IAudioClient_Start(cli))) {
    IAudioCaptureClient_Release(cap);
    if (mix_fmt) CoTaskMemFree(mix_fmt);
    IAudioClient_Release(cli);
    CoUninitialize();
    return 0;
  }

  int16_t accum[ZC_AUDIO_FRAME_SAMPLES * 2U];
  size_t accum_frames = 0;
  uint32_t seq = 0;
  zc_adpcm_state_t adpcm[2];
  memset(adpcm, 0, sizeof(adpcm));
  uint8_t pkt[ZC_AUDIO_MAX_PKT_SIZE];

  while (!aud_should_stop(e)) {
    UINT32 pkt_frames = 0;
    if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &pkt_frames))) break;
    if (pkt_frames == 0U) {
      Sleep(2);
      continue;
    }
    while (pkt_frames > 0U && !aud_should_stop(e)) {
      BYTE *data = NULL;
      UINT32 num_frames = 0;
      DWORD cflags = 0;
      if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &num_frames, &cflags,
                                               NULL, NULL))) {
        break;
      }
      uint32_t in_ch = mix_fmt ? mix_fmt->nChannels : e->channels;
      uint32_t in_rate = mix_fmt ? mix_fmt->nSamplesPerSec : ZC_AUDIO_SAMPLE_RATE;
      bool is_flt = mix_fmt ? wasapi_format_is_float(mix_fmt) : false;
      bool silent = ((cflags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U) || !data;

      uint32_t out_frames_est =
          (in_rate == ZC_AUDIO_SAMPLE_RATE)
              ? num_frames
              : (uint32_t)(((uint64_t)num_frames * ZC_AUDIO_SAMPLE_RATE) / in_rate);
      for (uint32_t i = 0; i < out_frames_est; ++i) {
        uint32_t si = (in_rate == ZC_AUDIO_SAMPLE_RATE)
                          ? i
                          : (uint32_t)(((uint64_t)i * in_rate) / ZC_AUDIO_SAMPLE_RATE);
        if (si >= num_frames) si = num_frames - 1U;
        for (uint8_t c = 0; c < e->channels; ++c) {
          int16_t s = 0;
          if (!silent) {
            uint32_t sc = (c < in_ch) ? c : 0U;
            if (!mix_fmt) {
              s = ((const int16_t *)data)[si * in_ch + sc];
            } else if (is_flt) {
              float fv = ((const float *)data)[si * in_ch + sc];
              if (fv > 1.0f) fv = 1.0f;
              if (fv < -1.0f) fv = -1.0f;
              s = (int16_t)(fv * 32767.0f);
            } else if (mix_fmt->wBitsPerSample == 16) {
              s = ((const int16_t *)data)[si * in_ch + sc];
            }
          }
          accum[accum_frames * e->channels + c] = s;
        }
        accum_frames++;
        if (accum_frames == ZC_AUDIO_FRAME_SAMPLES) {
          int plen = zc_audio_encode_packet(e->magic, seq++, e->channels,
                                            ZC_AUDIO_FRAME_SAMPLES, accum, adpcm,
                                            pkt, sizeof(pkt));
          if (plen > 0 && e->cb) e->cb(pkt, (size_t)plen, e->userdata);
          accum_frames = 0;
        }
      }
      IAudioCaptureClient_ReleaseBuffer(cap, num_frames);
      if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &pkt_frames))) {
        pkt_frames = 0;
      }
    }
  }

  IAudioClient_Stop(cli);
  IAudioCaptureClient_Release(cap);
  if (mix_fmt) CoTaskMemFree(mix_fmt);
  IAudioClient_Release(cli);
  CoUninitialize();
  return 0;
}

static DWORD WINAPI wasapi_playback_thread(LPVOID arg) {
  zc_audio_engine_t *e = (zc_audio_engine_t *)arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  IMMDeviceEnumerator *en = NULL;
  if (FAILED(CoCreateInstance(&ZC_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                              &ZC_IID_IMMDeviceEnumerator, (void **)&en)) ||
      !en) {
    CoUninitialize();
    return 0;
  }
  IMMDevice *dev = wasapi_pick_device(en, eRender, e->device_id);
  IMMDeviceEnumerator_Release(en);
  if (!dev) {
    CoUninitialize();
    return 0;
  }

  IAudioClient *cli = NULL;
  if (FAILED(IMMDevice_Activate(dev, &ZC_IID_IAudioClient, CLSCTX_ALL, NULL,
                                (void **)&cli)) ||
      !cli) {
    IMMDevice_Release(dev);
    CoUninitialize();
    return 0;
  }
  IMMDevice_Release(dev);

  WAVEFORMATEX want;
  memset(&want, 0, sizeof(want));
  want.wFormatTag = WAVE_FORMAT_PCM;
  want.nChannels = e->channels;
  want.nSamplesPerSec = ZC_AUDIO_SAMPLE_RATE;
  want.wBitsPerSample = 16;
  want.nBlockAlign = (WORD)(want.nChannels * 2);
  want.nAvgBytesPerSec = want.nSamplesPerSec * want.nBlockAlign;

  REFERENCE_TIME dur = 400000; /* 40 ms shared buffer */
  DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (FAILED(IAudioClient_Initialize(cli, AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, &want, NULL))) {
    IAudioClient_Release(cli);
    CoUninitialize();
    return 0;
  }

  UINT32 buf_frames = 0;
  IAudioClient_GetBufferSize(cli, &buf_frames);
  IAudioRenderClient *ren = NULL;
  if (FAILED(IAudioClient_GetService(cli, &ZC_IID_IAudioRenderClient, (void **)&ren)) ||
      !ren) {
    IAudioClient_Release(cli);
    CoUninitialize();
    return 0;
  }

  if (FAILED(IAudioClient_Start(cli))) {
    IAudioRenderClient_Release(ren);
    IAudioClient_Release(cli);
    CoUninitialize();
    return 0;
  }

  int16_t tmp[ZC_AUDIO_FRAME_SAMPLES * 4U];
  const size_t max_ring_latency = (size_t)e->channels * ZC_AUDIO_FRAME_SAMPLES * 6U; /* 60ms cap */
  while (!aud_should_stop(e)) {
    UINT32 padding = 0;
    if (FAILED(IAudioClient_GetCurrentPadding(cli, &padding))) break;
    UINT32 avail_frames = (buf_frames > padding) ? (buf_frames - padding) : 0U;
    if (avail_frames < ZC_AUDIO_FRAME_SAMPLES / 2U) {
      Sleep(2);
      continue;
    }

    size_t ring_avail = zc_audio_ring_available(e->ring);
    while (ring_avail > max_ring_latency) {
      size_t drop = zc_audio_ring_pop(e->ring, tmp, (size_t)e->channels * ZC_AUDIO_FRAME_SAMPLES);
      if (drop == 0U) break;
      ring_avail = zc_audio_ring_available(e->ring);
    }

    UINT32 req = (avail_frames > ZC_AUDIO_FRAME_SAMPLES * 2U)
                     ? (ZC_AUDIO_FRAME_SAMPLES * 2U)
                     : avail_frames;
    size_t can_frames = ring_avail / e->channels;
    if (can_frames == 0U) {
      Sleep(2);
      continue;
    }
    if (req > (UINT32)can_frames) req = (UINT32)can_frames;

    BYTE *dst = NULL;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(ren, req, &dst)) && dst) {
      size_t got = zc_audio_ring_pop(e->ring, (int16_t *)dst, (size_t)req * e->channels);
      if (got < (size_t)req * e->channels) {
        memset(((int16_t *)dst) + got, 0, ((size_t)req * e->channels - got) * sizeof(int16_t));
      }
      IAudioRenderClient_ReleaseBuffer(ren, req, 0);
    }
  }

  IAudioClient_Stop(cli);
  IAudioRenderClient_Release(ren);
  IAudioClient_Release(cli);
  CoUninitialize();
  return 0;
}

#else /* Linux PulseAudio via dlopen("libpulse-simple.so.0") */

static void *pa_capture_thread(void *arg) {
  zc_audio_engine_t *e = (zc_audio_engine_t *)arg;
  void *lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
  if (!lib) lib = dlopen("libpulse-simple.so", RTLD_LAZY);
  if (!lib) {
    fprintf(stderr, "[AUDIO] libpulse-simple.so.0 unavailable — audio capture disabled.\n");
    return NULL;
  }
  pfn_pa_simple_new_t p_new = NULL;
  pfn_pa_simple_free_t p_free = NULL;
  pfn_pa_simple_read_t p_read = NULL;
  zc_dlsym_copy(lib, &p_new, sizeof(p_new), "pa_simple_new");
  zc_dlsym_copy(lib, &p_free, sizeof(p_free), "pa_simple_free");
  zc_dlsym_copy(lib, &p_read, sizeof(p_read), "pa_simple_read");
  if (!p_new || !p_free || !p_read) {
    dlclose(lib);
    return NULL;
  }

  char dev_buf[256];
  const char *dev = NULL;
  if (e->device_id[0] != '\0' && strcmp(e->device_id, "default") != 0) {
    dev = e->device_id;
  } else if (e->loopback) {
    FILE *fp = popen("pactl get-default-sink 2>/dev/null", "r");
    if (fp) {
      if (fgets(dev_buf, (int)sizeof(dev_buf) - 16, fp)) {
        size_t l = strlen(dev_buf);
        while (l > 0 && (dev_buf[l - 1] == '\n' || dev_buf[l - 1] == '\r')) {
          dev_buf[--l] = '\0';
        }
        if (l > 0) {
          strcat(dev_buf, ".monitor");
          dev = dev_buf;
        }
      }
      pclose(fp);
    }
  }

  zc_pa_sample_spec_t ss = {ZC_PA_SAMPLE_S16LE, ZC_AUDIO_SAMPLE_RATE, e->channels};
  zc_pa_buffer_attr_t ba = {(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1,
                            (uint32_t)(ZC_AUDIO_FRAME_SAMPLES * e->channels * 2U)};
  int err = 0;
  zc_pa_simple_t *pa = p_new(NULL, "zcstream", ZC_PA_STREAM_RECORD, dev,
                             e->loopback ? "Desktop Loopback" : "Microphone", &ss,
                             NULL, &ba, &err);
  if (!pa) {
    fprintf(stderr, "[AUDIO] PulseAudio capture init failed (err=%d) — audio capture disabled.\n", err);
    dlclose(lib);
    return NULL;
  }

  int16_t pcm[ZC_AUDIO_FRAME_SAMPLES * 2U];
  uint8_t pkt[ZC_AUDIO_MAX_PKT_SIZE];
  zc_adpcm_state_t adpcm[2];
  memset(adpcm, 0, sizeof(adpcm));
  uint32_t seq = 0;

  while (!aud_should_stop(e)) {
    if (p_read(pa, pcm, (size_t)ZC_AUDIO_FRAME_SAMPLES * e->channels * 2U, &err) < 0) {
      break;
    }
    int plen = zc_audio_encode_packet(e->magic, seq++, e->channels,
                                      ZC_AUDIO_FRAME_SAMPLES, pcm, adpcm, pkt,
                                      sizeof(pkt));
    if (plen > 0 && e->cb) e->cb(pkt, (size_t)plen, e->userdata);
  }

  p_free(pa);
  dlclose(lib);
  return NULL;
}

static void *pa_playback_thread(void *arg) {
  zc_audio_engine_t *e = (zc_audio_engine_t *)arg;
  void *lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
  if (!lib) lib = dlopen("libpulse-simple.so", RTLD_LAZY);
  if (!lib) {
    fprintf(stderr, "[AUDIO] libpulse-simple.so.0 unavailable — audio playback disabled.\n");
    return NULL;
  }
  pfn_pa_simple_new_t p_new = NULL;
  pfn_pa_simple_free_t p_free = NULL;
  pfn_pa_simple_write_t p_write = NULL;
  zc_dlsym_copy(lib, &p_new, sizeof(p_new), "pa_simple_new");
  zc_dlsym_copy(lib, &p_free, sizeof(p_free), "pa_simple_free");
  zc_dlsym_copy(lib, &p_write, sizeof(p_write), "pa_simple_write");
  if (!p_new || !p_free || !p_write) {
    dlclose(lib);
    return NULL;
  }

  const char *dev = (e->device_id[0] != '\0' && strcmp(e->device_id, "default") != 0)
                        ? e->device_id
                        : NULL;
  uint32_t frame_bytes = (uint32_t)(ZC_AUDIO_FRAME_SAMPLES * e->channels * 2U);
  zc_pa_sample_spec_t ss = {ZC_PA_SAMPLE_S16LE, ZC_AUDIO_SAMPLE_RATE, e->channels};
  zc_pa_buffer_attr_t ba = {frame_bytes * 6U, frame_bytes * 2U, 0U, frame_bytes, (uint32_t)-1};
  int err = 0;
  zc_pa_simple_t *pa = p_new(NULL, "zcstream", ZC_PA_STREAM_PLAYBACK, dev,
                             "Stream Audio", &ss, NULL, &ba, &err);
  if (!pa) {
    fprintf(stderr, "[AUDIO] PulseAudio playback init failed (err=%d) — audio playback disabled.\n", err);
    dlclose(lib);
    return NULL;
  }

  int16_t pcm[ZC_AUDIO_FRAME_SAMPLES * 2U];
  const size_t chunk_samples = (size_t)ZC_AUDIO_FRAME_SAMPLES * e->channels;
  const size_t max_ring_latency = chunk_samples * 6U;
  while (!aud_should_stop(e)) {
    size_t avail = zc_audio_ring_available(e->ring);
    while (avail > max_ring_latency) {
      (void)zc_audio_ring_pop(e->ring, pcm, chunk_samples);
      avail = zc_audio_ring_available(e->ring);
    }
    if (avail >= chunk_samples) {
      size_t got = zc_audio_ring_pop(e->ring, pcm, chunk_samples);
      if (got > 0U && p_write(pa, pcm, got * sizeof(int16_t), &err) < 0) break;
    } else {
      struct timespec ts = {0, 2000000L};
      nanosleep(&ts, NULL);
    }
  }

  p_free(pa);
  dlclose(lib);
  return NULL;
}

#endif

zc_audio_engine_t *zc_audio_capture_start(bool loopback, uint8_t channels,
                                          const char *device_id,
                                          const char magic[4],
                                          zc_audio_pkt_cb_t cb,
                                          void *userdata) {
  if (!cb || !magic || (channels != 1U && channels != 2U)) return NULL;
  zc_audio_engine_t *e = (zc_audio_engine_t *)calloc(1, sizeof(*e));
  if (!e) return NULL;
  e->is_capture = true;
  e->loopback = loopback;
  e->channels = channels;
  memcpy(e->magic, magic, 4);
  if (device_id) snprintf(e->device_id, sizeof(e->device_id), "%s", device_id);
  e->cb = cb;
  e->userdata = userdata;
#ifdef _WIN32
  InterlockedExchange(&e->stop_flag, 0);
  e->thread = CreateThread(NULL, 0, wasapi_capture_thread, e, 0, NULL);
  if (!e->thread) {
    free(e);
    return NULL;
  }
#else
  atomic_init(&e->stop_flag, false);
  if (pthread_create(&e->thread, NULL, pa_capture_thread, e) != 0) {
    free(e);
    return NULL;
  }
  e->thread_started = true;
#endif
  return e;
}

zc_audio_engine_t *zc_audio_playback_start(uint8_t channels,
                                           const char *device_id,
                                           zc_audio_ring_t *ring) {
  if (!ring || (channels != 1U && channels != 2U)) return NULL;
  zc_audio_engine_t *e = (zc_audio_engine_t *)calloc(1, sizeof(*e));
  if (!e) return NULL;
  e->is_capture = false;
  e->channels = channels;
  if (device_id) snprintf(e->device_id, sizeof(e->device_id), "%s", device_id);
  e->ring = ring;
#ifdef _WIN32
  InterlockedExchange(&e->stop_flag, 0);
  e->thread = CreateThread(NULL, 0, wasapi_playback_thread, e, 0, NULL);
  if (!e->thread) {
    free(e);
    return NULL;
  }
#else
  atomic_init(&e->stop_flag, false);
  if (pthread_create(&e->thread, NULL, pa_playback_thread, e) != 0) {
    free(e);
    return NULL;
  }
  e->thread_started = true;
#endif
  return e;
}

void zc_audio_engine_stop(zc_audio_engine_t *eng) {
  if (!eng) return;
#ifdef _WIN32
  InterlockedExchange(&eng->stop_flag, 1);
  if (eng->thread) {
    WaitForSingleObject(eng->thread, 3000);
    CloseHandle(eng->thread);
    eng->thread = NULL;
  }
#else
  atomic_store_explicit(&eng->stop_flag, true, memory_order_release);
  if (eng->thread_started) {
    pthread_join(eng->thread, NULL);
    eng->thread_started = false;
  }
#endif
  free(eng);
}

/* =========================================================================
 * Device Enumeration (--list-devices)
 * ========================================================================= */

#ifdef _WIN32
static void print_wasapi_endpoints(FILE *out, IMMDeviceEnumerator *en, EDataFlow flow,
                                   const char *tag) {
  IMMDeviceCollection *coll = NULL;
  if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, flow, DEVICE_STATE_ACTIVE, &coll)) ||
      !coll) {
    return;
  }
  UINT cnt = 0;
  IMMDeviceCollection_GetCount(coll, &cnt);
  for (UINT i = 0; i < cnt; ++i) {
    IMMDevice *dev = NULL;
    if (FAILED(IMMDeviceCollection_Item(coll, i, &dev)) || !dev) continue;
    char name[256] = "Audio Device";
    IPropertyStore *props = NULL;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props)) && props) {
      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (SUCCEEDED(IPropertyStore_GetValue(props, &ZC_PKEY_Device_FriendlyName, &pv)) &&
          pv.vt == VT_LPWSTR && pv.pwszVal) {
        WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof(name), NULL, NULL);
        win_sanitize_field(name);
      }
      PropVariantClear(&pv);
      IPropertyStore_Release(props);
    }
    fprintf(out, "%s|%u|%s\n", tag, (unsigned)i, name);
    IMMDevice_Release(dev);
  }
  IMMDeviceCollection_Release(coll);
}
#endif

void zc_periph_print_devices(FILE *out) {
  if (!out) out = stdout;

#ifdef _WIN32
  zc_mon_print_t mp = {out, 0};
  EnumDisplayMonitors(NULL, NULL, win_mon_print_cb, (LPARAM)&mp);
  if (mp.idx == 0) {
    fprintf(out, "DISPLAY|0|Primary Display|%dx%d|0,0\n",
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
  }

  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  IMMDeviceEnumerator *en = NULL;
  if (SUCCEEDED(CoCreateInstance(&ZC_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                 &ZC_IID_IMMDeviceEnumerator, (void **)&en)) &&
      en) {
    print_wasapi_endpoints(out, en, eRender, "AUDIO_OUT");
    print_wasapi_endpoints(out, en, eCapture, "AUDIO_IN");
    IMMDeviceEnumerator_Release(en);
  }
  CoUninitialize();

  zc_gamepad_poller_t *gp = zc_gamepad_poller_create();
  if (gp) {
    zc_gamepad_state_t st[ZC_MAX_GAMEPADS];
    bool chg[ZC_MAX_GAMEPADS];
    zc_gamepad_poller_poll(gp, st, chg);
    for (uint32_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
      if (st[i].connected) {
        fprintf(out, "GAMEPAD|%u|XInput Controller #%u\n", (unsigned)i, (unsigned)(i + 1U));
      }
    }
    zc_gamepad_poller_destroy(gp);
  }
#else
  bool printed_disp = false;
  void *xlib = dlopen("libX11.so.6", RTLD_LAZY);
  if (xlib) {
    pfn_xopen_dpy_t p_open = NULL;
    pfn_xclose_dpy_t p_close = NULL;
    pfn_xdef_scr_t p_scr = NULL;
    pfn_xdisp_w_t p_dw = NULL;
    pfn_xdisp_h_t p_dh = NULL;
    zc_dlsym_copy(xlib, &p_open, sizeof(p_open), "XOpenDisplay");
    zc_dlsym_copy(xlib, &p_close, sizeof(p_close), "XCloseDisplay");
    zc_dlsym_copy(xlib, &p_scr, sizeof(p_scr), "XDefaultScreen");
    zc_dlsym_copy(xlib, &p_dw, sizeof(p_dw), "XDisplayWidth");
    zc_dlsym_copy(xlib, &p_dh, sizeof(p_dh), "XDisplayHeight");
    if (p_open && p_close && p_scr && p_dw && p_dh) {
      zc_x11_dpy_t *d = p_open(NULL);
      if (d) {
        int s = p_scr(d);
        fprintf(out, "DISPLAY|0|X11 Screen %d|%dx%d|0,0\n", s, p_dw(d, s), p_dh(d, s));
        printed_disp = true;
        p_close(d);
      }
    }
    dlclose(xlib);
  }
  if (!printed_disp) {
    fprintf(out, "DISPLAY|0|Virtual Display (Headless/Wayland)|1920x1080|0,0\n");
  }

  bool any_out = false;
  FILE *fp_sinks = popen("pactl list short sinks 2>/dev/null", "r");
  if (fp_sinks) {
    char line[512];
    while (fgets(line, (int)sizeof(line), fp_sinks)) {
      unsigned id = 0;
      char name[256] = "";
      if (sscanf(line, "%u\t%255[^\t\n]", &id, name) == 2) {
        fprintf(out, "AUDIO_OUT|%s|%s\n", name, name);
        any_out = true;
      }
    }
    pclose(fp_sinks);
  }
  if (!any_out) fprintf(out, "AUDIO_OUT|default|Sortie système par défaut\n");

  bool any_in = false;
  FILE *fp_src = popen("pactl list short sources 2>/dev/null", "r");
  if (fp_src) {
    char line[512];
    while (fgets(line, (int)sizeof(line), fp_src)) {
      unsigned id = 0;
      char name[256] = "";
      if (sscanf(line, "%u\t%255[^\t\n]", &id, name) == 2) {
        if (!strstr(name, ".monitor")) {
          fprintf(out, "AUDIO_IN|%s|%s\n", name, name);
          any_in = true;
        }
      }
    }
    pclose(fp_src);
  }
  if (!any_in) fprintf(out, "AUDIO_IN|default|Microphone par défaut\n");

#ifdef __linux__
  for (uint32_t i = 0; i < ZC_MAX_GAMEPADS; ++i) {
    char devpath[32];
    snprintf(devpath, sizeof(devpath), "/dev/input/js%u", (unsigned)i);
    int fd = open(devpath, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
      char gname[128] = "Linux Gamepad";
      (void)ioctl(fd, JSIOCGNAME(sizeof(gname)), gname);
      for (char *q = gname; *q; ++q) {
        if (*q == '|' || *q == '\n' || *q == '\r') *q = ' ';
      }
      fprintf(out, "GAMEPAD|%u|%s\n", (unsigned)i, gname);
      close(fd);
    }
  }
#endif
#endif
  fflush(out);
}
