#ifndef TCS3472_H
#define TCS3472_H

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#define TCS3472_ADDR (0x29 << 1)

#define TCS3472_PON_BIT 0b00000001  // Powers sensor on
#define TCS3472_AEN_BIT 0b00000010  // Enables ADC

#define TCS3472_CMD_BIT  0b10000000  // Bit specifying the command
#define TCS3472_AUTO_INC 0b00100000  // Bits specifying auto increment transaction

#define TCS3472_REG_ENB   0x00  // Address of the enable register
#define TCS3472_REG_CDATA 0x14  // Address of the clear data low byte

#define TCS3472_DATA_SIZE_BYTES 8u  // Size in bytes of the data for 4 colors (2 bytes per color)

#define TCS3472_STALE_TIMEOUT_MS 3000u  // Time after which read data of the sensor is considered stale

typedef struct {
    uint16_t clear;
    uint16_t red;
    uint16_t green;
    uint16_t blue;
    uint32_t lastTimestamp;
    bool isStale;
} TCS3472_Data_t;

extern TCS3472_Data_t colorSensorData;
extern bool colorSensorInitialized;

bool TCS3472_Init(I2C_HandleTypeDef* i2c_struct);
void TCS3472_ReadData(I2C_HandleTypeDef* i2c_struct, TCS3472_Data_t* sensorData);
void TCS3472_UpdateStale(TCS3472_Data_t* sensorData);

#endif
