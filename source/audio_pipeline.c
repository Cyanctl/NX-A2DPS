#include "audio_pipeline.h"
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

static void push(AudioPipeline *a, int16_t left, int16_t right) {
    if (a->write - a->read == AUDIO_RING_FRAMES) { a->read++; a->overruns++; }
    size_t index = (a->write++ % AUDIO_RING_FRAMES) * 2;
    a->ring[index] = left; a->ring[index + 1] = right;
}

void audio_pipeline_input(void *context, const int16_t *pcm, size_t frames, unsigned rate, unsigned channels) {
    AudioPipeline *a = context;
    if (!a->initialized || (rate != 44100 && rate != 48000) || channels < 1 || channels > 2) return;
    if (a->rate != rate) { a->have_previous = false; a->rate = rate; }
    a->have_audio = true;
    for (size_t i = 0; i < frames; i++) {
        int16_t current[2] = { pcm[i * channels], pcm[i * channels + (channels == 2)] };
        if (!a->have_previous) {
            push(a, current[0], current[1]);
            a->phase = rate; a->have_previous = true;
        } else {
            while (a->phase <= 48000) {
                int16_t out[2];
                for (unsigned c = 0; c < 2; c++)
                    out[c] = a->previous[c] + ((int64_t)(current[c] - a->previous[c]) * a->phase) / 48000;
                push(a, out[0], out[1]);
                a->phase += rate;
            }
            a->phase -= 48000;
        }
        a->previous[0] = current[0]; a->previous[1] = current[1];
    }
}

static void fill(AudioPipeline *a, AudioOutBuffer *buffer) {
    int16_t *pcm = buffer->buffer;
    bool short_buffer = false;
    for (size_t i = 0; i < AUDIO_BLOCK_FRAMES; i++) {
        if (a->read != a->write) {
            size_t at = (a->read++ % AUDIO_RING_FRAMES) * 2;
            pcm[2 * i] = a->ring[at]; pcm[2 * i + 1] = a->ring[at + 1];
            a->played++;
        } else {
            pcm[2 * i] = pcm[2 * i + 1] = 0;
            short_buffer = true;
        }
    }
    if (short_buffer && a->have_audio) a->underruns++;
}

void audio_pipeline_clear(AudioPipeline *a) {
    a->read = a->write;
    a->have_previous = a->have_audio = false;
    a->rate = 0;
}

Result audio_pipeline_init(AudioPipeline *a) {
    memset(a, 0, sizeof(*a));
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) return rc;
    a->initialized = true;
    if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 || audoutGetPcmFormat() != PcmFormat_Int16) {
        audio_pipeline_finish(a);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    for (unsigned i = 0; i < 4; i++) {
        AudioOutBuffer *b = &a->buffers[i];
        b->buffer = memalign(4096, 4096);
        if (!b->buffer) { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); break; }
        b->buffer_size = 4096;
        b->data_size = AUDIO_BLOCK_FRAMES * 4;
        fill(a, b);
        rc = audoutAppendAudioOutBuffer(b);
        if (R_FAILED(rc)) break;
    }
    if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
    if (R_FAILED(rc)) audio_pipeline_finish(a);
    return rc;
}

void audio_pipeline_finish(AudioPipeline *a) {
    if (!a->initialized) return;
    audoutStopAudioOut();
    audoutExit();
    for (unsigned i = 0; i < 4; i++) { free(a->buffers[i].buffer); a->buffers[i].buffer = NULL; }
    a->initialized = false;
}

Result audio_pipeline_pump(AudioPipeline *a) {
    if (!a->initialized) return 0;
    for (unsigned i = 0; i < 4; i++) {
        AudioOutBuffer *released = NULL;
        u32 count = 0;
        Result rc = audoutGetReleasedAudioOutBuffer(&released, &count);
        if (R_FAILED(rc)) return rc;
        if (!count) break;
        if (!released) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        fill(a, released);
        rc = audoutAppendAudioOutBuffer(released);
        if (R_FAILED(rc)) return rc;
    }
    return 0;
}
