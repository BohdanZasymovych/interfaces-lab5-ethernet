#include "utils/gps_parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

GPS_FixData_t gps_current_fix = {0};
GPS_FixData_t gps_last_valid_fix = {0};
GPS_Stats_t gps_stats = {0};

uint8_t gps_dma_buf[GPS_DMA_BUF_SIZE];
uint16_t old_pos = 0;

char gps_line_buf[GPS_LINE_BUF_SIZE];
uint16_t line_idx = 0;
bool gps_raw_enabled = false;

static char gps_line_queue[GPS_LINE_QUEUE_SIZE][GPS_LINE_BUF_SIZE];
static uint8_t gps_queue_head = 0;  // next slot to write
static uint8_t gps_queue_tail = 0;  // next slot to read
static uint8_t gps_queue_count = 0;

static void GPS_EnqueueLine(const char* line) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (gps_queue_count >= GPS_LINE_QUEUE_SIZE) {
        gps_stats.dropped_lines++;
    } else {
        strncpy(gps_line_queue[gps_queue_head], line, GPS_LINE_BUF_SIZE - 1);
        gps_line_queue[gps_queue_head][GPS_LINE_BUF_SIZE - 1] = '\0';
        gps_queue_head = (uint8_t) ((gps_queue_head + 1) % GPS_LINE_QUEUE_SIZE);
        gps_queue_count++;
    }

    if (!primask) { __enable_irq(); }
}

bool GPS_DequeueLine(char* dst, uint16_t max_len) {
    bool got_line = false;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (gps_queue_count > 0) {
        strncpy(dst, gps_line_queue[gps_queue_tail], max_len - 1);
        dst[max_len - 1] = '\0';
        gps_queue_tail = (uint8_t) ((gps_queue_tail + 1) % GPS_LINE_QUEUE_SIZE);
        gps_queue_count--;
        got_line = true;
    }

    if (!primask) { __enable_irq(); }
    return got_line;
}

static UART_HandleTypeDef* gps_huart;

void GPS_Init(UART_HandleTypeDef* huart) {
    gps_huart = huart;
    HAL_UART_Receive_DMA(gps_huart, gps_dma_buf, GPS_DMA_BUF_SIZE);
    __HAL_UART_ENABLE_IT(gps_huart, UART_IT_IDLE);
}

static double NMEA_To_DecimalDegrees(const char* raw_val, char dir) {
    if (raw_val == NULL || strlen(raw_val) < 4) return 0.0;

    double raw_num = atof(raw_val);
    int degrees = (int) (raw_num / 100.0);
    double minutes = raw_num - (degrees * 100.0);
    double dec_degrees = (double) degrees + (minutes / 60.0);

    if (dir == 'S' || dir == 'W') { dec_degrees = -dec_degrees; }

    return dec_degrees;
}

static void Get_NMEA_Field(const char* src, uint8_t field_idx, char* dst, uint8_t max_len) {
    dst[0] = '\0';
    uint8_t current_field = 0;
    uint8_t dst_i = 0;

    for (uint16_t i = 0; src[i] != '\0' && src[i] != '*'; i++) {
        if (src[i] == ',') {
            current_field++;
            if (current_field > field_idx) break;
            continue;
        }

        if (current_field == field_idx) {
            if (dst_i < max_len - 1) { dst[dst_i++] = src[i]; }
        }
    }
    dst[dst_i] = '\0';
}

static uint8_t Validate_NMEA_Checksum(const char* sentence) {
    const char* start = strchr(sentence, '$');
    if (!start) return 0;
    start++;

    const char* star = strchr(start, '*');
    if (!star) return 0;

    uint8_t calculated_checksum = 0;
    for (const char* p = start; p < star; p++) { calculated_checksum ^= (uint8_t) (*p); }

    uint8_t received_checksum = (uint8_t) strtoul(star + 1, NULL, 16);
    return (calculated_checksum == received_checksum);
}

