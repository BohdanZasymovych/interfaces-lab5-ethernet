#include "utils/tcs3472.h"

TCS3472_Data_t colorSensorData = {.isStale = true};
bool colorSensorInitialized = false;

bool TCS3472_Init(I2C_HandleTypeDef* i2c_struct) {
    uint8_t cmdByte = (uint8_t) (TCS3472_CMD_BIT | TCS3472_REG_ENB);

    uint8_t enblByte = (uint8_t) (TCS3472_PON_BIT);
    if (HAL_I2C_Mem_Write(i2c_struct, TCS3472_ADDR, cmdByte, I2C_MEMADD_SIZE_8BIT, &enblByte, 1, 50) != HAL_OK)
        return false;
    HAL_Delay(3);

    enblByte = (uint8_t) (TCS3472_PON_BIT | TCS3472_AEN_BIT);
    if (HAL_I2C_Mem_Write(i2c_struct, TCS3472_ADDR, cmdByte, I2C_MEMADD_SIZE_8BIT, &enblByte, 1, 50) != HAL_OK)
        return false;
    HAL_Delay(3);

    return true;
}

void TCS3472_ReadData(I2C_HandleTypeDef* i2c_struct, TCS3472_Data_t* sensorData) {
    uint8_t cmdByte = TCS3472_CMD_BIT | TCS3472_AUTO_INC | TCS3472_REG_CDATA;

    uint8_t rawData[TCS3472_DATA_SIZE_BYTES];

    if (HAL_I2C_Mem_Read(i2c_struct, TCS3472_ADDR, cmdByte, I2C_MEMADD_SIZE_8BIT, rawData, TCS3472_DATA_SIZE_BYTES,
                         50) != HAL_OK)
        return;

    sensorData->clear = (uint16_t) (rawData[1] << 8 | rawData[0]);
    sensorData->red = (uint16_t) (rawData[3] << 8 | rawData[2]);
    sensorData->green = (uint16_t) (rawData[5] << 8 | rawData[4]);
    sensorData->blue = (uint16_t) (rawData[7] << 8 | rawData[6]);

    sensorData->lastTimestamp = HAL_GetTick();
    sensorData->isStale = false;
}

void TCS3472_UpdateStale(TCS3472_Data_t* sensorData) {
    if (((HAL_GetTick() - sensorData->lastTimestamp) > TCS3472_STALE_TIMEOUT_MS) || (sensorData->lastTimestamp == 0)) {
        sensorData->isStale = true;
    } else {
        sensorData->isStale = false;
    }
}