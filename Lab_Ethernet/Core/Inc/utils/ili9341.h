#ifndef ILI9341_H
#define ILI9341_H

#include "main.h"

#define ILI9341_WIDTH  240
#define ILI9341_HEIGHT 320

#define COLOR_BLACK  0x0000
#define COLOR_WHITE  0xFFFF
#define COLOR_RED    0xF800
#define COLOR_GREEN  0x07E0
#define COLOR_BLUE   0x001F
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN   0x07FF
#define COLOR_ORANGE 0xFD20

typedef enum {
    ILI_PORTRAIT = 0,
    ILI_LANDSCAPE = 1,
} ili_orientation_t;

void ILI9341_Init(SPI_HandleTypeDef* hspi);
void ILI9341_Blit(const uint8_t* buf, uint16_t nbytes);
uint8_t ILI9341_IsBusy(void);
void ILI9341_FillColor(uint16_t color);
void ILI9341_DrawText(uint16_t x, uint16_t y, const char* str, uint16_t color, uint16_t bg_color);
void ILI9341_TestColors(uint16_t delay);
uint32_t ILI9341_GetSckHz(void);
void ILI9341_GetLastTileMetrics(uint32_t* us, uint32_t* bytes);

// Orientation: swaps MADCTL's row/column-exchange bit and the active width/height.
// Only reprograms the panel; the caller must clear and redraw the screen afterwards
// (e.g. via UI_Redraw), since old GRAM content isn't remapped.
void ILI9341_SetOrientation(ili_orientation_t orientation);
ili_orientation_t ILI9341_GetOrientation(void);
uint16_t ILI9341_GetWidth(void);
uint16_t ILI9341_GetHeight(void);

#endif
