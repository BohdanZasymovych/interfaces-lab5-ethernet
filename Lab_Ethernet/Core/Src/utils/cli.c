#include "utils/cli.h"
#include "utils/gps_parser.h"
#include "utils/gps_flash.h"
#include "utils/ili9341.h"
#include "utils/bme280.h"
#include "utils/tcs3472.h"
#include "utils/net.h"
#include "utils/http_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

volatile bool cli_test_colors_requested = false;
volatile bool cli_orientation_toggle_requested = false;
volatile bool cli_get_bme_requested = false;
volatile bool cli_get_color_requested = false;
volatile bool cli_get_net_requested = false;

static void CLI_Send_Response(const char* resp) {
    char full_resp[300];
    snprintf(full_resp, sizeof(full_resp), "[CLI] %s", resp);
    USB_CDC_Send_Async((const uint8_t*) full_resp, strlen(full_resp));
}

static void Format_ISO_DateTime(const char* raw_date, const char* raw_time, char* dst, uint8_t max_len) {
    int hh = 0, mm = 0, ss = 0;
    int day = 0, month = 0, year = 0;

    if (raw_time && strlen(raw_time) >= 6) { sscanf(raw_time, "%2d%2d%2d", &hh, &mm, &ss); }

    if (raw_date && strlen(raw_date) >= 6) {
        sscanf(raw_date, "%2d%2d%2d", &day, &month, &year);
        year += 2000;
    } else {
        year = 2026;
        day = 1;
        month = 1;
    }

    snprintf(dst, max_len, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day, hh, mm, ss);
}


typedef bool (*cli_handler_t)(char* resp, size_t size);

static bool Cmd_Version(char* resp, size_t size) {
    snprintf(resp, size, "VER:1.0.0 | Build: " __DATE__ " " __TIME__ " | UART:9600 8N1 | USB:CDC Virtual COM\r\n");
    return true;
}

static bool Cmd_GetPos(char* resp, size_t size) {
    if (gps_last_valid_fix.fix_status != 1) {
        snprintf(resp, size, "LAT:0,LON:0,FIX:0,SATS:0,AGE_MS:N/A\r\n");
        return true;
    }

    char lat_str[20], lon_str[20];
    GPS_GetFormattedLatitude(gps_last_valid_fix.latitude, lat_str, sizeof(lat_str));
    GPS_GetFormattedLongitude(gps_last_valid_fix.longitude, lon_str, sizeof(lon_str));

    if (gps_last_valid_fix.timestamp_ms > 0) {
        uint32_t age_ms = HAL_GetTick() - gps_last_valid_fix.timestamp_ms;
        snprintf(resp, size, "LAT:%s,LON:%s,FIX:%u,SATS:%u,AGE_MS:%lu\r\n", lat_str, lon_str,
                 gps_last_valid_fix.fix_status, gps_last_valid_fix.satellites, age_ms);
    } else {
        snprintf(resp, size, "LAT:%s,LON:%s,FIX:%u,SATS:%u,AGE_MS:N/A\r\n", lat_str, lon_str,
                 gps_last_valid_fix.fix_status, gps_last_valid_fix.satellites);
    }
    return true;
}

static bool Cmd_GetTime(char* resp, size_t size) {
    char iso_str[32];
    Format_ISO_DateTime(gps_last_valid_fix.date_utc, gps_last_valid_fix.time_utc, iso_str, sizeof(iso_str));

    snprintf(resp, size, "%s, STALE:%u\r\n", iso_str, gps_last_valid_fix.is_stale);
    return true;
}

static bool Cmd_GetBme(char* resp, size_t size) {
    cli_get_bme_requested = true;
    return false;
}

static bool Cmd_GetColor(char* resp, size_t size) {
    cli_get_color_requested = true;
    return false;
}

static bool Cmd_RawOn(char* resp, size_t size) {
    gps_raw_enabled = 1;
    snprintf(resp, size, "RAW Output: ENABLED\r\n");
    return true;
}

static bool Cmd_RawOff(char* resp, size_t size) {
    gps_raw_enabled = 0;
    snprintf(resp, size, "RAW Output: DISABLED\r\n");
    return true;
}

static bool Cmd_FlashSave(char* resp, size_t size) {
    GPS_Flash_Force_Save();
    snprintf(resp, size, "Flash save triggered.\r\n");
    return true;
}

static bool Cmd_FlashInfo(char* resp, size_t size) {
    GPS_Flash_Report(resp, size);
    return true;
}

static bool Cmd_GetErrors(char* resp, size_t size) {
    snprintf(resp, size, "UART ERRORS: ORE:%lu,FE:%lu,NE:%lu,PE:%lu | RX_BYTES:%lu | DROPPED_LINES:%lu\r\n",
             (unsigned long) gps_stats.err_ore, (unsigned long) gps_stats.err_fe, (unsigned long) gps_stats.err_ne,
             (unsigned long) gps_stats.err_pe, (unsigned long) gps_stats.rx_bytes,
             (unsigned long) gps_stats.dropped_lines);
    return true;
}

static bool Cmd_TestColors(char* resp, size_t size) {
    cli_test_colors_requested = true;
    snprintf(resp, size, "Color test started\r\n");
    return true;
}

// Answered from the main loop (CLI_Process_Pending), where net and HTTP state are updated
static bool Cmd_Net(char* resp, size_t size) {
    (void) resp;
    (void) size;
    cli_get_net_requested = true;
    return false;
}

