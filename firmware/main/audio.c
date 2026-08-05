#include "audio.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "mp3dec.h"                // Helix, via chmorgan/esp-libhelix-mp3

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

// Peak-hold of recent output, for LED effects that follow the sound. Every
// sample raises it; audio_level() lets it fall. Sampling the raw amplitude
// instead would strobe - this holds each syllable's peak so the decay between
// them is what shows.
static volatile uint8_t s_level = 0;

// Write one mono int16 to both stereo slots, applying software volume.
static inline void mono_to_stereo(int16_t sample, int16_t out[2])
{
    // Level BEFORE volume. The pulse follows what the clip is doing, not how
    // loud the room wants it.
    //
    // Measured the other way round: with the knob turned down the level peaked
    // at 57 of 255 where the effect is tuned for ~165, so the cheeky pulse
    // swung over roughly 18 distinct brightness values instead of 120. That
    // reads as a choppy animation, and it is one - at that range consecutive
    // frames round to the same value and the light genuinely stops moving.
    // Turning the volume down should not dim the light show.
    int32_t a = sample < 0 ? -(int32_t)sample : sample;   // int32: -32768 negates out of range
    uint8_t l = (uint8_t)(a >> 7);                        // 0..255
    if (l > s_level) s_level = l;

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

uint8_t audio_level(void)
{
    uint8_t v = s_level;
    // Decay on read rather than on a timer: the caller is an animation loop at a
    // steady frame rate, so this is the release, and it costs no extra task.
    s_level = (uint8_t)((v * 3) / 4);
    return v;
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
// MP3 (Helix). Decoded frame by frame straight into I2S, so RAM use is fixed
// no matter how long the clip is - a three-minute song costs the same as a
// one-second one. Speech at 16 kHz mono lands around 8x smaller than the
// equivalent WAV, which is what keeps the data partition from filling up.
// ---------------------------------------------------------------------------
#define MP3_INBUF   (2 * MAINBUF_SIZE)                    // one frame + refill slack
#define MP3_MAXSAMP (MAX_NGRAN * MAX_NSAMP * MAX_NCHAN)   // biggest frame Helix emits

// An ID3v2 tag sits in front of the audio and can contain bytes that look like
// a frame header, so skip it properly rather than letting the sync search walk
// into it. Leaves the file positioned at the first real frame.
static void skip_id3v2(FILE *f)
{
    uint8_t h[10];
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) { fseek(f, 0, SEEK_SET); return; }
    if (memcmp(h, "ID3", 3) != 0) { fseek(f, 0, SEEK_SET); return; }
    // Size is 4 sync-safe bytes (7 bits each), excluding this 10-byte header.
    uint32_t sz = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                  ((uint32_t)(h[8] & 0x7F) <<  7) |  (uint32_t)(h[9] & 0x7F);
    if (h[5] & 0x10) sz += 10;                  // footer present
    fseek(f, 10 + (long)sz, SEEK_SET);
    ESP_LOGD(TAG, "skipped %u-byte ID3v2 tag", (unsigned)sz);
}

// ---------------------------------------------------------------------------
// Gapless playback.
//
// An MP3 doesn't hold a whole number of frames' worth of audio, so the encoder
// pads it: a lead-in before the first real sample, and silence after the last.
// A decoder that ignores this plays the padding, which is why a naive MP3 of a
// 0.61 s clip comes out 0.68 s long. Standalone sounds don't care. The
// countdown does - it butts clips together to build one sentence, and 74-102 ms
// of silence at every join is an audible stutter.
//
// The amounts are recorded in the LAME/Xing tag in the first frame, so this
// reads them and skips exactly that much. `delay` is the encoder's lead-in;
// 529 is the decoder's own pipeline delay, which every MP3 decoder incurs and
// Helix is no exception.
// ---------------------------------------------------------------------------
typedef struct {
    bool  found;      // a Xing/Info tag was present
    int   skip;       // samples to drop from the very start
    long  emit;       // total samples of real audio, or 0 if unknown
} mp3_gapless_t;

#define MP3_DECODER_DELAY 529

static void parse_gapless(FILE *f, mp3_gapless_t *g)
{
    memset(g, 0, sizeof(*g));
    long start = ftell(f);

    uint8_t h[4];
    if (fread(h, 1, 4, f) != 4 || h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) goto out;

    int ver   = (h[1] >> 3) & 0x03;      // 0=MPEG2.5, 2=MPEG2, 3=MPEG1
    int prot  =  h[1] & 0x01;            // 0 = a 16-bit CRC follows the header
    int mono  = ((h[3] >> 6) & 0x03) == 3;
    int mpeg1 = (ver == 3);
    int spf   = mpeg1 ? 1152 : 576;      // samples per frame, Layer III
    int side  = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);

    // Tag sits after the header, the optional CRC, and the side info.
    if (fseek(f, start + 4 + (prot ? 0 : 2) + side, SEEK_SET) != 0) goto out;

    uint8_t tag[4];
    if (fread(tag, 1, 4, f) != 4) goto out;
    if (memcmp(tag, "Xing", 4) != 0 && memcmp(tag, "Info", 4) != 0) goto out;

    uint8_t fl[4];
    if (fread(fl, 1, 4, f) != 4) goto out;
    uint32_t flags = ((uint32_t)fl[0] << 24) | (fl[1] << 16) | (fl[2] << 8) | fl[3];

    long frames = 0;
    if (flags & 0x01) {                              // frame count
        uint8_t b[4];
        if (fread(b, 1, 4, f) != 4) goto out;
        frames = ((long)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    }
    if (flags & 0x02) fseek(f, 4,   SEEK_CUR);       // byte count
    if (flags & 0x04) fseek(f, 100, SEEK_CUR);       // seek table
    if (flags & 0x08) fseek(f, 4,   SEEK_CUR);       // quality

    // The LAME extension starts here with a 9-byte encoder string; the packed
    // 12-bit delay and 12-bit padding sit 21 bytes in.
    uint8_t lame[24];
    if (fread(lame, 1, sizeof(lame), f) != sizeof(lame)) goto out;
    int delay   = ((int)lame[21] << 4) | (lame[22] >> 4);
    int padding = ((int)(lame[22] & 0x0F) << 8) | lame[23];

    g->found = true;
    g->skip  = delay + MP3_DECODER_DELAY;
    if (frames > 0) {
        long total = frames * (long)spf;             // the tag counts audio frames
        long real  = total - delay - padding;
        g->emit = real > 0 ? real : 0;
    }
    ESP_LOGD(TAG, "gapless: delay=%d padding=%d frames=%ld -> skip %d, emit %ld",
             delay, padding, frames, g->skip, g->emit);
out:
    fseek(f, start, SEEK_SET);                       // always hand the file back unmoved
}

static void play_mp3(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open %s", path); return; }
    skip_id3v2(f);

    mp3_gapless_t gap;
    parse_gapless(f, &gap);

    HMP3Decoder dec  = MP3InitDecoder();
    uint8_t *in      = malloc(MP3_INBUF);
    int16_t *pcm     = malloc(MP3_MAXSAMP * sizeof(int16_t));
    // Worst case one frame is all-mono samples, each doubled into stereo.
    int16_t *out     = malloc(MP3_MAXSAMP * 2 * sizeof(int16_t));
    if (!dec || !in || !pcm || !out) {
        ESP_LOGE(TAG, "MP3 init OOM for %s", path);
        goto done;
    }

    uint8_t *rp    = in;
    int bytesLeft  = 0;
    bool eof       = false, rate_set = false;
    int  frames    = 0, stalls = 0;
    int  toSkip    = gap.found ? gap.skip : 0;   // counts down over the first frames
    long emitted   = 0;
    size_t w;

    while (!s_stop_req) {
        // Top up whenever we're inside one frame of running dry. The memmove
        // keeps the unconsumed tail at the front so a frame is never split.
        if (bytesLeft < MAINBUF_SIZE && !eof) {
            if (bytesLeft > 0 && rp != in) memmove(in, rp, bytesLeft);
            rp = in;
            size_t got = fread(in + bytesLeft, 1, MP3_INBUF - bytesLeft, f);
            if (got == 0) eof = true;
            bytesLeft += (int)got;
        }
        if (bytesLeft <= 0) break;

        int off = MP3FindSyncWord(rp, bytesLeft);
        if (off < 0) {                     // nothing frame-like in hand
            if (eof) break;
            bytesLeft = 0;                 // discard and refill
            continue;
        }
        rp += off; bytesLeft -= off;

        int err = MP3Decode(dec, &rp, &bytesLeft, pcm, 0);
        if (err) {
            // A truncated frame at the buffer edge just needs more bytes; the
            // stall counter stops that becoming a spin if the refill can't help.
            if (err == ERR_MP3_INDATA_UNDERFLOW && !eof && ++stalls < 8) continue;
            if (err != ERR_MP3_INDATA_UNDERFLOW)
                ESP_LOGW(TAG, "%s: MP3 error %d after %d frame(s)", path, err, frames);
            break;
        }
        stalls = 0;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (!rate_set) {
            i2s_set_rate((uint32_t)fi.samprate);
            ESP_LOGI(TAG, "Playing %s (MP3, %s, %d Hz, %d kbps)", path,
                     fi.nChans == 1 ? "mono" : "stereo", fi.samprate, fi.bitrate / 1000);
            rate_set = true;
        }

        volume_pot_update();
        int nframes = (fi.nChans == 2) ? fi.outputSamps / 2 : fi.outputSamps;
        int first   = 0;

        // The Xing/Info tag lives in a real frame, so Helix decodes it and hands
        // back a frame of silence. Drop it whole rather than letting it count
        // against the lead-in we're about to skip.
        if (gap.found && frames == 0) { frames++; continue; }

        if (toSkip > 0) {                    // encoder lead-in + decoder delay
            if (toSkip >= nframes) { toSkip -= nframes; frames++; continue; }
            first   = toSkip;
            toSkip  = 0;
        }
        if (gap.emit > 0) {                  // and stop before the trailing padding
            long room = gap.emit - emitted;
            if (room <= 0) break;
            if (nframes - first > room) nframes = first + (int)room;
        }

        for (int i = first; i < nframes; i++) {
            int16_t m = (fi.nChans == 2)
                        ? (int16_t)(((int32_t)pcm[i*2] + pcm[i*2+1]) / 2)   // downmix
                        : pcm[i];
            mono_to_stereo(m, &out[(i - first) * 2]);
        }
        int n = nframes - first;
        if (n > 0) {
            i2s_channel_write(s_tx, out, n * 4, &w, portMAX_DELAY);
            emitted += n;
        }
        frames++;
    }

    if (frames == 0) {
        ESP_LOGE(TAG, "%s: no MP3 frames decoded", path);
    } else if (gap.found) {
        // Sample-exact is the whole point: this should match the source WAV's
        // count, which is how gapless gets verified without trusting an ear.
        ESP_LOGI(TAG, "%s: %ld samples emitted (target %ld), %d frames",
                 path, emitted, gap.emit, frames);
    } else {
        ESP_LOGI(TAG, "%s: %ld samples, %d frames (no gapless tag - padding kept)",
                 path, emitted, frames);
    }

done:
    if (dec) MP3FreeDecoder(dec);
    free(in); free(pcm); free(out);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Extension in a stored path is a logical name, not a promise. The build script
// decides per folder whether a clip ships as WAV or MP3, and bands enrolled
// months ago have "/spiffs/chime.wav" sitting in NVS. Resolving here means
// re-encoding the media never invalidates a saved path.
// ---------------------------------------------------------------------------
bool audio_resolve(const char *path, char *out, size_t out_sz)
{
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        if (out && out_sz) { strncpy(out, path, out_sz - 1); out[out_sz - 1] = '\0'; }
        return true;
    }
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/')) return false;          // no extension to swap
    const char *alt = (strcasecmp(dot, ".wav") == 0) ? ".mp3"
                    : (strcasecmp(dot, ".mp3") == 0) ? ".wav" : NULL;
    if (!alt) return false;

    char cand[PATH_MAX_LEN];
    size_t stem = (size_t)(dot - path);
    if (stem + 5 > sizeof(cand)) return false;
    memcpy(cand, path, stem);
    strcpy(cand + stem, alt);
    if (stat(cand, &st) != 0 || st.st_size <= 0) return false;
    if (out && out_sz) { strncpy(out, cand, out_sz - 1); out[out_sz - 1] = '\0'; }
    return true;
}

// --- variant selection ------------------------------------------------------
// Split a logical path into its directory and its base name (extension removed,
// "-N" suffix KEPT - see the header for why pinning depends on that).
static void split_logical(const char *logical, char *dir, size_t dir_sz,
                          char *base, size_t base_sz)
{
    const char *slash = strrchr(logical, '/');
    if (slash) {
        size_t n = (size_t)(slash - logical);
        if (n >= dir_sz) n = dir_sz - 1;
        memcpy(dir, logical, n);
        dir[n] = '\0';
        if (dir[0] == '\0') strncpy(dir, "/", dir_sz);   // file at the root
        snprintf(base, base_sz, "%s", slash + 1);
    } else {
        snprintf(dir, dir_sz, ".");
        snprintf(base, base_sz, "%s", logical);
    }
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
}

// Does `name` (already extension-stripped) belong to the family `base`?
// Returns the variant number, or -1 for no.
static int variant_of(const char *name, const char *base)
{
    size_t bl = strlen(base);
    if (strcmp(name, base) == 0) return 0;                  // the bare name
    if (strncmp(name, base, bl) != 0 || name[bl] != '-') return -1;

    const char *num = name + bl + 1;
    if (!*num) return -1;
    for (const char *c = num; *c; c++)
        if (*c < '0' || *c > '9') return -1;                // "be-our-guest": not digits
    long v = strtol(num, NULL, 10);
    return (v >= 0 && v < AUDIO_VARIANT_MAX) ? (int)v : -1;
}

int audio_variants(const char *logical, uint8_t *out, int max)
{
    if (!logical || !logical[0] || !out || max <= 0) return 0;

    char dir[PATH_MAX_LEN], base[PATH_MAX_LEN];
    split_logical(logical, dir, sizeof(dir), base, sizeof(base));

    DIR *d = opendir(dir);
    if (!d) return 0;

    uint32_t seen = 0;                       // bitmap: variant N present
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char stem[64];
        snprintf(stem, sizeof(stem), "%.63s", e->d_name);
        char *dot = strrchr(stem, '.');
        if (!dot) continue;                                  // audio always has one
        if (strcasecmp(dot, ".wav") != 0 && strcasecmp(dot, ".mp3") != 0) continue;
        *dot = '\0';

        int v = variant_of(stem, base);
        if (v >= 0) seen |= (1u << v);
    }
    closedir(d);

    // Ascending, so variant numbers stay stable identities: countdown persists
    // "the clip I used last" as this number, and it has to keep meaning the
    // same clip across a pack that adds or retires others.
    int n = 0;
    for (int v = 0; v < AUDIO_VARIANT_MAX && n < max; v++)
        if (seen & (1u << v)) out[n++] = (uint8_t)v;
    return n;
}

