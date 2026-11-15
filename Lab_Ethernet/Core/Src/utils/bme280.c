#include "bme280.h"
#include <stdio.h>

#define BME280_I2C_ADDR     (0x76 << 1)
#define BME280_REG_ID       0xD0
#define BME280_CHIP_ID      0x60
#define BME280_REG_CTRL_HUM 0xF2
#define BME280_REG_STATUS   0xF3
#define BME280_REG_CTRL     0xF4
#define BME280_REG_CONFIG   0xF5
#define BME280_REG_DATA     0xF7

#define BME280_CTRL_HUM_X1      0x01  // humidity oversampling x1
#define BME280_CTRL_MEAS_SLEEP  0x24  // temperature x1, pressure x1, mode = sleep
#define BME280_CTRL_MEAS_FORCED 0x25  // temperature x1, pressure x1, mode = forced
#define BME280_CONFIG_VAL       0x08  // standby time unused in forced mode, IIR filter coefficient 4, 3-wire SPI off
#define BME280_STATUS_MEASURING 0x08  // status bit 3
#define BME280_MEAS_TIME_MS     10u   // max conversion time at x1 oversampling is about 9.3 ms
#define BME280_MEAS_POLL_MAX    10u   // extra 1 ms polls of the status register after the fixed wait

BME280_Data_t bme_data = {0, .is_stale = true};
bool bmeSensorInitialized = false;

static bme280_calib_data_t calib;
static int32_t t_fine;

static int32_t BME280_Compensate_T(int32_t adc_t) {
    int32_t var1, var2;
    var1 = (((adc_t >> 3) - ((int32_t) calib.dig_T1 << 1))) * ((int32_t) calib.dig_T2) >> 11;
    var2 = (((((adc_t >> 4) - ((int32_t) calib.dig_T1)) * ((adc_t >> 4) - ((int32_t) calib.dig_T1))) >> 12) *
            ((int32_t) calib.dig_T3)) >>
           14;
    t_fine = var1 + var2;
    return (t_fine * 5 + 128) >> 8;
}

static uint32_t BME280_Compensate_P(int32_t adc_p) {
    int64_t var1, var2, p;
    var1 = ((int64_t) t_fine) - 128000;
    var2 = var1 * var1 * (int64_t) calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t) calib.dig_P5) << 17);
    var2 = var2 + (((int64_t) calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t) calib.dig_P3) >> 8) + ((var1 * (int64_t) calib.dig_P2) << 12);
    var1 = (((int64_t) 1 << 47) + var1) * ((int64_t) calib.dig_P1) >> 33;

    if (var1 == 0) return 0;

    p = 1048576 - adc_p;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t) calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t) calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t) calib.dig_P7) << 4);
    return (uint32_t) (p >> 8);
}

