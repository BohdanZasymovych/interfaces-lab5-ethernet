#include "utils/gps_flash.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static uint32_t gps_flash_last_save_ms = 0;

static uint32_t GPS_Flash_Checksum(const GPS_FlashRecord_t* rec) {
    const uint8_t* bytes = (const uint8_t*) rec;
    size_t len = offsetof(GPS_FlashRecord_t, checksum);
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) { sum = (sum * 31) + bytes[i]; }
    return sum;
}

void GPS_Flash_Load(void) {
    const GPS_FlashRecord_t* stored = (const GPS_FlashRecord_t*) GPS_FLASH_STORAGE_ADDR;

    if (stored->magic != GPS_FLASH_MAGIC) { return; }
    if (GPS_Flash_Checksum(stored) != stored->checksum) { return; }

    gps_last_valid_fix.latitude = stored->latitude;
    gps_last_valid_fix.longitude = stored->longitude;
    gps_last_valid_fix.ns = stored->ns;
    gps_last_valid_fix.ew = stored->ew;
    strncpy(gps_last_valid_fix.time_utc, stored->time_utc, sizeof(gps_last_valid_fix.time_utc) - 1);
    strncpy(gps_last_valid_fix.date_utc, stored->date_utc, sizeof(gps_last_valid_fix.date_utc) - 1);
    gps_last_valid_fix.satellites = stored->satellites;
    gps_last_valid_fix.fix_status = stored->fix_status;

    gps_last_valid_fix.is_stale = 1;
    gps_last_valid_fix.timestamp_ms = 0;
}

static void GPS_Flash_Save(void) {
    GPS_FlashRecord_t rec = {0};
    rec.magic = GPS_FLASH_MAGIC;
    rec.latitude = gps_last_valid_fix.latitude;
    rec.longitude = gps_last_valid_fix.longitude;
    rec.ns = gps_last_valid_fix.ns;
    rec.ew = gps_last_valid_fix.ew;
    strncpy(rec.time_utc, gps_last_valid_fix.time_utc, sizeof(rec.time_utc) - 1);
    strncpy(rec.date_utc, gps_last_valid_fix.date_utc, sizeof(rec.date_utc) - 1);
    rec.satellites = gps_last_valid_fix.satellites;
    rec.fix_status = gps_last_valid_fix.fix_status;
    rec.checksum = GPS_Flash_Checksum(&rec);

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef eraseInit = {.TypeErase = FLASH_TYPEERASE_SECTORS,
                                        .Sector = GPS_FLASH_STORAGE_SECTOR,
                                        .NbSectors = 1,
                                        .VoltageRange = FLASH_VOLTAGE_RANGE_3};
    uint32_t sectorError = 0;
    if (HAL_FLASHEx_Erase(&eraseInit, &sectorError) == HAL_OK) {
        const uint8_t* src = (const uint8_t*) &rec;
        uint32_t addr = GPS_FLASH_STORAGE_ADDR;
        for (size_t i = 0; i < sizeof(rec); i += 4) {
            uint32_t word;
            memcpy(&word, src + i, 4);
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word);
        }
    }

    HAL_FLASH_Lock();
}

void GPS_Flash_Save_If_Due(void) {
    uint32_t now = HAL_GetTick();

    if (gps_last_valid_fix.fix_status != 1) { return; }
    if (gps_flash_last_save_ms != 0 && (now - gps_flash_last_save_ms) < GPS_FLASH_SAVE_INTERVAL_MS) { return; }

    GPS_Flash_Save();
    gps_flash_last_save_ms = HAL_GetTick();
}

void GPS_Flash_Force_Save(void) {
    GPS_Flash_Save();
    gps_flash_last_save_ms = HAL_GetTick();
}

void GPS_Flash_Report(char* out, size_t out_len) {
    const GPS_FlashRecord_t* stored = (const GPS_FlashRecord_t*) GPS_FLASH_STORAGE_ADDR;

    if (stored->magic != GPS_FLASH_MAGIC) {
        snprintf(out, out_len, "FLASH: empty/erased (magic=0x%08lX)\r\n", (unsigned long) stored->magic);
        return;
    }

    uint32_t calc = GPS_Flash_Checksum(stored);
    if (calc != stored->checksum) {
        snprintf(out, out_len, "FLASH: CORRUPT (checksum=0x%08lX, expected=0x%08lX)\r\n",
                 (unsigned long) stored->checksum, (unsigned long) calc);
        return;
    }

    char lat_str[20], lon_str[20];
    GPS_GetFormattedLatitude(stored->latitude, lat_str, sizeof(lat_str));
    GPS_GetFormattedLongitude(stored->longitude, lon_str, sizeof(lon_str));

    snprintf(out, out_len, "FLASH: VALID | LAT:%s,LON:%s | SATS:%u | FIX:%u | %s %s\r\n", lat_str, lon_str,
             stored->satellites, stored->fix_status, stored->date_utc, stored->time_utc);
}
