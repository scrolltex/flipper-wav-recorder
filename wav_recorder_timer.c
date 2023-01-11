#include <furi.h>
#include <furi_hal.h>
#include "wav_recorder_timer.h"

#define SAMPLE_RATE_TIMER TIM2
#define SAMPLE_RATE_TIMER_CHANNEL LL_TIM_CHANNEL_CH3
#define SAMPLE_RATE_TIMER_IRQ FuriHalInterruptIdTIM2

typedef struct {
    RecorderSamplingCallback callback;
    void* context;
} RecorderSampling;

RecorderSampling* recorder_sampling = NULL;

static void wav_recorder_sampling_isr(void* context) {
    UNUSED(context);
    if(LL_TIM_IsActiveFlag_UPDATE(SAMPLE_RATE_TIMER)) {
        LL_TIM_ClearFlag_UPDATE(SAMPLE_RATE_TIMER);
        recorder_sampling->callback(recorder_sampling->context);
    }
}

void wav_recorder_sampling_init(uint32_t sample_rate) {
    furi_assert(recorder_sampling == NULL);
    furi_check(sample_rate > 0);

    recorder_sampling = malloc(sizeof(RecorderSampling));

    FURI_CRITICAL_ENTER();
    LL_TIM_DeInit(SAMPLE_RATE_TIMER);
    FURI_CRITICAL_EXIT();

    LL_TIM_InitTypeDef TIM_InitStruct = {0};
    TIM_InitStruct.Prescaler = 0;
    //TIM_InitStruct.Autoreload = 1451; //64 000 000 / 1451 ~= 44100 Hz
    TIM_InitStruct.Autoreload = SystemCoreClock / sample_rate - 1;
    LL_TIM_Init(SAMPLE_RATE_TIMER, &TIM_InitStruct);
    LL_TIM_DisableARRPreload(SAMPLE_RATE_TIMER);

    LL_TIM_OC_InitTypeDef TIM_OC_InitStruct = {0};
    TIM_OC_InitStruct.OCMode = LL_TIM_OCMODE_PWM1;
    TIM_OC_InitStruct.OCState = LL_TIM_OCSTATE_ENABLE;
    TIM_OC_InitStruct.CompareValue = 1;
    LL_TIM_OC_Init(SAMPLE_RATE_TIMER, SAMPLE_RATE_TIMER_CHANNEL, &TIM_OC_InitStruct);
}

void wav_recorder_sampling_start(RecorderSamplingCallback callback, void* context) {
    furi_assert(recorder_sampling);

    recorder_sampling->callback = callback;
    recorder_sampling->context = context;

    furi_hal_interrupt_set_isr(SAMPLE_RATE_TIMER_IRQ, wav_recorder_sampling_isr, NULL);

    LL_TIM_EnableIT_UPDATE(SAMPLE_RATE_TIMER);
    LL_TIM_EnableAllOutputs(SAMPLE_RATE_TIMER);
    LL_TIM_EnableCounter(SAMPLE_RATE_TIMER);
}

void wav_recorder_sampling_stop() {
    LL_TIM_DisableCounter(SAMPLE_RATE_TIMER);
    LL_TIM_DisableAllOutputs(SAMPLE_RATE_TIMER);
    furi_hal_interrupt_set_isr(SAMPLE_RATE_TIMER_IRQ, NULL, NULL);
}

void wav_recorder_sampling_deinit() {
    furi_hal_interrupt_set_isr(SAMPLE_RATE_TIMER_IRQ, NULL, NULL);

    FURI_CRITICAL_ENTER();
    LL_TIM_DeInit(SAMPLE_RATE_TIMER);
    FURI_CRITICAL_EXIT();

    free(recorder_sampling);
}
