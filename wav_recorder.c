#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <locale/locale.h>
#include <wav_recorder_icons.h>
#include "furi_hal_adc.h"
#include "wav_recorder_timer.h"

#define TAG "WavRecorder"

#define APPS_DATA EXT_PATH("apps_data")
#define WAVRECORDER_FOLDER APPS_DATA "/wav_recorder"

#define BUFFER_COUNT (2048)

typedef enum {
    EventTypeTick,
    EventTypeInput,
} EventType;

typedef struct {
    EventType type;
    union {
        InputEvent input;
    };
} RecorderEvent;

typedef struct {
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    Gui* gui;
    FuriMutex* mutex;
    uint32_t sample_max;
    uint32_t sample_min;
} RecorderApp;

/// 36 + SubChunk2Size
// You Don't know this until you write your data but at a minimum it is 36 for an empty file
uint32_t chunkSize = 36;
///: For PCM == 16, since audioFormat == uint16_t
uint32_t subChunk1Size = 16;
///: For PCM this is 1, other values indicate compression
const uint16_t audioFormat = 1;
///: Mono = 1, Stereo = 2, etc.
const uint16_t numChannels = 1;
///: Sample Rate of file
const uint32_t sampleRate = 11025;
///: SampleRate * NumChannels * BitsPerSample/8
const uint32_t byteRate = sampleRate * 2;
///: The number of byte for one frame NumChannels * BitsPerSample/8
const uint16_t blockAlign = 2;
///: 8 bits = 8, 16 bits = 16
const uint16_t bitsPerSample = 16;
///: == NumSamples * NumChannels * BitsPerSample/8  i.e. number of byte in the data.
uint32_t subChunk2Size = 0; // You Don't know this until you write your data

static void wav_recorder_draw(Canvas* canvas, void* context) {
    furi_assert(context);
    RecorderApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    char str[8];

    snprintf(str, 64, "%lu", app->sample_min);
    canvas_draw_str(canvas, 10, 10, "Min:");
    canvas_draw_str(canvas, 40, 10, str);

    snprintf(str, 64, "%lu", app->sample_max);
    canvas_draw_str(canvas, 10, 20, "Max:");
    canvas_draw_str(canvas, 40, 20, str);

    furi_mutex_release(app->mutex);
}

static void wav_recorder_input(InputEvent* event, void* context) {
    furi_assert(context);
    FuriMessageQueue* event_queue = context;
    RecorderEvent app_event = {.type = EventTypeInput, .input = *event};
    furi_message_queue_put(event_queue, &app_event, FuriWaitForever);
}

static void wav_recorder_tick(void* context) {
    furi_assert(context);
    FuriMessageQueue* event_queue = context;

    // For now system was overloaded and skips at least half of samples.
    // TODO: Write to buffer directly and trigger event only for flushing it to storage.

    RecorderEvent app_event = {.type = EventTypeTick};
    furi_message_queue_put(event_queue, &app_event, 0);
}

