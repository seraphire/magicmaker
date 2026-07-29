#include "audio.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "audio";

// I2S output channel (transmit).
static i2s_chan_handle_t s_tx = NULL;

// The rate the I2S clock is currently configured for. We retune it per file so
// clips recorded at different rates (e.g. 16 kHz voice, 22050 Hz stingers) all
// play at the correct pitch instead of assuming one fixed rate.
static uint32_t s_cur_rate = AUDIO_SAMPLE_RATE;
static volatile bool s_stop_req = false;   // cut the current clip short

// Requests from other tasks: fixed-size path strings on a queue.
#define PATH_MAX_LEN 64
static QueueHandle_t s_req_q = NULL;

// Set true on request, cleared by the audio task when the file finishes.
static volatile bool s_playing = false;

// Streaming buffers. 512 mono frames per read; expanded to stereo for I2S.
#define CHUNK_FRAMES 512

// ---------------------------------------------------------------------------
// Volume: runtime 0..256 (256 = unity). Driven by the optional pot on an ADC
// pin; if the pot is disabled it just stays at AUDIO_VOLUME.
// ---------------------------------------------------------------------------
static volatile int s_volume = AUDIO_VOLUME;

#if USE_VOLUME_POT
#include "esp_adc/adc_oneshot.h"
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_channel_t             s_vol_ch;

static void volume_pot_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(VOLUME_POT_GPIO, &unit, &s_vol_ch) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d is not ADC-capable; volume knob disabled", VOLUME_POT_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) { s_adc = NULL; return; }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, s_vol_ch, &ccfg);
    ESP_LOGI(TAG, "Volume knob active on GPIO %d", VOLUME_POT_GPIO);
}

static void volume_pot_update(void)
{
    if (!s_adc) return;
    int raw = 0;
    if (adc_oneshot_read(s_adc, s_vol_ch, &raw) != ESP_OK) return;
    int v = (raw * 256) / 4095;
    if (v > 256) v = 256;
    s_volume = (s_volume * 3 + v) / 4;   // light smoothing kills ADC jitter
}
#else
static void volume_pot_init(void)   {}
static void volume_pot_update(void) {}
#endif

// ---------------------------------------------------------------------------
// Data filesystem (WAVs + the web page). LittleFS now, but still mounted at
// "/spiffs" so the existing asset paths don't change. LittleFS drops SPIFFS's
// 32-char object-name cap and survives a power cut mid-write.
// ---------------------------------------------------------------------------
static void mount_spiffs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "storage",
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed (%s). Did you `idf.py flash` the data image?",
                 esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
}

// ---------------------------------------------------------------------------
// I2S - stereo 16-bit at AUDIO_SAMPLE_RATE. The amp is mono; we feed the same
// sample to both slots so it doesn't matter which one it listens to.
// ---------------------------------------------------------------------------
static void setup_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_LOGI(TAG, "I2S up: %d Hz on BCLK=%d WS=%d DOUT=%d",
             AUDIO_SAMPLE_RATE, I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO);
}

// Retune the I2S clock to `rate` (no-op if already there). Requires briefly
// disabling the channel, so only call it from the audio task between clips.
static void i2s_set_rate(uint32_t rate)
{
    if (rate == s_cur_rate) return;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_disable(s_tx);
    esp_err_t r = i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);
    if (r == ESP_OK) { s_cur_rate = rate; ESP_LOGI(TAG, "I2S retuned to %u Hz", (unsigned)rate); }
    else               ESP_LOGW(TAG, "I2S retune to %u Hz failed: %s", (unsigned)rate, esp_err_to_name(r));
}

// Write one mono int16 to both stereo slots, applying software volume.
static inline void mono_to_stereo(int16_t sample, int16_t out[2])
{
    int vol = s_volume;
    if (vol != 256) {
        int32_t s = ((int32_t)sample * vol) >> 8;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        sample = (int16_t)s;
    }
    out[0] = sample;
    out[1] = sample;
}

// ---------------------------------------------------------------------------
// Minimal WAV parsing. We walk the RIFF chunks rather than assuming the data
// starts at byte 44, so files with extra LIST/fact chunks still play.
// Expects PCM 16-bit; handles mono or stereo; assumes AUDIO_SAMPLE_RATE.
// ---------------------------------------------------------------------------
static uint32_t rd_u32(FILE *f) { uint8_t b[4]; fread(b,1,4,f); return b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24); }
static uint16_t rd_u16(FILE *f) { uint8_t b[2]; fread(b,1,2,f); return b[0]|(b[1]<<8); }

static void stream_stereo(FILE *f, uint32_t data_bytes)
{
    int16_t *in  = malloc(CHUNK_FRAMES * 2 * sizeof(int16_t)); // stereo in
    int16_t *out = malloc(CHUNK_FRAMES * 2 * sizeof(int16_t));
    if (!in || !out) { ESP_LOGE(TAG, "OOM"); free(in); free(out); return; }

    uint32_t remaining = data_bytes;
    size_t w;
    while (remaining >= 4) {                          // 4 bytes = one stereo frame
        if (s_stop_req) break;                        // interrupted -> stop promptly
        volume_pot_update();
        uint32_t want = CHUNK_FRAMES * 4;
        if (want > remaining) want = remaining;
        size_t got = fread(in, 1, want, f);
        if (got == 0) break;
        int frames = got / 4;
        for (int i = 0; i < frames; i++) {
            int16_t mixed = (int16_t)(((int32_t)in[i*2] + in[i*2+1]) / 2); // downmix L+R
            mono_to_stereo(mixed, &out[i*2]);
        }
        i2s_channel_write(s_tx, out, frames * 4, &w, portMAX_DELAY);
        remaining -= got;
    }
    free(in);
    free(out);
}