bool audio_variant_path(const char *logical, uint8_t variant, char *out, size_t out_sz)
{
    if (!logical || !out || !out_sz) return false;

    char dir[PATH_MAX_LEN], base[PATH_MAX_LEN];
    // Sized so the compiler can PROVE no truncation: dir + '/' + base +
    // "-NN.wav" can't reach this even at both buffers' worst case. It only
    // has to survive as far as audio_resolve, which does its own bounds check.
    char want[PATH_MAX_LEN * 3];
    split_logical(logical, dir, sizeof(dir), base, sizeof(base));

    if (variant == 0) snprintf(want, sizeof(want), "%s/%s.wav", dir, base);
    else              snprintf(want, sizeof(want), "%s/%s-%u.wav", dir, base, (unsigned)variant);

    return audio_resolve(want, out, out_sz);   // .wav here is logical; may land on .mp3
}

int audio_pick_variant(const char *logical, uint8_t avoid, char *out, size_t out_sz)
{
    uint8_t v[AUDIO_VARIANT_MAX];
    int n = audio_variants(logical, v, AUDIO_VARIANT_MAX);
    if (n <= 0) return -1;

    // Draw from n-1 and step past the avoided one, rather than re-rolling.
    // Re-rolling can spin on a family of two; stepping without the draw
    // adjustment would make the clip after it twice as likely. This stays
    // uniform over what's allowed.
    int idx;
    int avoid_idx = -1;
    for (int i = 0; i < n; i++) if (v[i] == avoid) { avoid_idx = i; break; }

    if (n > 1 && avoid_idx >= 0) {
        idx = (int)(esp_random() % (uint32_t)(n - 1));
        if (idx >= avoid_idx) idx++;
    } else {
        idx = (int)(esp_random() % (uint32_t)n);
    }

    if (!audio_variant_path(logical, v[idx], out, out_sz)) return -1;
    return v[idx];
}

