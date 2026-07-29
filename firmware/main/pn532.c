#include "pn532.h"
#include "config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "pn532";

// ---------------------------------------------------------------------------
// PN532 protocol constants
// ---------------------------------------------------------------------------
#define PN532_I2C_ADDR              0x24   // 7-bit
#define PN532_PREAMBLE              0x00
#define PN532_STARTCODE1            0x00
#define PN532_STARTCODE2            0xFF
#define PN532_POSTAMBLE             0x00
#define PN532_HOSTTOPN532           0xD4
#define PN532_PN532TOHOST           0xD5

#define PN532_CMD_GETFIRMWAREVERSION 0x02
#define PN532_CMD_SAMCONFIGURATION   0x14
#define PN532_CMD_INLISTPASSIVETARGET 0x4A
#define PN532_MIFARE_ISO14443A       0x00

#define I2C_XFER_TIMEOUT_MS          100

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

// ---------------------------------------------------------------------------
// Low-level I2C helpers
//
// Every PN532 read over I2C is prefixed by a 1-byte status: bit0 = "ready".
// We poll that single byte for readiness, and strip it when reading a frame.
// ---------------------------------------------------------------------------
static bool pn532_wait_ready(uint32_t timeout_ms)
{
    uint8_t status = 0;
    int64_t start = esp_timer_get_time();
    do {
        if (i2c_master_receive(s_dev, &status, 1, I2C_XFER_TIMEOUT_MS) == ESP_OK &&
            (status & 0x01)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    } while ((esp_timer_get_time() - start) < (int64_t)timeout_ms * 1000);
    return false;
}

// Read a response frame of `n` bytes, discarding the leading status byte.
static esp_err_t pn532_read_frame(uint8_t *buf, size_t n)
{
    uint8_t tmp[40];
    if (n + 1 > sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = i2c_master_receive(s_dev, tmp, n + 1, I2C_XFER_TIMEOUT_MS);
    if (err == ESP_OK) memcpy(buf, tmp + 1, n);
    return err;
}

// Build and send a command frame, then wait for and verify the 6-byte ACK.
static bool pn532_send_command(const uint8_t *cmd, uint8_t cmd_len, uint32_t timeout_ms)
{
    uint8_t frame[8 + 16];
    if (cmd_len > 16) return false;

    uint8_t i = 0;
    frame[i++] = PN532_PREAMBLE;
    frame[i++] = PN532_STARTCODE1;
    frame[i++] = PN532_STARTCODE2;

    uint8_t length = cmd_len + 1;             // +1 for the TFI (host->PN532) byte
    frame[i++] = length;
    frame[i++] = (uint8_t)(~length + 1);      // length checksum

    frame[i++] = PN532_HOSTTOPN532;
    uint8_t sum = PN532_HOSTTOPN532;
    for (uint8_t k = 0; k < cmd_len; k++) {
        frame[i++] = cmd[k];
        sum += cmd[k];
    }
    frame[i++] = (uint8_t)(~sum + 1);         // data checksum
    frame[i++] = PN532_POSTAMBLE;

    if (i2c_master_transmit(s_dev, frame, i, I2C_XFER_TIMEOUT_MS) != ESP_OK) {
        return false;
    }

    if (!pn532_wait_ready(timeout_ms)) return false;

    uint8_t ack[6];
    static const uint8_t ack_good[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
    if (pn532_read_frame(ack, sizeof(ack)) != ESP_OK) return false;
    return memcmp(ack, ack_good, sizeof(ack)) == 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t pn532_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = PN532_SDA_GPIO,
        .scl_io_num                   = PN532_SCL_GPIO,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PN532_I2C_ADDR,
        .scl_speed_hz    = PN532_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    vTaskDelay(pdMS_TO_TICKS(100));   // let the module wake up

    // SAMConfiguration: normal mode, ~1s timeout, IRQ pin used.
    uint8_t sam[] = { PN532_CMD_SAMCONFIGURATION, 0x01, 0x14, 0x01 };
    if (!pn532_send_command(sam, sizeof(sam), 1000)) {
        ESP_LOGE(TAG, "SAMConfiguration failed - check I2C wiring / mode switches");
        return ESP_FAIL;
    }
    // Consume the SAMConfiguration response so the next read stays aligned.
    if (pn532_wait_ready(1000)) {
        uint8_t resp[9];
        pn532_read_frame(resp, sizeof(resp));
    }
    ESP_LOGI(TAG, "PN532 initialised on I2C (SDA=%d SCL=%d)", PN532_SDA_GPIO, PN532_SCL_GPIO);
    return ESP_OK;
}

bool pn532_get_firmware_version(uint32_t *version)
{
    uint8_t cmd[] = { PN532_CMD_GETFIRMWAREVERSION };
    if (!pn532_send_command(cmd, sizeof(cmd), 500)) return false;
    if (!pn532_wait_ready(500)) return false;

    // Frame: 00 00 FF LEN LCS D5 03 IC Ver Rev Support DCS 00
    uint8_t r[13];
    if (pn532_read_frame(r, sizeof(r)) != ESP_OK) return false;
    if (r[5] != PN532_PN532TOHOST || r[6] != (PN532_CMD_GETFIRMWAREVERSION + 1)) return false;

    if (version) {
        *version = ((uint32_t)r[7] << 24) | ((uint32_t)r[8] << 16) |
                   ((uint32_t)r[9] << 8)  |  (uint32_t)r[10];
    }
    return true;
}

bool pn532_start_passive_detection(void)
{
    // Arm detection and return once the ACK is in. The PN532 then scans on its
    // own and asserts IRQ when a band appears; we don't hold the I2C bus.
    uint8_t cmd[] = { PN532_CMD_INLISTPASSIVETARGET, 1 /*max targets*/, PN532_MIFARE_ISO14443A };
    return pn532_send_command(cmd, sizeof(cmd), 1000);
}

bool pn532_read_detected_target(uint8_t *uid, uint8_t *uid_len)
{
    // Call once the response is ready (IRQ low, or pn532_wait_ready() true).
    // Frame: 00 00 FF LEN LCS D5 4B NbTg Tg SENS(2) SEL uidLen uid... DCS 00
    uint8_t r[24];
    if (pn532_read_frame(r, sizeof(r)) != ESP_OK) return false;
    if (r[5] != PN532_PN532TOHOST || r[6] != (PN532_CMD_INLISTPASSIVETARGET + 1)) return false;
    if (r[7] != 1) return false;              // NbTg: number of targets found

    uint8_t len = r[12];
    if (len > 7) len = 7;
    memcpy(uid, &r[13], len);
    if (uid_len) *uid_len = len;
    return true;
}

bool pn532_read_passive_target(uint8_t *uid, uint8_t *uid_len, uint32_t timeout_ms)
{
    if (!pn532_start_passive_detection()) return false;
    if (!pn532_wait_ready(timeout_ms)) return false;
    return pn532_read_detected_target(uid, uid_len);
}
