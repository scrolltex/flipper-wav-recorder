#pragma once

typedef void (*RecorderSamplingCallback)(void* context);

extern void wav_recorder_sampling_init(uint32_t sample_rate);

extern void wav_recorder_sampling_start(RecorderSamplingCallback callback, void* context);

extern void wav_recorder_sampling_stop();

extern void wav_recorder_sampling_deinit();