// ---------------------------------------------------------------------------
// Pick a decoder by looking at the file, not its name. Contributed audio is
// often an MP3 called .wav (or the reverse), and a name-based guess turns that
// into silence with no explanation.
// ---------------------------------------------------------------------------
static void play_file(const char *want)
{
    char path[PATH_MAX_LEN];
    if (!audio_resolve(want, path, sizeof(path))) {
        ESP_LOGE(TAG, "No such clip: %s (tried the other extension too)", want);
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open %s", path); return; }
    uint8_t h[3] = { 0 };
    size_t got = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (got < 3) { ESP_LOGE(TAG, "%s is too short to identify", path); return; }

    if (memcmp(h, "RIFF", 3) == 0)                        play_wav(path);
    else if (memcmp(h, "ID3", 3) == 0 ||                  // tagged MP3
             (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0))     // bare frame sync
                                                          play_mp3(path);
    else ESP_LOGE(TAG, "%s: unrecognised audio (%02X %02X %02X)", path, h[0], h[1], h[2]);
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
            s_playing = true;
            s_stop_req = false;     // clear here: a stop only kills the clip it
                                    // was aimed at, never the next one
            play_file(path);

            // Only drop the flag once nothing is queued behind this clip.
            //
            // audio_is_playing() is "flag OR queue depth", and clearing the flag
            // unconditionally leaves a gap at every clip boundary: xQueueReceive
            // has already removed the next clip, so the queue reads empty, while
            // the flag is still false from the clip that just ended. A caller
            // sampling in that window - and the sustain loops poll every 20 ms -
            // sees "not playing" and abandons the show mid-phrase.
            //
            // Keeping the flag set whenever work remains closes it: across a
            // composed phrase the flag simply never goes false until the last
            // clip has actually finished.
            if (uxQueueMessagesWaiting(s_req_q) == 0) s_playing = false;
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
    // Pinned to core 1, away from app_main and Wi-Fi on core 0. MP3 decoding is
    // real computation, and this task outranks the LED loop (6 against 1), so
    // on a shared core it would win every scheduling contest and the animation
    // would starve. The S3 has a second core; the decoder may as well use it.
    // Stack is above the old 4096: Helix keeps its tables on the heap but still
    // wants more stack than reading WAV bytes ever did.
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 1);
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
