#pragma once
#include <switch.h>
#define AUDIO_RING_FRAMES 16384
#define AUDIO_BLOCK_FRAMES 960
typedef struct {
    int16_t ring[AUDIO_RING_FRAMES * 2];
    uint64_t read, write, underruns, overruns, played;
    AudioOutBuffer buffers[4];
    int16_t previous[2];
    unsigned phase, rate;
    bool initialized, have_previous, have_audio;
} AudioPipeline;
Result audio_pipeline_init(AudioPipeline *);
void audio_pipeline_finish(AudioPipeline *);
void audio_pipeline_clear(AudioPipeline *);
void audio_pipeline_input(void *, const int16_t *, size_t frames, unsigned rate, unsigned channels);
Result audio_pipeline_pump(AudioPipeline *);
int audio_pipeline_selftest(void);
