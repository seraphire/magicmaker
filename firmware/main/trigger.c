#include "trigger.h"
#include "config.h"
#include <stdio.h>
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "trigger";

#if TRIGGER_USE_RC522
// ===========================================================================
// RC522 RFID over SPI. Any 13.56 MHz card/band tap = one trigger.
// ===========================================================================
#include "rc522.h"

static int64_t s_last_trigger_us = 0;

void trigger_init(void)
{
    if (rc522_init() != ESP_OK) {
        ESP_LOGE(TAG, "RC522 init failed - check SPI wiring / power");
        return;
    }
    ESP_LOGI(TAG, "RC522 RFID trigger ready - tap a card or 'band");
    s_last_trigger_us = 0;
}

bool trigger_poll(uint8_t *uid, uint8_t *uid_len)
{
    if (uid_len) *uid_len = 0;

    uint8_t raw[10], raw_len = 0;
    if (!rc522_read_uid(raw, &raw_len)) return false;

    int64_t now = esp_timer_get_time();
    if ((now - s_last_trigger_us) < (int64_t)NFC_RETRIGGER_MS * 1000)
        return false;                       // same tap still lingering
    s_last_trigger_us = now;

    uint8_t n = raw_len < 4 ? raw_len : 4;   // UID for the caller (up to 4 bytes)
    for (int i = 0; i < n; i++) uid[i] = raw[i];
    if (uid_len) *uid_len = n;

    char hex[3 * 10 + 1] = {0};
    for (int i = 0; i < raw_len && i < 10; i++) snprintf(hex + i * 3, 4, "%02X ", raw[i]);
    ESP_LOGI(TAG, "Card detected, UID: %s", hex);
    return true;
}

void trigger_flush(void)
{
    uint8_t raw[10], raw_len = 0;
    rc522_read_uid(raw, &raw_len);          // drop whatever is on the reader now
    s_last_trigger_us = esp_timer_get_time();
}

#elif TRIGGER_USE_NFC
// ===========================================================================
// NFC implementation - PN532 over I2C. A band tap is one trigger.
// ===========================================================================
#include "pn532.h"

static int64_t s_last_trigger_us = 0;   // re-trigger cooldown

static void log_uid(const uint8_t *uid, uint8_t len)
{
    char hex[3 * 7 + 1] = {0};
    for (int i = 0; i < len && i < 7; i++) {
        snprintf(hex + i * 3, 4, "%02X ", uid[i]);
    }
    ESP_LOGI(TAG, "Band detected, UID: %s", hex);
}

static void nfc_report_ready(void)
{
    uint32_t ver = 0;
    if (pn532_get_firmware_version(&ver)) {
        ESP_LOGI(TAG, "PN532 firmware 0x%08" PRIx32 " - NFC trigger ready", ver);
    } else {
        ESP_LOGW(TAG, "PN532 did not report firmware; will still try to read bands");
    }
}

#if PN532_USE_IRQ
// ---- Interrupt-driven: the PN532 pulls IRQ LOW when a band is found -------
#include "driver/gpio.h"

static volatile bool s_irq_fired = false;
static bool s_armed = false;

static void IRAM_ATTR pn532_irq_isr(void *arg)
{
    s_irq_fired = true;
}

// (Re)arm detection, clearing any IRQ edge left over from the ACK phase so we
// only react to the *next* one, which means "a band was found".
static void nfc_arm(void)
{
    s_armed = pn532_start_passive_detection();
    s_irq_fired = false;
}

void trigger_init(void)
{
    if (pn532_init() != ESP_OK) {
        ESP_LOGE(TAG, "PN532 init failed - is the module in I2C mode and powered?");
        return;
    }
    nfc_report_ready();

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PN532_IRQ_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,          // IRQ is open-drain, active low
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(gpio_isr_handler_add(PN532_IRQ_GPIO, pn532_irq_isr, NULL));

    nfc_arm();
    ESP_LOGI(TAG, "NFC IRQ trigger ready on GPIO %d", PN532_IRQ_GPIO);
}

