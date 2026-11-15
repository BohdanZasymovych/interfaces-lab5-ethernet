#ifndef W5500_PORT_H
#define W5500_PORT_H

/*
 * W5500 hardware glue.
 *
 * The only module that touches the HAL for the W5500. It connects WIZnet's
 * ioLibrary to this board: SPI transfers, chip select and hardware reset.
 * Moving the chip to another SPI or other pins only changes this file.
 *
 * Wiring (set up in CubeMX, Lab_Ethernet.ioc):
 *   - SPI: full duplex master, mode 0 (CPOL=0, CPHA=0), 8-bit, MSB first.
 *     SPI1 cannot be used as it is, because it is TX-only for the display.
 *   - W5500_CS:  GPIO output, idle high (active low chip select).
 *   - W5500_RST: GPIO output, idle high (active low reset).
 *   - W5500_INT: optional, not used while the sockets are polled.
 *
 * Call order: MX_SPIx_Init() -> W5500_Port_Init() -> NET_Init().
 */

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#define W5500_VERSION 0x04u  // Value of VERSIONR on every W5500, used to check the SPI link

#define W5500_RESET_LOW_MS    1u  // RSTn low time; datasheet minimum is 500 us
#define W5500_RESET_SETTLE_MS 2u  // Wait after releasing RSTn for the internal PLL to lock

/**
 * Bring up the W5500 and connect it to the ioLibrary.
 *
 * Steps:
 *   1. Remember the SPI handle for the callbacks.
 *   2. Drive CS high (deselected).
 *   3. W5500_Port_HardReset().
 *   4. Register the callbacks with the ioLibrary:
 *        reg_wizchip_cs_cbfunc(select, deselect);
 *        reg_wizchip_spi_cbfunc(read_byte, write_byte);
 *        reg_wizchip_spiburst_cbfunc(read_buf, write_buf);
 *   5. Read VERSIONR (getVERSIONR()) and compare it with W5500_VERSION.
 *
 * @param hspi  Initialised full-duplex SPI handle the W5500 is wired to.
 * @return true if the chip answered with the expected version, false if it
 *         did not (wiring, SPI mode, CS or power problem).
 */
bool W5500_Port_Init(SPI_HandleTypeDef* hspi);

/**
 * Hardware reset through RSTn: pull low for W5500_RESET_LOW_MS, release, then
 * wait W5500_RESET_SETTLE_MS. Afterwards every W5500 register is at its default
 * (no MAC/IP, all sockets closed), so NET_Init() has to run again.
 */
void W5500_Port_HardReset(void);

/**
 * Read the W5500 version register.
 *
 * @return VERSIONR (W5500_VERSION on a working chip). 0x00 or 0xFF usually
 *         means MISO is not connected or the chip is not selected.
 */
uint8_t W5500_Port_GetVersion(void);

/*
 * Callbacks for the ioLibrary (implement them as static functions in
 * w5500_port.c; they are not part of the public API):
 *
 *   static void    w5500_cs_select(void);    CS low
 *   static void    w5500_cs_deselect(void);  CS high
 *   static uint8_t w5500_spi_read_byte(void);              HAL_SPI_Receive, 1 byte
 *   static void    w5500_spi_write_byte(uint8_t b);        HAL_SPI_Transmit, 1 byte
 *   static void    w5500_spi_read_buf(uint8_t* buf, uint16_t len);
 *   static void    w5500_spi_write_buf(uint8_t* buf, uint16_t len);
 *
 * Use blocking HAL calls, not DMA: the ioLibrary expects each callback to
 * return only after the transfer is finished.
 */

#endif
