#include "utils/w5500_port.h"

#include "wizchip_conf.h"

#define W5500_SPI_TIMEOUT_MS 10u  // a transfer of a few bytes takes µs; this only guards against a hung bus

static SPI_HandleTypeDef* w5500_spi = NULL;  // SPI handle passed to W5500_Port_Init(), used by the callbacks

/* ---------- callbacks for the ioLibrary ---------- */

static void w5500_cs_select(void) {
    HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_RESET);
}

static void w5500_cs_deselect(void) {
    HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_SET);
}

static uint8_t w5500_spi_read_byte(void) {
    uint8_t tx = 0x00, rx = 0x00;
    HAL_SPI_TransmitReceive(w5500_spi, &tx, &rx, 1, W5500_SPI_TIMEOUT_MS);
    return rx;
}

static void w5500_spi_write_byte(uint8_t b) {
    HAL_SPI_Transmit(w5500_spi, &b, 1, W5500_SPI_TIMEOUT_MS);
}

static void w5500_spi_read_buf(uint8_t* buf, uint16_t len) {
    HAL_SPI_Receive(w5500_spi, buf, len, W5500_SPI_TIMEOUT_MS);
}

static void w5500_spi_write_buf(uint8_t* buf, uint16_t len) {
    HAL_SPI_Transmit(w5500_spi, buf, len, W5500_SPI_TIMEOUT_MS);
}

bool W5500_Port_Init(SPI_HandleTypeDef* hspi) {
    if (hspi == NULL) return false;

    w5500_spi = hspi;

    w5500_cs_deselect();
    W5500_Port_HardReset();

    reg_wizchip_cs_cbfunc(w5500_cs_select, w5500_cs_deselect);
    reg_wizchip_spi_cbfunc(w5500_spi_read_byte, w5500_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(w5500_spi_read_buf, w5500_spi_write_buf);

    return W5500_Port_GetVersion() == W5500_VERSION;
}

void W5500_Port_HardReset(void) {
    HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(W5500_RESET_LOW_MS);
    HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(W5500_RESET_SETTLE_MS);
}

uint8_t W5500_Port_GetVersion(void) {
    return getVERSIONR();
}