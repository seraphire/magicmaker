#include "rc522.h"
#include "config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "rc522";
static spi_device_handle_t s_spi = NULL;

// --- MFRC522 registers (subset) --------------------------------------------
#define CommandReg      0x01
#define CommIEnReg      0x02
#define CommIrqReg      0x04
#define ErrorReg        0x06
#define FIFODataReg     0x09
#define FIFOLevelReg    0x0A
#define ControlReg      0x0C
#define BitFramingReg   0x0D
#define ModeReg         0x11
#define TxControlReg    0x14
#define TxASKReg        0x15
#define TModeReg        0x2A
#define TPrescalerReg   0x2B
#define TReloadRegH     0x2C
#define TReloadRegL     0x2D
#define VersionReg      0x37

// --- commands --------------------------------------------------------------
#define PCD_IDLE        0x00
#define PCD_TRANSCEIVE  0x0C
#define PCD_RESETPHASE  0x0F

#define PICC_REQIDL     0x26   // REQA - wake IDLE cards
#define PICC_ANTICOLL   0x93   // anticollision, cascade level 1

#define MI_OK           0
#define MI_NOTAGERR     1
#define MI_ERR          2

#define MAX_LEN         16

// ---------------------------------------------------------------------------
// Low-level register access. Each is one SPI transaction with hardware CS,
// which the MFRC522 SPI protocol is happy with.
//   write: address byte = (reg << 1) & 0x7E
//   read : address byte = ((reg << 1) & 0x7E) | 0x80
// ---------------------------------------------------------------------------
static void rc522_write(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_polling_transmit(s_spi, &t);
}

static uint8_t rc522_read(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_spi, &t);
    return rx[1];
}

static void set_bits(uint8_t reg, uint8_t mask) { rc522_write(reg, rc522_read(reg) |  mask); }
static void clr_bits(uint8_t reg, uint8_t mask) { rc522_write(reg, rc522_read(reg) & ~mask); }

static void antenna_on(void)
{
    if (!(rc522_read(TxControlReg) & 0x03)) set_bits(TxControlReg, 0x03);
}

// ---------------------------------------------------------------------------
// Core transceive: push a command to the card and read back the response.
// ---------------------------------------------------------------------------
static uint8_t rc522_to_card(uint8_t cmd, uint8_t *send, uint8_t sendLen,
                             uint8_t *back, uint16_t *backLen)
{
    uint8_t status = MI_ERR;
    uint8_t irqEn = 0x00, waitIRq = 0x00, lastBits, n;
    int i;

    if (cmd == PCD_TRANSCEIVE) { irqEn = 0x77; waitIRq = 0x30; }

    rc522_write(CommIEnReg, irqEn | 0x80);
    clr_bits(CommIrqReg, 0x80);
    set_bits(FIFOLevelReg, 0x80);            // flush FIFO
    rc522_write(CommandReg, PCD_IDLE);

    for (i = 0; i < sendLen; i++) rc522_write(FIFODataReg, send[i]);

    rc522_write(CommandReg, cmd);
    if (cmd == PCD_TRANSCEIVE) set_bits(BitFramingReg, 0x80);   // StartSend

    i = 2000;                                // ~ generous timeout
    do {
        n = rc522_read(CommIrqReg);
        i--;
    } while ((i != 0) && !(n & 0x01) && !(n & waitIRq));

    clr_bits(BitFramingReg, 0x80);

    if (i != 0 && !(rc522_read(ErrorReg) & 0x1B)) {
        status = MI_OK;
        if (n & irqEn & 0x01) status = MI_NOTAGERR;
        if (cmd == PCD_TRANSCEIVE) {
            n = rc522_read(FIFOLevelReg);
            lastBits = rc522_read(ControlReg) & 0x07;
            if (lastBits) *backLen = (n - 1) * 8 + lastBits;
            else          *backLen = n * 8;
            if (n == 0)       n = 1;
            if (n > MAX_LEN)  n = MAX_LEN;
            for (i = 0; i < n; i++) back[i] = rc522_read(FIFODataReg);
        }
    }
    return status;
}

uint8_t rc522_version(void) { return rc522_read(VersionReg); }

esp_err_t rc522_init(void)
{
    // RST pin: high = running.
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << RC522_RST_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst);
    gpio_set_level(RC522_RST_GPIO, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num     = RC522_MOSI_GPIO,
        .miso_io_num     = RC522_MISO_GPIO,
        .sclk_io_num     = RC522_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(RC522_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = RC522_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = RC522_CS_GPIO,
        .queue_size     = 4,
    };
    err = spi_bus_add_device(RC522_SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) return err;

    // Hard reset pulse, then soft reset.
    gpio_set_level(RC522_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(RC522_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    rc522_write(CommandReg, PCD_RESETPHASE);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Timer + ASK modulation config (standard MFRC522 init).
    rc522_write(TModeReg,      0x8D);
    rc522_write(TPrescalerReg, 0x3E);
    rc522_write(TReloadRegL,   30);
    rc522_write(TReloadRegH,   0);
    rc522_write(TxASKReg,      0x40);
    rc522_write(ModeReg,       0x3D);
    antenna_on();

    uint8_t v = rc522_version();
    const char *tag = (v == 0x91 || v == 0x92) ? "genuine"
                    : (v == 0x88 || v == 0xB2) ? "clone"
                    : (v == 0x00 || v == 0xFF) ? "NO RESPONSE - check wiring/power!"
                    : "unknown";
    ESP_LOGI(TAG, "RC522 version 0x%02X (%s)", v, tag);
    return ESP_OK;
}

bool rc522_read_uid(uint8_t *uid, uint8_t *uid_len)
{
    uint8_t  buf[MAX_LEN];
    uint16_t bits = 0;

    // REQA: is a card in the field?
    rc522_write(BitFramingReg, 0x07);          // short frame (7 bits)
    buf[0] = PICC_REQIDL;
    if (rc522_to_card(PCD_TRANSCEIVE, buf, 1, buf, &bits) != MI_OK || bits != 0x10)
        return false;

    // Anticollision: read the 4-byte UID + BCC checksum.
    uint8_t  ser[MAX_LEN];
    uint16_t unLen = 0;
    rc522_write(BitFramingReg, 0x00);          // full bytes
    ser[0] = PICC_ANTICOLL;
    ser[1] = 0x20;
    if (rc522_to_card(PCD_TRANSCEIVE, ser, 2, ser, &unLen) != MI_OK)
        return false;

    uint8_t chk = 0;
    for (int i = 0; i < 4; i++) chk ^= ser[i];
    if (chk != ser[4]) return false;           // checksum mismatch -> garbage read

    memcpy(uid, ser, 4);
    *uid_len = 4;
    return true;
}