bool trigger_poll(uint8_t *uid, uint8_t *uid_len)
{
    if (uid_len) *uid_len = 0;
    int64_t now = esp_timer_get_time();

    if (!s_armed) {           // a previous arm failed (bus hiccup) - keep trying
        nfc_arm();
        return false;
    }
    if (!s_irq_fired) {
        return false;         // nothing waiting - the idle animation stays smooth
    }
    s_irq_fired = false;

    uint8_t raw[7], raw_len = 0;
    bool found = pn532_read_detected_target(raw, &raw_len);
    nfc_arm();                // re-arm for the next band

    if (!found) return false;
    if ((now - s_last_trigger_us) < (int64_t)NFC_RETRIGGER_MS * 1000) {
        return false;         // same band still lingering on the reader
    }
    s_last_trigger_us = now;
    uint8_t n = raw_len < 4 ? raw_len : 4;
    for (int i = 0; i < n; i++) uid[i] = raw[i];
    if (uid_len) *uid_len = n;
    log_uid(raw, raw_len);
    return true;
}

void trigger_flush(void)
{
    s_irq_fired = false;                    // drop a detection raised during the show
    nfc_arm();
    s_last_trigger_us = esp_timer_get_time();
}

#else   // PN532_USE_IRQ == 0
// ---- Polled fallback: short blocking read each poll ----------------------
void trigger_init(void)
{
    if (pn532_init() != ESP_OK) {
        ESP_LOGE(TAG, "PN532 init failed - is the module in I2C mode and powered?");
        return;
    }
    nfc_report_ready();
    s_last_trigger_us = 0;
}

bool trigger_poll(uint8_t *uid, uint8_t *uid_len)
{
    if (uid_len) *uid_len = 0;
    uint8_t raw[7], raw_len = 0;
    if (!pn532_read_passive_target(raw, &raw_len, 50)) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if ((now - s_last_trigger_us) < (int64_t)NFC_RETRIGGER_MS * 1000) {
        return false;
    }
    s_last_trigger_us = now;
    uint8_t n = raw_len < 4 ? raw_len : 4;
    for (int i = 0; i < n; i++) uid[i] = raw[i];
    if (uid_len) *uid_len = n;
    log_uid(raw, raw_len);
    return true;
}

void trigger_flush(void)
{
    s_last_trigger_us = esp_timer_get_time();
}
#endif  // PN532_USE_IRQ

#else
// ===========================================================================
// Button implementation - debounced GPIO. This is the stopgap default.
// ===========================================================================
#include "driver/gpio.h"

// A reading must hold steady this long before we trust it. 25 ms kills bounce.
#define DEBOUNCE_US   (25 * 1000)
#define PRESSED_LEVEL (BUTTON_ACTIVE_LOW ? 0 : 1)

static int     s_raw_level;        // most recent raw reading
static int     s_stable_level;     // debounced reading
static int64_t s_last_edge_us;     // when the raw reading last changed

void trigger_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
        .pull_down_en = BUTTON_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    s_raw_level    = gpio_get_level(BUTTON_GPIO);
    s_stable_level = s_raw_level;
    s_last_edge_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Button trigger ready on GPIO %d (active %s)",
             BUTTON_GPIO, BUTTON_ACTIVE_LOW ? "LOW" : "HIGH");
}

bool trigger_poll(uint8_t *uid, uint8_t *uid_len)
{
    (void)uid;
    if (uid_len) *uid_len = 0;          // the button has no UID

    int     level = gpio_get_level(BUTTON_GPIO);
    int64_t now   = esp_timer_get_time();

    if (level != s_raw_level) {           // track raw edges to time the debounce
        s_raw_level    = level;
        s_last_edge_us = now;
    }

    if ((now - s_last_edge_us) > DEBOUNCE_US && level != s_stable_level) {
        s_stable_level = level;
        if (s_stable_level == PRESSED_LEVEL) {
            return true;                  // fresh press
        }
    }
    return false;
}

void trigger_flush(void)
{
    // Adopt the current level as the debounced state, so a button still held
    // when the show ends doesn't read as a fresh press.
    s_raw_level    = gpio_get_level(BUTTON_GPIO);
    s_stable_level = s_raw_level;
    s_last_edge_us = esp_timer_get_time();
}

#endif // TRIGGER_USE_NFC