static uint32_t BME280_Compensate_H(int32_t adc_h) {
    int32_t v_x1_u32r;
    v_x1_u32r = (t_fine - ((int32_t) 76800));
    v_x1_u32r = (((((adc_h << 14) - (((int32_t) calib.dig_H4) << 20) - (((int32_t) calib.dig_H5) * v_x1_u32r)) +
                   ((int32_t) 16384)) >>
                  15) *
                 (((((((v_x1_u32r * ((int32_t) calib.dig_H6)) >> 10) *
                      (((v_x1_u32r * ((int32_t) calib.dig_H3)) >> 11) + ((int32_t) 32768))) >>
                     10) +
                    ((int32_t) 2097152)) *
                           ((int32_t) calib.dig_H2) +
                   8192) >>
                  14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t) calib.dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    return (uint32_t) (v_x1_u32r >> 12);
}

bool BME280_Init_Check(I2C_HandleTypeDef* hi2c) {
    uint8_t chip_id = 0;
    if (HAL_I2C_Mem_Read(hi2c, BME280_I2C_ADDR, BME280_REG_ID, I2C_MEMADD_SIZE_8BIT, &chip_id, 1, 100) == HAL_OK) {
        if (chip_id != BME280_CHIP_ID) {
            printf("Unexpected chip ID at BME280 address: 0x%02X\r\n", chip_id);
            return false;
        }
        printf("BME280 found! Chip ID: 0x%02X\r\n", chip_id);
        return true;
    }

    printf("BME280 not found on I2C!\r\n");
    return false;
}

bool BME280_Read_Calib(I2C_HandleTypeDef* hi2c) {
    uint8_t buf[24];
    uint8_t h1;
    uint8_t h_buf[7];

    if (HAL_I2C_Mem_Read(hi2c, BME280_I2C_ADDR, 0x88, I2C_MEMADD_SIZE_8BIT, buf, 24, 100) != HAL_OK) return false;
    if (HAL_I2C_Mem_Read(hi2c, BME280_I2C_ADDR, 0xA1, I2C_MEMADD_SIZE_8BIT, &h1, 1, 100) != HAL_OK) return false;
    if (HAL_I2C_Mem_Read(hi2c, BME280_I2C_ADDR, 0xE1, I2C_MEMADD_SIZE_8BIT, h_buf, 7, 100) != HAL_OK) return false;

    calib.dig_T1 = (buf[1] << 8) | buf[0];
    calib.dig_T2 = (int16_t) ((buf[3] << 8) | buf[2]);
    calib.dig_T3 = (int16_t) ((buf[5] << 8) | buf[4]);

    calib.dig_P1 = (uint16_t) ((buf[7] << 8) | buf[6]);
    calib.dig_P2 = (int16_t) ((buf[9] << 8) | buf[8]);
    calib.dig_P3 = (int16_t) ((buf[11] << 8) | buf[10]);
    calib.dig_P4 = (int16_t) ((buf[13] << 8) | buf[12]);
    calib.dig_P5 = (int16_t) ((buf[15] << 8) | buf[14]);
    calib.dig_P6 = (int16_t) ((buf[17] << 8) | buf[16]);
    calib.dig_P7 = (int16_t) ((buf[19] << 8) | buf[18]);
    calib.dig_P8 = (int16_t) ((buf[21] << 8) | buf[20]);
    calib.dig_P9 = (int16_t) ((buf[23] << 8) | buf[22]);

    calib.dig_H1 = h1;

    calib.dig_H2 = (int16_t) ((h_buf[1] << 8) | h_buf[0]);
    calib.dig_H3 = h_buf[2];
    calib.dig_H4 = (int16_t) ((h_buf[3] << 4) | (h_buf[4] & 0x0F));
    calib.dig_H5 = (int16_t) ((h_buf[5] << 4) | (h_buf[4] >> 4));
    calib.dig_H6 = (int8_t) h_buf[6];

    return true;
}

static bool BME280_Write_Reg(I2C_HandleTypeDef* hi2c, uint8_t reg, uint8_t value) {
    return HAL_I2C_Mem_Write(hi2c, BME280_I2C_ADDR, reg, I2C_MEMADD_SIZE_8BIT, &value, 1, 100) == HAL_OK;
}

bool BME280_Start(I2C_HandleTypeDef* hi2c) {
    if (!BME280_Write_Reg(hi2c, BME280_REG_CTRL, BME280_CTRL_MEAS_SLEEP)) return false;
    if (!BME280_Write_Reg(hi2c, BME280_REG_CONFIG, BME280_CONFIG_VAL)) return false;

    if (!BME280_Write_Reg(hi2c, BME280_REG_CTRL_HUM, BME280_CTRL_HUM_X1)) return false;
    return BME280_Write_Reg(hi2c, BME280_REG_CTRL, BME280_CTRL_MEAS_SLEEP);
}

static bool BME280_Measure(I2C_HandleTypeDef* hi2c) {
    if (!BME280_Write_Reg(hi2c, BME280_REG_CTRL, BME280_CTRL_MEAS_FORCED)) return false;

    HAL_Delay(BME280_MEAS_TIME_MS);

    for (uint32_t i = 0; i < BME280_MEAS_POLL_MAX; i++) {
        uint8_t status;
        if (HAL_I2C_Mem_Read(hi2c, BME280_I2C_ADDR, BME280_REG_STATUS, I2C_MEMADD_SIZE_8BIT, &status, 1, 100) != HAL_OK)
            return false;
        if (!(status & BME280_STATUS_MEASURING)) return true;
        HAL_Delay(1);
    }
    return false;
}

bool BME280_Init(I2C_HandleTypeDef* hi2c) {
    return BME280_Init_Check(hi2c) && BME280_Read_Calib(hi2c) && BME280_Start(hi2c);
}

void BME280_Read_Real_Data(I2C_HandleTypeDef* hi2c) {
    uint8_t data[8];
    if (BME280_Measure(hi2c) &&
        HAL_I2C_Mem_Read(hi2c, BME280_I2C_ADDR, BME280_REG_DATA, I2C_MEMADD_SIZE_8BIT, data, 8, 100) == HAL_OK) {
        int32_t adc_p = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
        int32_t adc_t = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);
        int32_t adc_h = (data[6] << 8) | data[7];

        int32_t temp_calibrated = BME280_Compensate_T(adc_t);
        uint32_t press_calibrated = BME280_Compensate_P(adc_p);
        uint32_t hum_calibrated = BME280_Compensate_H(adc_h);

        bme_data.temperature = (float) temp_calibrated / 100.0f;
        bme_data.pressure = (float) press_calibrated / 100.0f;
        bme_data.humidity = (float) hum_calibrated / 1024.0f;
        bme_data.timestamp_ms = HAL_GetTick();
        bme_data.is_stale = false;

        int32_t temp_int = temp_calibrated / 100;
        int32_t temp_frac = temp_calibrated % 100;
        if (temp_frac < 0) temp_frac = -temp_frac;

        uint32_t press_hpa = press_calibrated / 100;

        uint32_t hum_int = hum_calibrated / 1024;
        uint32_t hum_frac = (hum_calibrated % 1024) * 100 / 1024;

        printf("Temp: %ld.%02ld C | Press: %ld hPa | Hum: %ld.%02ld %%\r\n", temp_int, temp_frac, press_hpa, hum_int,
               hum_frac);
    } else {
        printf("Failed to read BME280 data\r\n");
    }
}

void BME280_Update_Stale(void) {
    if (((HAL_GetTick() - bme_data.timestamp_ms) > BME280_STALE_TIMEOUT_MS) || (bme_data.timestamp_ms == 0)) {
        bme_data.is_stale = true;
    } else {
        bme_data.is_stale = false;
    }
}