static bool Cmd_DispInfo(char* resp, size_t size) {
    uint32_t us, bytes;
    ILI9341_GetLastTileMetrics(&us, &bytes);
    snprintf(resp, size, "SCK:%luHz TILE_BYTES:%lu TILE_US:%lu\r\n", (unsigned long) ILI9341_GetSckHz(),
             (unsigned long) bytes, (unsigned long) us);
    return true;
}

static bool Cmd_Orient(char* resp, size_t size) {
    cli_orientation_toggle_requested = true;
    snprintf(resp, size, "Orientation toggle requested\r\n");
    return true;
}

static const struct {
    const char* name;
    cli_handler_t handler;
} cli_commands[] = {
        {"VER", Cmd_Version},
        {"GET POS", Cmd_GetPos},
        {"GET TIME", Cmd_GetTime},
        {"GET BME", Cmd_GetBme},
        {"GET ENV", Cmd_GetBme},
        {"GET TCS", Cmd_GetColor},
        {"GET COLOR", Cmd_GetColor},
        {"RAW ON", Cmd_RawOn},
        {"RAW OFF", Cmd_RawOff},
        {"FLASH SAVE", Cmd_FlashSave},
        {"FLASH INFO", Cmd_FlashInfo},
        {"GET ERRORS", Cmd_GetErrors},
        {"TEST COLORS", Cmd_TestColors},
        {"DISP INFO", Cmd_DispInfo},
        {"ORIENT", Cmd_Orient},
        {"NET", Cmd_Net},
};

void Process_CLI_Command(const char* cmd) {
    char clean_cmd[64];
    uint8_t j = 0;

    for (uint8_t i = 0; cmd[i] != '\0' && j < sizeof(clean_cmd) - 1; i++) {
        if (cmd[i] != '\r' && cmd[i] != '\n') { clean_cmd[j++] = (char) toupper((unsigned char) cmd[i]); }
    }
    clean_cmd[j] = '\0';

    char resp[256];

    for (size_t i = 0; i < sizeof(cli_commands) / sizeof(cli_commands[0]); i++) {
        if (strcmp(clean_cmd, cli_commands[i].name) == 0) {
            if (cli_commands[i].handler(resp, sizeof(resp))) { CLI_Send_Response(resp); }
            return;
        }
    }

    snprintf(resp, sizeof(resp),
             "ERROR: Unknown command '%s'. Available: VER, GET POS, GET TIME, GET BME / GET ENV, GET TCS / GET COLOR, "
             "RAW ON, RAW OFF, "
             "FLASH SAVE, FLASH INFO, GET ERRORS, TEST COLORS, DISP INFO, ORIENT, NET\r\n",
             clean_cmd);
    CLI_Send_Response(resp);
}

void CLI_Process_Pending(void) {
    char resp[256];

    if (cli_get_bme_requested) {
        cli_get_bme_requested = false;

        if (!bmeSensorInitialized) {
            snprintf(resp, sizeof(resp), "ERROR: BME280 sensor not detected\r\n");
        } else {
            uint32_t now = HAL_GetTick();
            uint32_t age_ms = (bme_data.timestamp_ms > 0) ? (now - bme_data.timestamp_ms) : 0;

            snprintf(resp, sizeof(resp), "TEMP:%.2f C, PRESS:%.2f hPa, HUM:%.2f %%, STALE:%u, AGE_MS:%lu\r\n",
                     bme_data.temperature, bme_data.pressure, bme_data.humidity, bme_data.is_stale ? 1 : 0,
                     (unsigned long) age_ms);
        }
        CLI_Send_Response(resp);
    }

    if (cli_get_color_requested) {
        cli_get_color_requested = false;

        if (!colorSensorInitialized) {
            snprintf(resp, sizeof(resp), "ERROR: TCS3472 sensor not detected\r\n");
        } else {
            uint32_t now = HAL_GetTick();
            uint32_t age_ms = (colorSensorData.lastTimestamp > 0) ? (now - colorSensorData.lastTimestamp) : 0;

            snprintf(resp, sizeof(resp), "CLEAR:%u, RED:%u, GREEN:%u, BLUE:%u, STALE:%u, AGE_MS:%lu\r\n",
                     colorSensorData.clear, colorSensorData.red, colorSensorData.green, colorSensorData.blue,
                     colorSensorData.isStale ? 1 : 0, (unsigned long) age_ms);
        }
        CLI_Send_Response(resp);
    }

    if (cli_get_net_requested) {
        cli_get_net_requested = false;

        // Only cached state: no SPI access to the W5500 here
        const net_info_t* n = NET_GetInfo();
        uint32_t now = HAL_GetTick();
        uint32_t net_up_s = (n->state == NET_ST_UP) ? (now - n->up_since_ms) / 1000u : 0;

        snprintf(resp, sizeof(resp),
                 "STATE:%s, MODE:%s, LINK:%s, IP:%u.%u.%u.%u, MASK:%u.%u.%u.%u, GW:%u.%u.%u.%u, "
                 "MAC:%02x:%02x:%02x:%02x:%02x:%02x, LEASE_S:%lu, NET_UP_S:%lu, HTTP_UP_S:%lu, HTTP_REQ:%lu\r\n",
                 NET_StateName(n->state), NET_ModeName(n->mode), n->link_up ? "UP" : "DOWN", n->ip[0], n->ip[1],
                 n->ip[2], n->ip[3], n->mask[0], n->mask[1], n->mask[2], n->mask[3], n->gw[0], n->gw[1], n->gw[2],
                 n->gw[3], n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5], (unsigned long) n->lease_s,
                 (unsigned long) net_up_s, (unsigned long) HTTP_GetUptimeS(now),
                 (unsigned long) HTTP_GetRequestCount());
        CLI_Send_Response(resp);
    }
}
