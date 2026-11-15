#include "utils/ili9341.h"
#include <stdio.h>

static SPI_HandleTypeDef* ili_hspi;

#define ILI_CS_Low()  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET)
#define ILI_CS_High() HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET)

#define ILI_DC_Cmd()  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET)
#define ILI_DC_Data() HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET)

#define ILI_RST_Low()  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET)
#define ILI_RST_High() HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET)

static inline void ILI_CS_Delay(void) {
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
}

static volatile uint32_t ili_tx_start_cycles;
static volatile uint32_t ili_last_tile_us;
static volatile uint32_t ili_last_tile_bytes;

static void ILI9341_DWT_Init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static ili_orientation_t ili_orientation = ILI_PORTRAIT;
static uint16_t ili_width = ILI9341_WIDTH;
static uint16_t ili_height = ILI9341_HEIGHT;

#define ILI_DMA_BUF_HEIGHT 16
#define ILI_CHAR_W         6
#define ILI_CHAR_H         8
#define ILI_BUF_SIZE_BYTES (ILI9341_WIDTH * ILI_DMA_BUF_HEIGHT * 2)

static const uint8_t font5x7[][5] = {
        {0x00, 0x00, 0x00, 0x00, 0x00},  // ' ' (Space)
        {0x00, 0x00, 0x5F, 0x00, 0x00},  // '!'
        {0x00, 0x07, 0x03, 0x07, 0x00},  // '"'
        {0x14, 0x7F, 0x14, 0x7F, 0x14},  // '#'
        {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // '$'
        {0x23, 0x13, 0x08, 0x64, 0x62},  // '%'
        {0x36, 0x49, 0x55, 0x22, 0x50},  // '&'
        {0x00, 0x05, 0x03, 0x00, 0x00},  // '''
        {0x00, 0x1C, 0x22, 0x41, 0x00},  // '('
        {0x00, 0x41, 0x22, 0x1C, 0x00},  // ')'
        {0x14, 0x08, 0x3E, 0x08, 0x14},  // '*'
        {0x08, 0x08, 0x3E, 0x08, 0x08},  // '+'
        {0x00, 0x50, 0x30, 0x00, 0x00},  // ','
        {0x08, 0x08, 0x08, 0x08, 0x08},  // '-'
        {0x00, 0x60, 0x60, 0x00, 0x00},  // '.'
        {0x20, 0x10, 0x08, 0x04, 0x02},  // '/'
        {0x3E, 0x51, 0x49, 0x45, 0x3E},  // '0'
        {0x00, 0x42, 0x7F, 0x40, 0x00},  // '1'
        {0x42, 0x61, 0x51, 0x49, 0x46},  // '2'
        {0x21, 0x41, 0x45, 0x4B, 0x31},  // '3'
        {0x18, 0x14, 0x12, 0x7F, 0x10},  // '4'
        {0x27, 0x45, 0x45, 0x45, 0x39},  // '5'
        {0x3C, 0x4A, 0x49, 0x49, 0x30},  // '6'
        {0x01, 0x71, 0x09, 0x05, 0x03},  // '7'
        {0x36, 0x49, 0x49, 0x49, 0x36},  // '8'
        {0x06, 0x49, 0x49, 0x29, 0x1E},  // '9'
        {0x00, 0x36, 0x36, 0x00, 0x00},  // ':'
        {0x00, 0x56, 0x36, 0x00, 0x00},  // ';'
        {0x08, 0x14, 0x22, 0x41, 0x00},  // '<'
        {0x14, 0x14, 0x14, 0x14, 0x14},  // '='
        {0x00, 0x41, 0x22, 0x14, 0x08},  // '>'
        {0x02, 0x01, 0x51, 0x09, 0x06},  // '?'
        {0x32, 0x49, 0x79, 0x41, 0x3E},  // '@'
        {0x7E, 0x09, 0x09, 0x09, 0x7E},  // 'A'
        {0x7F, 0x49, 0x49, 0x49, 0x36},  // 'B'
        {0x3E, 0x41, 0x41, 0x41, 0x22},  // 'C'
        {0x7F, 0x41, 0x41, 0x22, 0x1C},  // 'D'
        {0x7F, 0x49, 0x49, 0x49, 0x41},  // 'E'
        {0x7F, 0x09, 0x09, 0x09, 0x01},  // 'F'
        {0x3E, 0x41, 0x49, 0x49, 0x7A},  // 'G'
        {0x7F, 0x08, 0x08, 0x08, 0x7F},  // 'H'
        {0x00, 0x41, 0x7F, 0x41, 0x00},  // 'I'
        {0x20, 0x40, 0x41, 0x3F, 0x01},  // 'J'
        {0x7F, 0x08, 0x14, 0x22, 0x41},  // 'K'
        {0x7F, 0x40, 0x40, 0x40, 0x40},  // 'L'
        {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // 'M'
        {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 'N'
        {0x3E, 0x41, 0x41, 0x41, 0x3E},  // 'O'
        {0x7F, 0x09, 0x09, 0x09, 0x06},  // 'P'
        {0x3E, 0x41, 0x51, 0x21, 0x5E},  // 'Q'
        {0x7F, 0x09, 0x09, 0x19, 0x66},  // 'R'
        {0x26, 0x49, 0x49, 0x49, 0x32},  // 'S'
        {0x01, 0x01, 0x7F, 0x01, 0x01},  // 'T'
        {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 'U'
        {0x1F, 0x20, 0x40, 0x20, 0x1F},  // 'V'
        {0x3F, 0x40, 0x38, 0x40, 0x3F},  // 'W'
        {0x63, 0x14, 0x08, 0x14, 0x63},  // 'X'
        {0x07, 0x08, 0x70, 0x08, 0x07},  // 'Y'
        {0x61, 0x51, 0x49, 0x45, 0x43}   // 'Z'
};

static uint8_t ili_buf[ILI_BUF_SIZE_BYTES];
static volatile uint8_t ili_dma_busy = 0;

static void ILI9341_Wait(void) {
    while (ili_dma_busy) {}
}

static void ILI9341_WriteCommand(uint8_t cmd) {
    ILI9341_Wait();

    ILI_DC_Cmd();
    ILI_CS_Low();
    ILI_CS_Delay();  // tCSS
    HAL_SPI_Transmit(ili_hspi, &cmd, 1, HAL_MAX_DELAY);
    ILI_CS_Delay();  // tCSH
    ILI_CS_High();
}

static void ILI9341_WriteData(uint8_t* data, uint16_t size) {
    ILI9341_Wait();

    ILI_DC_Data();
    ILI_CS_Low();
    ILI_CS_Delay();  // tCSS
    HAL_SPI_Transmit(ili_hspi, data, size, HAL_MAX_DELAY);
    ILI_CS_Delay();  // tCSH
    ILI_CS_High();
}

void ILI9341_Init(SPI_HandleTypeDef* hspi) {
    ili_hspi = hspi;
    ILI9341_DWT_Init();

    ILI_RST_Low();
    HAL_Delay(50);
    ILI_RST_High();
    HAL_Delay(120);

    ILI9341_WriteCommand(0x01);  // Software Reset
    HAL_Delay(100);

    ILI9341_WriteCommand(0x11);  // Sleep Out
    HAL_Delay(120);

    ILI9341_WriteCommand(0x3A);  // Pixel Format Set (RGB565)
    uint8_t pf = 0x55;
    ILI9341_WriteData(&pf, 1);

    ILI9341_WriteCommand(0x36);
    uint8_t madctl = 0x48;
    ILI9341_WriteData(&madctl, 1);

    ILI9341_WriteCommand(0x29);  // Display On
    HAL_Delay(10);
}

static void ILI9341_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t data[4];

    ILI9341_WriteCommand(0x2A);
    data[0] = (x0 >> 8) & 0xFF;
    data[1] = x0 & 0xFF;
    data[2] = (x1 >> 8) & 0xFF;
    data[3] = x1 & 0xFF;
    ILI9341_WriteData(data, 4);

    ILI9341_WriteCommand(0x2B);
    data[0] = (y0 >> 8) & 0xFF;
    data[1] = y0 & 0xFF;
    data[2] = (y1 >> 8) & 0xFF;
    data[3] = y1 & 0xFF;
    ILI9341_WriteData(data, 4);

    ILI9341_WriteCommand(0x2C);
}

void ILI9341_Blit(const uint8_t* buf, uint16_t nbytes) {
    ILI9341_Wait();
    ili_dma_busy = 1;
    ili_last_tile_bytes = nbytes;
    ili_tx_start_cycles = DWT->CYCCNT;

    ILI_DC_Data();
    ILI_CS_Low();
    ILI_CS_Delay();  // tCSS
    if (HAL_SPI_Transmit_DMA(ili_hspi, (uint8_t*) buf, nbytes) != HAL_OK) {
        ILI_CS_High();
        ili_dma_busy = 0;
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef* hspi) {
    if (hspi == ili_hspi) {
        uint32_t cycles = DWT->CYCCNT - ili_tx_start_cycles;
        ili_last_tile_us = cycles / (SystemCoreClock / 1000000);

        ILI_CS_Delay();  // tCSH
        ILI_CS_High();
        ili_dma_busy = 0;
    }
}

uint32_t ILI9341_GetSckHz(void) {
    return HAL_RCC_GetPCLK2Freq() / 8;
}

void ILI9341_GetLastTileMetrics(uint32_t* us, uint32_t* bytes) {
    *us = ili_last_tile_us;
    *bytes = ili_last_tile_bytes;
}

void ILI9341_SetOrientation(ili_orientation_t orientation) {
    uint8_t madctl = (orientation == ILI_LANDSCAPE) ? 0x28 : 0x48;

    ILI9341_WriteCommand(0x36);
    ILI9341_WriteData(&madctl, 1);

    ili_orientation = orientation;
    if (orientation == ILI_LANDSCAPE) {
        ili_width = ILI9341_HEIGHT;
        ili_height = ILI9341_WIDTH;
    } else {
        ili_width = ILI9341_WIDTH;
        ili_height = ILI9341_HEIGHT;
    }
}

ili_orientation_t ILI9341_GetOrientation(void) {
    return ili_orientation;
}

uint16_t ILI9341_GetWidth(void) {
    return ili_width;
}

uint16_t ILI9341_GetHeight(void) {
    return ili_height;
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef* hspi) {
    if (hspi == ili_hspi) {
        ILI_CS_High();
        ili_dma_busy = 0;
    }
}

void ILI9341_FillColor(uint16_t color) {
    ILI9341_Wait();

    ILI9341_SetWindow(0, 0, ili_width - 1, ili_height - 1);

    const uint8_t high_byte = color >> 8;
    const uint8_t low_byte = color & 0xFF;

    for (size_t i = 0; i < ILI_BUF_SIZE_BYTES; i += 2) {
        ili_buf[i] = high_byte;
        ili_buf[i + 1] = low_byte;
    }

    uint32_t line_bytes = (uint32_t) ili_width * 2;
    uint32_t lines_per_chunk = ILI_BUF_SIZE_BYTES / line_bytes;
    uint32_t remaining = ili_height;
    while (remaining > 0) {
        uint32_t chunk_lines = (remaining < lines_per_chunk) ? remaining : lines_per_chunk;
        ILI9341_Blit(ili_buf, (uint16_t) (chunk_lines * line_bytes));
        remaining -= chunk_lines;
    }

    ILI9341_Wait();
}

static void ILI9341_RenderChar(uint16_t x_off, uint16_t tile_w, char c, uint16_t color, uint16_t bg_color) {
    if (c >= 'a' && c <= 'z') { c -= ('a' - 'A'); }  // Font supports only uppercase letters
    uint8_t char_idx = ('?' - ' ');                  // Placeholder for unsupported characters
    if (c >= ' ' && c <= 'Z') { char_idx = c - ' '; }

    for (uint8_t row = 0; row < ILI_CHAR_H; ++row) {
        for (uint8_t col = 0; col < ILI_CHAR_W; ++col) {
            uint16_t px_color = bg_color;
            if (col < 5 && row < 7 && (font5x7[char_idx][col] & (1 << row))) { px_color = color; }

            uint32_t idx = ((uint32_t) row * tile_w + x_off + col) * 2;
            ili_buf[idx] = px_color >> 8;
            ili_buf[idx + 1] = px_color & 0xFF;
        }
    }
}

void ILI9341_DrawText(uint16_t x, uint16_t y, const char* str, uint16_t color, uint16_t bg_color) {
    uint16_t n = 0;
    while (str[n] && (x + (n + 1) * ILI_CHAR_W) <= ili_width) { n++; }
    if (n == 0) return;

    uint16_t tile_w = n * ILI_CHAR_W;

    ILI9341_Wait();
    for (uint16_t i = 0; i < n; i++) { ILI9341_RenderChar(i * ILI_CHAR_W, tile_w, str[i], color, bg_color); }

    ILI9341_SetWindow(x, y, x + tile_w - 1, y + ILI_CHAR_H - 1);
    ILI9341_Blit(ili_buf, tile_w * ILI_CHAR_H * 2);
}

void ILI9341_TestColors(uint16_t delay_ms) {
    ILI9341_FillColor(COLOR_BLACK);
    HAL_Delay(delay_ms);
    ILI9341_FillColor(COLOR_WHITE);
    HAL_Delay(delay_ms);
    ILI9341_FillColor(COLOR_RED);
    HAL_Delay(delay_ms);
    ILI9341_FillColor(COLOR_GREEN);
    HAL_Delay(delay_ms);
    ILI9341_FillColor(COLOR_BLUE);
    HAL_Delay(delay_ms);
    ILI9341_FillColor(COLOR_YELLOW);
    HAL_Delay(delay_ms);
    ILI9341_FillColor(COLOR_CYAN);
    HAL_Delay(delay_ms);
    ILI9341_FillColor(COLOR_ORANGE);
    HAL_Delay(delay_ms);
}
