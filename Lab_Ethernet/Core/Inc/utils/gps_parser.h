#ifndef GPS_PARSER_H
#define GPS_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#define GPS_DMA_BUF_SIZE     256
#define GPS_LINE_BUF_SIZE    128
#define GPS_STALE_TIMEOUT_MS 5000

#define GPS_LINE_QUEUE_SIZE 6

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_chunks;
    uint32_t err_ore;
    uint32_t err_fe;
    uint32_t err_ne;
    uint32_t err_pe;
    uint32_t dropped_lines;
} GPS_Stats_t;

typedef struct {
    double latitude;
    double longitude;
    char ns;
    char ew;
    char time_utc[16];
    char date_utc[16];
    uint8_t satellites;
    uint8_t fix_status;
    uint8_t is_stale;
    uint32_t timestamp_ms;
} GPS_FixData_t;

extern GPS_Stats_t gps_stats;
extern GPS_FixData_t gps_current_fix;
extern GPS_FixData_t gps_last_valid_fix;
extern bool gps_raw_enabled;

void GPS_Init(UART_HandleTypeDef* huart);
void Process_GPS_DMA_Data(void);
void Parse_NMEA_Sentence(const char* sentence);

bool GPS_DequeueLine(char* dst, uint16_t max_len);
void GPS_GetFormattedTime(const char* utc_str, char* dst, uint8_t max_len);
void GPS_GetFormattedLatitude(double lat, char* dst, uint8_t max_len);
void GPS_GetFormattedLongitude(double lon, char* dst, uint8_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* GPS_PARSER_H */