static void stream_mono(FILE *f, uint32_t data_bytes)
{
    int16_t *in  = malloc(CHUNK_FRAMES * sizeof(int16_t));        // mono in
    int16_t *out = malloc(CHUNK_FRAMES * 2 * sizeof(int16_t));    // stereo out
    if (!in || !out) { ESP_LOGE(TAG, "OOM"); free(in); free(out); return; }

    uint32_t remaining = data_bytes;
    size_t w;
    while (remaining >= 2) {                          // 2 bytes = one mono sample
        if (s_stop_req) break;                        // interrupted -> stop promptly
        volume_pot_update();
        uint32_t want = CHUNK_FRAMES * 2;
        if (want > remaining) want = remaining;
        size_t got = fread(in, 1, want, f);
        if (got == 0) break;
        int samples = got / 2;
        for (int i = 0; i < samples; i++) {
            mono_to_stereo(in[i], &out[i*2]);
        }
        i2s_channel_write(s_tx, out, samples * 4, &w, portMAX_DELAY);
        remaining -= got;
    }
    free(in);
    free(out);
}

static void play_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open %s", path); return; }

    char riff[4], wave[4];
    fread(riff, 1, 4, f);            // "RIFF"
    rd_u32(f);                       // overall size (ignored)
    fread(wave, 1, 4, f);            // "WAVE"
    if (memcmp(riff, "RIFF", 4) || memcmp(wave, "WAVE", 4)) {
        ESP_LOGE(TAG, "%s is not a WAV", path);
        fclose(f);
        return;
    }

    uint16_t channels = 1, bits = 16;
    uint32_t rate = AUDIO_SAMPLE_RATE;
    uint32_t data_bytes = 0;
    bool have_data = false;

    // Walk chunks until we find "data".
    char id[4];
    while (fread(id, 1, 4, f) == 4) {
        uint32_t csz = rd_u32(f);
        if (memcmp(id, "fmt ", 4) == 0) {
            rd_u16(f);                    // audio format (1 = PCM)
            channels = rd_u16(f);
            rate     = rd_u32(f);
            rd_u32(f);                    // byte rate
            rd_u16(f);                    // block align
            bits     = rd_u16(f);
            if (csz > 16) fseek(f, csz - 16, SEEK_CUR);   // skip any extension
        } else if (memcmp(id, "data", 4) == 0) {
            data_bytes = csz;
            have_data  = true;
            break;                        // audio samples start right here
        } else {
            fseek(f, csz + (csz & 1), SEEK_CUR);          // skip, chunks are word-aligned
        }
    }

    if (!have_data || bits != 16) {
        ESP_LOGE(TAG, "%s: unsupported (bits=%u, data=%s)", path, bits, have_data ? "yes" : "no");
        fclose(f);
        return;
    }
    i2s_set_rate(rate);        // honor the file's own sample rate (correct pitch)

    ESP_LOGI(TAG, "Playing %s (%s, %u Hz, %u bytes)", path, channels == 1 ? "mono" : "stereo",
             (unsigned)rate, (unsigned)data_bytes);
    if (channels == 1) stream_mono(f, data_bytes);
    else               stream_stereo(f, data_bytes);

    fclose(f);
}

// ---------------------------------------------------------------------------
// The one task that owns I2S: play a queued file, otherwise stream silence.
// ---------------------------------------------------------------------------
static void audio_task(void *arg)
{
    const int SIL_FRAMES = 256;
    int16_t *silence = calloc(SIL_FRAMES * 2, sizeof(int16_t)); // stereo zeros
    char path[PATH_MAX_LEN];
    size_t w;

    for (;;) {
        if (xQueueReceive(s_req_q, path, 0) == pdTRUE) {
            s_stop_req = false;     // clear here: a stop only kills the clip it
                                    // was aimed at, never the next one
            play_wav(path);
            s_playing = false;      // back to idle -> silence resumes
        } else {
            // Keep the amp fed so it stays quiet between sounds.
            i2s_channel_write(s_tx, silence, SIL_FRAMES * 2 * sizeof(int16_t),
                              &w, portMAX_DELAY);
        }
    }
    free(silence);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void audio_init(void)
{
    mount_spiffs();
    setup_i2s();
    volume_pot_init();
    // Deep enough for a composed countdown phrase (lead-in + number + unit +
    // trailer) plus headroom, so the parts stream back-to-back with no gap.
    s_req_q = xQueueCreate(8, PATH_MAX_LEN);
    xTaskCreate(audio_task, "audio", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "Audio engine started");
}

void audio_play(const char *path)
{
    if (!s_req_q) return;
    char buf[PATH_MAX_LEN];
    strncpy(buf, path, PATH_MAX_LEN - 1);
    buf[PATH_MAX_LEN - 1] = '\0';
    s_playing = true;                        // visible immediately to the caller
    if (xQueueSend(s_req_q, buf, 0) != pdTRUE) {
        s_playing = false;                   // queue full; drop the request
        ESP_LOGW(TAG, "Audio queue full, dropped %s", path);
    }
}

bool audio_is_playing(void)
{
    // A composed phrase is several queued clips: s_playing drops between them,
    // so also report "playing" while parts are still waiting in the queue.
    // Otherwise a caller waiting on the phrase would bail after the first word.
    return s_playing || (s_req_q && uxQueueMessagesWaiting(s_req_q) > 0);
}

void audio_stop(void)
{
    s_stop_req = true;                       // streaming loops bail at the next chunk
    if (s_req_q) {                           // and drop anything queued behind it
        char drop[PATH_MAX_LEN];
        while (xQueueReceive(s_req_q, drop, 0) == pdTRUE) { }
    }
}
