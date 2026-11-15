#ifndef BME280_H
#define BME280_H

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#define BME280_STALE_TIMEOUT_MS 3000u

typedef struct {
    float temperature;
    float pressure;
    float humidity;
    uint32_t timestamp_ms;
    bool is_stale;
} BME280_Data_t;

typedef struct {
    uint16_t dig_T1;
    int16_t dig_T2;
    int16_t dig_T3;
    uint16_t dig_P1;
    int16_t dig_P2;
    int16_t dig_P3;
    int16_t dig_P4;
    int16_t dig_P5;
    int16_t dig_P6;
    int16_t dig_P7;
    int16_t dig_P8;
    int16_t dig_P9;
    uint8_t dig_H1;
    int16_t dig_H2;
    uint8_t dig_H3;
    int16_t dig_H4;
    int16_t dig_H5;
    int8_t dig_H6;
} bme280_calib_data_t;

extern BME280_Data_t bme_data;
extern bool bmeSensorInitialized;

bool BME280_Init_Check(I2C_HandleTypeDef* hi2c);
bool BME280_Read_Calib(I2C_HandleTypeDef* hi2c);
bool BME280_Start(I2C_HandleTypeDef* hi2c);
bool BME280_Init(I2C_HandleTypeDef* hi2c);
void BME280_Read_Real_Data(I2C_HandleTypeDef* hi2c);
void BME280_Update_Stale(void);

#endif /* BME280_H */
