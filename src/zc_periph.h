#ifndef ZC_PERIPH_H
#define ZC_PERIPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifndef _WIN32
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Wire Formats (Little-Endian, compact for RUDP/UDP low latency)
 * ========================================================================= */

#define ZC_INPUT_MAGIC "ZCI1"
#define ZC_INPUT_PKT_SIZE 20U

typedef enum {
  ZC_INPUT_KEY = 1,
  ZC_INPUT_MOUSE_ABS = 2,
  ZC_INPUT_MOUSE_REL = 3,
  ZC_INPUT_MOUSE_BTN = 4,
  ZC_INPUT_MOUSE_WHEEL = 5
} zc_input_kind_t;

#define ZC_INPUT_FLAG_DOWN 0x01U
#define ZC_INPUT_FLAG_EXTENDED 0x02U
#define ZC_INPUT_FLAG_HORIZ 0x02U

#define ZC_MOUSE_BTN_LEFT 1U
#define ZC_MOUSE_BTN_RIGHT 2U
#define ZC_MOUSE_BTN_MIDDLE 3U
#define ZC_MOUSE_BTN_X1 4U
#define ZC_MOUSE_BTN_X2 5U

typedef struct {
  uint8_t kind;   /* zc_input_kind_t */
  uint8_t flags;  /* bit 0 = pressed/down, bit 1 = extended scancode / horiz wheel */
  uint16_t code;  /* AT Set 1 scancode (KEY) or ZC_MOUSE_BTN_* (BTN) */
  int32_t x;      /* ABS: 0..65535 normalized; REL: dx; WHEEL: hdelta */
  int32_t y;      /* ABS: 0..65535 normalized; REL: dy; WHEEL: vdelta (120/notch) */
  uint32_t seq;   /* Monotonic sender sequence */
} zc_input_event_t;

#define ZC_GAMEPAD_MAGIC "ZCGP"
#define ZC_GAMEPAD_PKT_SIZE 24U
#define ZC_MAX_GAMEPADS 4U

/* XInput-compatible button bitmask */
#define ZC_GP_DPAD_UP 0x0001U
#define ZC_GP_DPAD_DOWN 0x0002U
#define ZC_GP_DPAD_LEFT 0x0004U
#define ZC_GP_DPAD_RIGHT 0x0008U
#define ZC_GP_START 0x0010U
#define ZC_GP_BACK 0x0020U
#define ZC_GP_LEFT_THUMB 0x0040U
#define ZC_GP_RIGHT_THUMB 0x0080U
#define ZC_GP_LEFT_SHOULDER 0x0100U
#define ZC_GP_RIGHT_SHOULDER 0x0200U
#define ZC_GP_A 0x1000U
#define ZC_GP_B 0x2000U
#define ZC_GP_X 0x4000U
#define ZC_GP_Y 0x8000U

typedef struct {
  uint8_t index;     /* 0..3 */
  uint8_t connected; /* 0 or 1 */
  uint16_t buttons;  /* ZC_GP_* bitmask */
  uint8_t lt;        /* 0..255 */
  uint8_t rt;        /* 0..255 */
  int16_t lx;        /* -32768..32767 */
  int16_t ly;
  int16_t rx;
  int16_t ry;
  uint32_t seq;
} zc_gamepad_state_t;

#define ZC_AUDIO_OUT_MAGIC "ZCA0"
#define ZC_AUDIO_MIC_MAGIC "ZCM0"
#define ZC_AUDIO_HDR_SIZE 16U
#define ZC_AUDIO_SAMPLE_RATE 48000U
#define ZC_AUDIO_FRAME_SAMPLES 480U /* 10 ms @ 48 kHz */
#define ZC_AUDIO_CODEC_ADPCM 1U
#define ZC_AUDIO_MAX_PKT_SIZE 1200U

typedef struct {
  char magic[4]; /* ZCA0 or ZCM0 */
  uint32_t seq;
  uint32_t sample_rate;
  uint8_t channels;        /* 1 or 2 */
  uint8_t codec;           /* ZC_AUDIO_CODEC_ADPCM */
  uint16_t samples_per_ch; /* typically 480 */
  uint16_t payload_bytes;  /* bytes following 16-byte header */
} zc_audio_hdr_t;

/* Pack / unpack helpers */
int zc_input_pack(const zc_input_event_t *ev, uint8_t out[ZC_INPUT_PKT_SIZE]);
int zc_input_unpack(const uint8_t *buf, size_t len, zc_input_event_t *out);