void Parse_NMEA_Sentence(const char* nmea_str) {
    if (!Validate_NMEA_Checksum(nmea_str)) { return; }

    if (strstr(nmea_str, "$GPRMC") != NULL || strstr(nmea_str, "$GNRMC") != NULL) {
        char utc[16] = {0};
        char status[4] = {0};
        char raw_lat[16] = {0};
        char ns[4] = {0};
        char raw_lon[16] = {0};
        char ew[4] = {0};
        char date[16] = {0};

        Get_NMEA_Field(nmea_str, 1, utc, sizeof(utc));
        Get_NMEA_Field(nmea_str, 2, status, sizeof(status));
        Get_NMEA_Field(nmea_str, 3, raw_lat, sizeof(raw_lat));
        Get_NMEA_Field(nmea_str, 4, ns, sizeof(ns));
        Get_NMEA_Field(nmea_str, 5, raw_lon, sizeof(raw_lon));
        Get_NMEA_Field(nmea_str, 6, ew, sizeof(ew));
        Get_NMEA_Field(nmea_str, 9, date, sizeof(date));

        if (utc[0] >= '0' && utc[0] <= '9') {
            strncpy(gps_current_fix.time_utc, utc, sizeof(gps_current_fix.time_utc) - 1);
        }
        if (date[0] >= '0' && date[0] <= '9') {
            strncpy(gps_current_fix.date_utc, date, sizeof(gps_current_fix.date_utc) - 1);
        }

        if (status[0] == 'A') {
            char dir_ns = ns[0] ? ns[0] : 'N';
            char dir_ew = ew[0] ? ew[0] : 'E';

            gps_current_fix.latitude = NMEA_To_DecimalDegrees(raw_lat, dir_ns);
            gps_current_fix.longitude = NMEA_To_DecimalDegrees(raw_lon, dir_ew);
            gps_current_fix.ns = dir_ns;
            gps_current_fix.ew = dir_ew;
            gps_current_fix.fix_status = 1;

            gps_last_valid_fix = gps_current_fix;
            gps_last_valid_fix.timestamp_ms = HAL_GetTick();
            gps_last_valid_fix.is_stale = 0;
        } else {
            gps_current_fix.fix_status = 0;
        }
    } else if (strstr(nmea_str, "$GPGGA") != NULL || strstr(nmea_str, "$GNGGA") != NULL) {
        char sats_str[8] = {0};
        Get_NMEA_Field(nmea_str, 7, sats_str, sizeof(sats_str));
        if (sats_str[0] != '\0') {
            gps_current_fix.satellites = (uint8_t) atoi(sats_str);
            gps_last_valid_fix.satellites = gps_current_fix.satellites;
        }
    }
}

void GPS_GetFormattedTime(const char* utc_str, char* dst, uint8_t max_len) {
    int hh = 0, mm = 0, ss = 0;
    if (utc_str != NULL && strlen(utc_str) >= 6) {
        sscanf(utc_str, "%2d%2d%2d", &hh, &mm, &ss);
        snprintf(dst, max_len, "%02d:%02d:%02d", hh, mm, ss);
    } else {
        snprintf(dst, max_len, "--:--:--");
    }
}

void GPS_GetFormattedLatitude(double lat, char* dst, uint8_t max_len) {
    int lat_deg = (int) fabs(lat);
    int lat_frac = (int) ((fabs(lat) - lat_deg) * 1000000.0);
    snprintf(dst, max_len, "%s%d.%06d %c", (lat < 0) ? "-" : "", lat_deg, lat_frac, (lat < 0) ? 'S' : 'N');
}

void GPS_GetFormattedLongitude(double lon, char* dst, uint8_t max_len) {
    int lon_deg = (int) fabs(lon);
    int lon_frac = (int) ((fabs(lon) - lon_deg) * 1000000.0);
    snprintf(dst, max_len, "%s%d.%06d %c", (lon < 0) ? "-" : "", lon_deg, lon_frac, (lon < 0) ? 'W' : 'E');
}

void Process_GPS_DMA_Data(void) {
    uint16_t pos = GPS_DMA_BUF_SIZE - __HAL_DMA_GET_COUNTER(gps_huart->hdmarx);

    while (old_pos != pos) {
        uint8_t ch = gps_dma_buf[old_pos];
        gps_stats.rx_bytes++;

        if (ch == '\n') {
            gps_line_buf[line_idx] = '\0';
            GPS_EnqueueLine(gps_line_buf);  // every line in the burst, not just the first
            line_idx = 0;
        } else if (ch != '\r') {
            if (line_idx < GPS_LINE_BUF_SIZE - 1) {
                gps_line_buf[line_idx++] = ch;
            } else {
                line_idx = 0;
            }
        }

        old_pos++;
        if (old_pos >= GPS_DMA_BUF_SIZE) { old_pos = 0; }
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart) {
    if (huart->Instance == USART1) { Process_GPS_DMA_Data(); }
}

void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef* huart) {
    if (huart->Instance == USART1) { Process_GPS_DMA_Data(); }
}
