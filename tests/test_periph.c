#include "zc_periph.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_input_wire_roundtrip(void) {
  zc_input_event_t ev;
  ev.kind = ZC_INPUT_KEY;
  ev.flags = (uint8_t)(ZC_INPUT_FLAG_DOWN | ZC_INPUT_FLAG_EXTENDED);
  ev.code = 0x48U; /* Arrow Up */
  ev.x = -123;
  ev.y = 45678;
  ev.seq = 0x12345678U;

  uint8_t buf[ZC_INPUT_PKT_SIZE];
  assert(zc_input_pack(&ev, buf) == (int)ZC_INPUT_PKT_SIZE);
  assert(memcmp(buf, ZC_INPUT_MAGIC, 4) == 0);

  zc_input_event_t dec;
  memset(&dec, 0, sizeof(dec));
  assert(zc_input_unpack(buf, sizeof(buf), &dec) == 0);
  assert(dec.kind == ev.kind);
  assert(dec.flags == ev.flags);
  assert(dec.code == ev.code);
  assert(dec.x == ev.x);
  assert(dec.y == ev.y);
  assert(dec.seq == ev.seq);

  /* Short or corrupted magic must fail */
  assert(zc_input_unpack(buf, ZC_INPUT_PKT_SIZE - 1U, &dec) < 0);
  buf[0] = 'X';
  assert(zc_input_unpack(buf, sizeof(buf), &dec) < 0);
}

static void test_gamepad_wire_roundtrip(void) {
  zc_gamepad_state_t gp;
  memset(&gp, 0, sizeof(gp));
  gp.index = 2U;
  gp.connected = 1U;
  gp.buttons = ZC_GP_A | ZC_GP_RIGHT_SHOULDER | ZC_GP_START;
  gp.lt = 200U;
  gp.rt = 99U;
  gp.lx = -18000;
  gp.ly = 31000;
  gp.rx = 1234;
  gp.ry = -4321;
  gp.seq = 777U;

  uint8_t buf[ZC_GAMEPAD_PKT_SIZE];
  assert(zc_gamepad_pack(&gp, buf) == (int)ZC_GAMEPAD_PKT_SIZE);

  zc_gamepad_state_t out;
  memset(&out, 0, sizeof(out));
  assert(zc_gamepad_unpack(buf, sizeof(buf), &out) == 0);
  assert(out.index == gp.index);
  assert(out.connected == 1U);
  assert(out.buttons == gp.buttons);
  assert(out.lt == 200U && out.rt == 99U);
  assert(out.lx == -18000 && out.ly == 31000);
  assert(out.rx == 1234 && out.ry == -4321);
  assert(out.seq == 777U);
}

static void test_scancode_mapping(void) {
  uint16_t set1 = 0, evdev = 0;
  bool ext = false;

  /* Letter W (evdev 17 -> Set 1 0x11, not extended) */
  assert(zc_scancode_evdev_to_set1(17U, &set1, &ext));
  assert(set1 == 0x11U && !ext);
  assert(zc_scancode_set1_to_evdev(set1, ext, &evdev) && evdev == 17U);

  /* Arrow Up (evdev 103 -> Set 1 0x48, extended) */
  assert(zc_scancode_evdev_to_set1(103U, &set1, &ext));
  assert(set1 == 0x48U && ext);
  assert(zc_scancode_set1_to_evdev(set1, ext, &evdev) && evdev == 103U);

  /* Left Meta (evdev 125 -> Set 1 0x5B, extended) */
  assert(zc_scancode_evdev_to_set1(125U, &set1, &ext));
  assert(set1 == 0x5BU && ext);
  assert(zc_scancode_set1_to_evdev(set1, ext, &evdev) && evdev == 125U);
}

static void test_adpcm_codec_and_ring(void) {
  int16_t src[ZC_AUDIO_FRAME_SAMPLES * 2U];
  int16_t dec[ZC_AUDIO_FRAME_SAMPLES * 2U];
  for (size_t i = 0; i < ZC_AUDIO_FRAME_SAMPLES; ++i) {
    double th = (double)i * (2.0 * 3.14159265 * 440.0 / 48000.0);
    src[i * 2U + 0U] = (int16_t)(12000.0 * sin(th));
    src[i * 2U + 1U] = (int16_t)(8000.0 * cos(th));
  }

  zc_adpcm_state_t enc_st[2];
  memset(enc_st, 0, sizeof(enc_st));
  uint8_t pkt[ZC_AUDIO_MAX_PKT_SIZE];
  int plen = zc_audio_encode_packet(ZC_AUDIO_OUT_MAGIC, 42U, 2U,
                                    ZC_AUDIO_FRAME_SAMPLES, src, enc_st, pkt,
                                    sizeof(pkt));
  assert(plen == 504);

  zc_audio_hdr_t hdr;
  memset(&hdr, 0, sizeof(hdr));
  int dsamples = zc_audio_decode_packet(pkt, (size_t)plen, &hdr, dec,
                                        ZC_AUDIO_FRAME_SAMPLES * 2U);
  assert(dsamples == (int)ZC_AUDIO_FRAME_SAMPLES);
  assert(hdr.seq == 42U && hdr.channels == 2U && hdr.sample_rate == 48000U);

  double err2 = 0.0;
  for (size_t i = 0; i < ZC_AUDIO_FRAME_SAMPLES * 2U; ++i) {
    double d = (double)src[i] - (double)dec[i];
    err2 += d * d;
  }
  double rmse = sqrt(err2 / (double)(ZC_AUDIO_FRAME_SAMPLES * 2U));
  assert(rmse < 600.0);

  zc_audio_ring_t *ring = (zc_audio_ring_t *)calloc(1, sizeof(*ring));
  assert(ring != NULL);
  zc_audio_ring_init(ring);
  assert(zc_audio_ring_available(ring) == 0U);
  assert(zc_audio_ring_push(ring, dec, ZC_AUDIO_FRAME_SAMPLES * 2U) ==
         ZC_AUDIO_FRAME_SAMPLES * 2U);
  assert(zc_audio_ring_available(ring) == ZC_AUDIO_FRAME_SAMPLES * 2U);

  int16_t popped[ZC_AUDIO_FRAME_SAMPLES * 2U];
  assert(zc_audio_ring_pop(ring, popped, ZC_AUDIO_FRAME_SAMPLES * 2U) ==
         ZC_AUDIO_FRAME_SAMPLES * 2U);
  assert(memcmp(popped, dec, sizeof(popped)) == 0);
  assert(zc_audio_ring_available(ring) == 0U);
  free(ring);
}

static void test_device_enumeration_smoke(void) {
  FILE *tmp = tmpfile();
  if (tmp) {
    zc_periph_print_devices(tmp);
    rewind(tmp);
    char line[256];
    bool saw_display = false;
    while (fgets(line, (int)sizeof(line), tmp)) {
      if (strncmp(line, "DISPLAY|", 8) == 0) saw_display = true;
    }
    assert(saw_display);
    fclose(tmp);
  }
}

int main(void) {
  test_input_wire_roundtrip();
  test_gamepad_wire_roundtrip();
  test_scancode_mapping();
  test_adpcm_codec_and_ring();
  test_device_enumeration_smoke();
  printf("test_periph passed (wire + scancodes + ADPCM + SPSC ring + enumeration).\n");
  return 0;
}