int zc_gamepad_pack(const zc_gamepad_state_t *gp, uint8_t out[ZC_GAMEPAD_PKT_SIZE]);
int zc_gamepad_unpack(const uint8_t *buf, size_t len, zc_gamepad_state_t *out);

/* Scancode translation (Linux evdev <-> AT Set 1) */
bool zc_scancode_evdev_to_set1(uint16_t evdev, uint16_t *out_set1, bool *out_ext);
bool zc_scancode_set1_to_evdev(uint16_t set1, bool ext, uint16_t *out_evdev);

/* =========================================================================
 * Audio Codec (IMA-ADPCM 48 kHz 16-bit) & Lock-Free SPSC PCM Ring Buffer
 * ========================================================================= */

typedef struct {
  int16_t prev_sample;
  uint8_t index;
  uint8_t reserved;
} zc_adpcm_state_t;

int zc_audio_encode_packet(const char magic[4], uint32_t seq, uint8_t channels,
                           uint16_t samples_per_ch, const int16_t *pcm,
                           zc_adpcm_state_t state[2], uint8_t *out_pkt,
                           size_t out_cap);

int zc_audio_decode_packet(const uint8_t *pkt, size_t pkt_len,
                           zc_audio_hdr_t *out_hdr, int16_t *out_pcm,
                           size_t max_samples_total);

#define ZC_AUDIO_RING_SAMPLES 16384U /* power of 2 (~170ms @ 48kHz stereo) */

typedef struct {
  int16_t data[ZC_AUDIO_RING_SAMPLES];
#ifdef _WIN32
  volatile long write_pos;
  volatile long read_pos;
#else
  _Atomic uint32_t write_pos;
  _Atomic uint32_t read_pos;
#endif
} zc_audio_ring_t;

void zc_audio_ring_init(zc_audio_ring_t *ring);
void zc_audio_ring_reset(zc_audio_ring_t *ring);
size_t zc_audio_ring_available(const zc_audio_ring_t *ring);
size_t zc_audio_ring_push(zc_audio_ring_t *ring, const int16_t *samples, size_t count);
size_t zc_audio_ring_pop(zc_audio_ring_t *ring, int16_t *out_samples, size_t count);

/* =========================================================================
 * Host Input / Gamepad Injector (Windows SendInput/ViGEm, Linux XTest/uinput)
 * ========================================================================= */

typedef struct zc_input_injector_s zc_input_injector_t;

zc_input_injector_t *zc_input_injector_create(uint32_t display_idx,
                                              bool enable_kb_mouse,
                                              bool enable_gamepad);
void zc_input_injector_destroy(zc_input_injector_t *inj);
int zc_input_injector_apply_input(zc_input_injector_t *inj, const zc_input_event_t *ev);
int zc_input_injector_apply_gamepad(zc_input_injector_t *inj, const zc_gamepad_state_t *gp);

/* =========================================================================
 * Client Gamepad Poller (Windows XInput, Linux /dev/input/js*)
 * ========================================================================= */

typedef struct zc_gamepad_poller_s zc_gamepad_poller_t;

zc_gamepad_poller_t *zc_gamepad_poller_create(void);
void zc_gamepad_poller_destroy(zc_gamepad_poller_t *poller);
int zc_gamepad_poller_poll(zc_gamepad_poller_t *poller,
                           zc_gamepad_state_t out_states[ZC_MAX_GAMEPADS],
                           bool out_changed[ZC_MAX_GAMEPADS]);

/* =========================================================================
 * Bidirectional Audio Capture & Playback Engine (WASAPI / PulseAudio)
 * ========================================================================= */

typedef struct zc_audio_engine_s zc_audio_engine_t;

typedef void (*zc_audio_pkt_cb_t)(const uint8_t *pkt, size_t pkt_len, void *userdata);

zc_audio_engine_t *zc_audio_capture_start(bool loopback,
                                          uint8_t channels,
                                          const char *device_id,
                                          const char magic[4],
                                          zc_audio_pkt_cb_t cb,
                                          void *userdata);

zc_audio_engine_t *zc_audio_playback_start(uint8_t channels,
                                           const char *device_id,
                                           zc_audio_ring_t *ring);

void zc_audio_engine_stop(zc_audio_engine_t *eng);

/* =========================================================================
 * Device Enumeration (--list-devices)
 * ========================================================================= */

void zc_periph_print_devices(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* ZC_PERIPH_H */
