#ifndef GPS_FLASH_H
#define GPS_FLASH_H

#include "main.h"
#include "utils/gps_parser.h"

#define GPS_FLASH_STORAGE_ADDR     0x08040000UL
#define GPS_FLASH_STORAGE_SECTOR   FLASH_SECTOR_6
#define GPS_FLASH_MAGIC            0xCAFEF00DUL
#define GPS_FLASH_SAVE_INTERVAL_MS 30000

typedef struct {
    uint32_t magic;
    double latitude;
    double longitude;
    char ns;
    char ew;
    char time_utc[16];
    char date_utc[16];
    uint8_t satellites;
    uint8_t fix_status;
    uint32_t checksum;
} GPS_FlashRecord_t;

void GPS_Flash_Load(void);
void GPS_Flash_Save_If_Due(void);
void GPS_Flash_Force_Save(void);
void GPS_Flash_Report(char* out, size_t out_len);

#endif