static void wav_recorder_adc_init() {
    // PC3 is ADC1_IN4
    furi_hal_gpio_init(&gpio_ext_pc3, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    FURI_LOG_I(TAG, "Gpio Set OK");

    furi_hal_adc_init();
    FURI_LOG_I(TAG, "ADC Init OK");

    furi_hal_adc_set_vref(FuriHalVref2500);
    FURI_LOG_I(TAG, "Vref Set OK");

    furi_hal_adc_set_single_channel(FuriHalAdcChannel4);
    FURI_LOG_I(TAG, "ADC Set Channel OK");

    furi_hal_adc_enable();
    FURI_LOG_I(TAG, "ADC Enable OK");
}

static void wav_recorder_adc_deinit() {
    furi_hal_adc_disable();
    FURI_LOG_I(TAG, "ADC Disable OK");

    furi_hal_adc_deinit();
    FURI_LOG_I(TAG, "ADC Deinit OK");
}

static RecorderApp* wav_recorder_alloc() {
    RecorderApp* app = malloc(sizeof(RecorderApp));
    app->sample_max = 0;
    app->sample_min = 4096;

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->event_queue = furi_message_queue_alloc(32, sizeof(RecorderEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, wav_recorder_draw, app);
    view_port_input_callback_set(app->view_port, wav_recorder_input, app->event_queue);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    return app;
}

static void wav_recorder_free(RecorderApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_message_queue_free(app->event_queue);

    furi_mutex_free(app->mutex);
    free(app);
}

static FuriString* wav_recorder_get_file_path() {
    FuriHalRtcDateTime now;
    furi_hal_rtc_get_datetime(&now);

    FuriString* file_path = furi_string_alloc_set(WAVRECORDER_FOLDER);

    FuriString* file_name = furi_string_alloc();
    locale_format_date(file_name, &now, locale_get_date_format(), "_");
    furi_string_cat_printf(file_path, "/%s%s", furi_string_get_cstr(file_name), ".wav");

    furi_string_free(file_name);

    return file_path;
}

void write_wav_header(File* file) {
    furi_assert(file);

    storage_file_seek(file, 0, true);
    storage_file_write(file, "RIFF", 4);
    storage_file_write(file, (void*)&chunkSize, 4);
    storage_file_write(file, "WAVE", 4);
    storage_file_write(file, "fmt ", 4);
    storage_file_write(file, &subChunk1Size, sizeof(uint32_t)); // format chunk size (16 for PCM)
    storage_file_write(file, &audioFormat, sizeof(uint16_t)); // audio format = 1
    storage_file_write(file, &numChannels, sizeof(uint16_t));
    storage_file_write(file, (void*)&sampleRate, sizeof(uint32_t));
    storage_file_write(file, (void*)&byteRate, sizeof(uint32_t));
    storage_file_write(file, (void*)&blockAlign, sizeof(uint16_t));
    storage_file_write(file, (void*)&bitsPerSample, sizeof(uint16_t));
    storage_file_write(file, "data", 4);
    storage_file_write(file, (void*)&subChunk2Size, sizeof(uint32_t));
}

static int32_t
    map(int32_t value, int32_t in_min, int32_t in_max, int32_t out_min, int32_t out_max) {
    return ((((value - in_min) * (out_max - out_min)) / (in_max - in_min)) + out_min);
}

void write_samples_to_file(File* file, int16_t* samples, size_t samples_count) {
    furi_assert(file);
    furi_assert(samples);

    if(samples_count == 0) {
        return;
    }

    subChunk2Size += numChannels * bitsPerSample / 8 * samples_count;
    chunkSize = 36 + subChunk2Size;

    storage_file_seek(file, 4, true);
    storage_file_write(file, (void*)&chunkSize, 4);

    storage_file_seek(file, 40, true);
    storage_file_write(file, (void*)&subChunk2Size, 4);

    storage_file_seek(file, storage_file_size(file) - 1, true);
    storage_file_write(file, samples, sizeof(int16_t) * samples_count);
}

int32_t wav_recorder_app(void* p) {
    UNUSED(p);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_simply_mkdir(storage, APPS_DATA)) {
        FURI_LOG_E(TAG, "Could not create folder %s", APPS_DATA);
        furi_record_close(storage);
        return 0;
    }

    if(!storage_simply_mkdir(storage, WAVRECORDER_FOLDER)) {
        FURI_LOG_E(TAG, "Could not create folder %s", WAVRECORDER_FOLDER);
        furi_record_close(storage);
        return 0;
    }

    FuriString* file_path = wav_recorder_get_file_path();

    File* file = storage_file_alloc(storage);
    bool res =
        storage_file_open(file, furi_string_get_cstr(file_path), FSAM_WRITE, FSOM_CREATE_ALWAYS);
    // TODO: check is file is successfully open

    furi_check(res);

    write_wav_header(file);

    wav_recorder_adc_init();
    wav_recorder_sampling_init(sampleRate);

    RecorderApp* app = wav_recorder_alloc();

    int16_t* buffer = malloc(sizeof(int16_t) * BUFFER_COUNT);
    size_t buffer_idx = 0;

    wav_recorder_sampling_start(wav_recorder_tick, app->event_queue);

    RecorderEvent event;
    for(bool running = true; running;) {
        furi_check(
            furi_message_queue_get(app->event_queue, &event, FuriWaitForever) == FuriStatusOk);
        if(event.type == EventTypeInput) {
            if(event.input.type == InputTypeShort && event.input.key == InputKeyBack) {
                running = false;
            }
        } else if(event.type == EventTypeTick) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);

            uint32_t adc_value = furi_hal_adc_read_sw();

            uint32_t prev_max = app->sample_max;
            uint32_t prev_min = app->sample_min;

            app->sample_max = MAX(adc_value, app->sample_max);
            app->sample_min = MIN(adc_value, app->sample_min);

            buffer[buffer_idx++] = (int16_t)map(adc_value, 0, 4095, -32767, 32767);
            if(buffer_idx == BUFFER_COUNT) {
                write_samples_to_file(file, buffer, buffer_idx);
                memset(buffer, 0, sizeof(int16_t) * BUFFER_COUNT);
                buffer_idx = 0;
            }

            furi_mutex_release(app->mutex);
            if(prev_max != app->sample_max || prev_min != app->sample_min) {
                view_port_update(app->view_port);
            }
        }
    }

    wav_recorder_sampling_stop();

    if(buffer_idx > BUFFER_COUNT) {
        write_samples_to_file(file, buffer, buffer_idx);
        memset(buffer, 0, sizeof(int16_t) * BUFFER_COUNT);
        buffer_idx = 0;
    }

    free(buffer);

    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(file_path);
    furi_record_close(RECORD_STORAGE);

    wav_recorder_free(app);

    wav_recorder_sampling_deinit();
    wav_recorder_adc_deinit();

    return 0;
}
