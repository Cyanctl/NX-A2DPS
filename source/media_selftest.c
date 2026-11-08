#include "sbc_receiver.h"
#include <string.h>

typedef struct { size_t frames; unsigned rate, channels; uint64_t energy; } Capture;
static void capture(void *context, const int16_t *pcm, size_t frames, unsigned rate, unsigned channels) {
    Capture *c = context;
    c->frames += frames;
    c->rate = rate;
    c->channels = channels;
    for (size_t i = 0; i < frames * channels; i++) {
        int sample = pcm[i];
        c->energy += sample < 0 ? -sample : sample;
    }
}

static size_t packet(uint8_t *p, unsigned sequence, unsigned header, const uint8_t *frame, size_t length) {
    memset(p, 0, 13);
    p[0] = 0x80; p[1] = 96;
    p[2] = (uint8_t)(sequence >> 8); p[3] = (uint8_t)sequence;
    p[12] = (uint8_t)header;
    memcpy(p + 13, frame, length);
    return length + 13;
}

int sbc_receiver_selftest(void) {
    int failures = 0;
    sbc_t encoder;
    if (sbc_init(&encoder, 0) < 0) return 1;
    encoder.frequency = SBC_FREQ_48000;
    encoder.mode = SBC_MODE_JOINT_STEREO;
    encoder.blocks = SBC_BLK_16;
    encoder.subbands = SBC_SB_8;
    encoder.allocation = SBC_AM_LOUDNESS;
    encoder.bitpool = 35;
    encoder.endian = SBC_LE;
    int16_t pcm[256];
    for (unsigned i = 0; i < 256; i++) pcm[i] = ((i / 16) & 1) ? 1000 : -1000;
    uint8_t frame[1024];
    ssize_t length = 0;
    ssize_t consumed = sbc_encode(&encoder, pcm, sizeof(pcm), frame, sizeof(frame), &length);
    sbc_finish(&encoder);
    if (consumed != sizeof(pcm) || length <= 0 || length > 500) return 1;
    size_t nframe = (size_t)length;
    uint8_t p[2100];
    SbcReceiver rx;
    Capture c = {0};
    const uint8_t config[4] = {0x11, 0x15, 2, 53};
    if (sbc_receiver_init(&rx, config, capture, &c)) return 1;
#define CHECK(condition) do { if (!(condition)) failures++; } while (0)
    size_t n = packet(p, 65535, 1, frame, nframe);
    CHECK(sbc_receiver_packet(&rx, p, n) == 0);
    CHECK(c.frames == 128 && c.rate == 48000 && c.channels == 2 && c.energy);
    n = packet(p, 0, 1, frame, nframe);
    CHECK(sbc_receiver_packet(&rx, p, n) == 0); // Sequence wrap.
    CHECK(sbc_receiver_packet(&rx, p, n) == -2); // Duplicate.
    CHECK(c.frames == 256);
    n = packet(p, 1, 2, frame, nframe);
    memcpy(p + n, frame, nframe); n += nframe;
    CHECK(sbc_receiver_packet(&rx, p, n) == 0);
    CHECK(c.frames == 512);
    n = packet(p, 2, 2, frame, nframe); // Truncated second frame.
    CHECK(sbc_receiver_packet(&rx, p, n) == -1 && c.frames == 512);
    n = packet(p, 3, 1, frame, nframe);
    p[16] ^= 1; // Corrupt SBC header CRC.
    CHECK(sbc_receiver_packet(&rx, p, n) == -1 && c.frames == 512);
    n = packet(p, 4, 1, frame, nframe);
    p[14] ^= 0x40; // Codec contradicts negotiated sampling rate.
    CHECK(sbc_receiver_packet(&rx, p, n) == -1);
    // Fragment assembly, and loss must discard a partial frame.
    n = packet(p, 5, 0xc0, frame, nframe / 2);
    CHECK(sbc_receiver_packet(&rx, p, n) == 0 && c.frames == 512);
    n = packet(p, 6, 0xa0, frame + nframe / 2, nframe - nframe / 2);
    CHECK(sbc_receiver_packet(&rx, p, n) == 0 && c.frames == 640);
    n = packet(p, 7, 0xc0, frame, nframe / 2);
    CHECK(sbc_receiver_packet(&rx, p, n) == 0);
    n = packet(p, 9, 0xa0, frame + nframe / 2, nframe - nframe / 2);
    CHECK(sbc_receiver_packet(&rx, p, n) == -1 && c.frames == 640 && rx.discontinuities == 1);
    // CSRC + RTP extension + padding, followed by one SBC frame.
    memset(p, 0, sizeof(p));
    p[0] = 0xb1; p[1] = 96; p[3] = 10; p[19] = 1; p[24] = 1;
    memcpy(p + 25, frame, nframe); p[25 + nframe + 3] = 4;
    CHECK(sbc_receiver_packet(&rx, p, 25 + nframe + 4) == 0 && c.frames == 768);
    for (size_t size = 0; size < 13; size++) CHECK(sbc_receiver_packet(&rx, p, size) == -1);
    p[18] = 0xff; p[19] = 0xff; // Extension-length overflow attempt.
    CHECK(sbc_receiver_packet(&rx, p, 25 + nframe + 4) == -1);
    sbc_receiver_finish(&rx);
    // Native AVDTP strips RTP before invoking its data callback.
    Capture native = {0};
    if (sbc_receiver_init(&rx, config, capture, &native)) return failures + 1;
    p[0] = 1; memcpy(p + 1, frame, nframe);
    CHECK(sbc_receiver_payload(&rx, 65535, p, nframe + 1) == 0);
    CHECK(sbc_receiver_payload(&rx, 0, p, nframe + 1) == 0);
    CHECK(sbc_receiver_payload(&rx, 0, p, nframe + 1) == -2);
    CHECK(native.frames == 256 && native.rate == 48000 && native.channels == 2);
    sbc_receiver_finish(&rx);
    uint8_t bad[4] = {0x31, 0x15, 2, 53};
    CHECK(sbc_receiver_init(&rx, bad, capture, &c) == -1); // Multiple frequencies.
    // The second mandatory music sampling rate must decode as well.
    if (sbc_init(&encoder, 0) < 0) return failures + 1;
    encoder.frequency = SBC_FREQ_44100; encoder.mode = SBC_MODE_MONO;
    encoder.blocks = SBC_BLK_16; encoder.subbands = SBC_SB_8;
    encoder.allocation = SBC_AM_LOUDNESS; encoder.bitpool = 32; encoder.endian = SBC_LE;
    consumed = sbc_encode(&encoder, pcm, 256, frame, sizeof(frame), &length);
    sbc_finish(&encoder);
    CHECK(consumed == 256 && length > 0 && length < 500);
    const uint8_t mono[4] = {0x28, 0x15, 2, 53};
    if (sbc_receiver_init(&rx, mono, capture, &c)) return failures + 1;
    if (length > 0 && length < 500) {
        n = packet(p, 0, 1, frame, (size_t)length);
        CHECK(sbc_receiver_packet(&rx, p, n) == 0 && c.rate == 44100 && c.channels == 1 && c.frames == 896);
    }
    // Deterministic malformed-input smoke test on the real ARM64 build.
    uint32_t random = 0x18273465;
    for (unsigned i = 0; i < 4096; i++) {
        size_t size = i % 200;
        for (size_t j = 0; j < size; j++) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            p[j] = (uint8_t)random;
        }
        int rc = sbc_receiver_packet(&rx, p, size);
        CHECK(rc >= -2 && rc <= 0);
    }
    sbc_receiver_finish(&rx);
#undef CHECK
    return failures;
}
