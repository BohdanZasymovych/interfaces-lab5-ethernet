#include "utils/ui.h"
#include "utils/ili9341.h"
#include "utils/gps_parser.h"
#include "utils/bme280.h"
#include "utils/tcs3472.h"
#include "utils/net.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define UI_VALUE_X     40
#define UI_VALUE_CHARS 33
#define UI_REFRESH_MS  1000

typedef struct {
    const uint16_t y;
    uint16_t last_color;
    char last[UI_VALUE_CHARS + 1];  // padded text currently on screen
} ui_row_t;

static ui_row_t row_lat = {.y = 60};
static ui_row_t row_lon = {.y = 80};
static ui_row_t row_utc = {.y = 100};
static ui_row_t row_sat = {.y = 120};
static ui_row_t row_sta = {.y = 140};
static ui_row_t row_env = {.y = 160};
static ui_row_t row_clr = {.y = 180};
static ui_row_t row_lum = {.y = 200};
static ui_row_t row_net = {.y = 220};  // rows from here on are below the screen in landscape (240 px high)
static ui_row_t row_ip = {.y = 240};
static ui_row_t row_gw = {.y = 260};

static ui_row_t* const ui_rows[] = {&row_lat, &row_lon, &row_utc, &row_sat, &row_sta, &row_env,
                                    &row_clr, &row_lum, &row_net, &row_ip,  &row_gw};

static uint32_t last_ui_update = 0;

// Redraws the row only if its text or colour changed since the last draw.
static void UI_UpdateRow(ui_row_t* row, const char* text, uint16_t color) {
    char padded[UI_VALUE_CHARS + 1];
    snprintf(padded, sizeof(padded), "%-*s", UI_VALUE_CHARS, text);

    if (row->last[0] != '\0' && color == row->last_color && strcmp(padded, row->last) == 0) { return; }
    ILI9341_DrawText(UI_VALUE_X, row->y, padded, color, COLOR_BLACK);
    memcpy(row->last, padded, sizeof(row->last));
    row->last_color = color;
}

static void UI_FormatStaleAge(char* dst, size_t size, bool is_stale, uint32_t timestamp_ms, uint32_t now) {
    if (!is_stale) {
        dst[0] = '\0';
    } else if (timestamp_ms == 0) {
        snprintf(dst, size, " !--");
    } else {
        uint32_t age_s = (now - timestamp_ms) / 1000;
        snprintf(dst, size, " !%lus", (unsigned long) (age_s > 99 ? 99 : age_s));
    }
}

static void UI_InvalidateAll(void) {
    for (size_t i = 0; i < sizeof(ui_rows) / sizeof(ui_rows[0]); i++) { ui_rows[i]->last[0] = '\0'; }
}

static void UI_DrawStatic(uint16_t node_id) {
    char header_buf[32];
    snprintf(header_buf, sizeof(header_buf), "ID: 0x%04X", node_id);

    ILI9341_DrawText(10, 10, "GPS TRACKER NODE", COLOR_CYAN, COLOR_BLACK);
    ILI9341_DrawText(10, 30, header_buf, COLOR_CYAN, COLOR_BLACK);

    ILI9341_DrawText(10, 60, "LAT: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 80, "LON: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 100, "UTC: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 120, "SAT: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 140, "STA: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 160, "ENV: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 180, "CLR: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 200, "LUM: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 220, "NET: ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 240, "IP:  ", COLOR_WHITE, COLOR_BLACK);
    ILI9341_DrawText(10, 260, "GW:  ", COLOR_WHITE, COLOR_BLACK);
}

static void UI_FormatIp(char* dst, size_t size, const uint8_t ip[4]) {
    snprintf(dst, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// Subnet mask -> prefix length, e.g. 255.255.255.0 -> 24
static uint8_t UI_MaskPrefix(const uint8_t mask[4]) {
    uint8_t bits = 0;
    for (int i = 0; i < 4; i++) {
        for (uint8_t m = mask[i]; m & 0x80u; m = (uint8_t) (m << 1)) bits++;
    }
    return bits;
}

// NET: state/mode (coloured), IP: address/prefix, GW: gateway. Only reads NET_GetInfo().
static void UI_UpdateNetRows(char* buf, size_t size) {
    const net_info_t* net = NET_GetInfo();

    switch (net->state) {
        case NET_ST_NO_CHIP: UI_UpdateRow(&row_net, "W5500 NOT FOUND", COLOR_RED); break;
        case NET_ST_NO_LINK: UI_UpdateRow(&row_net, "NO LINK", COLOR_RED); break;
        case NET_ST_DHCP: UI_UpdateRow(&row_net, "DHCP...", COLOR_CYAN); break;
        case NET_ST_UP:
            snprintf(buf, size, "%s  LINK UP", NET_ModeName(net->mode));
            UI_UpdateRow(&row_net, buf, net->mode == NET_MODE_DHCP ? COLOR_GREEN : COLOR_YELLOW);
            break;
        default: UI_UpdateRow(&row_net, "?", COLOR_RED); break;
    }

    if (net->state != NET_ST_UP) {
        UI_UpdateRow(&row_ip, "--", COLOR_WHITE);
        UI_UpdateRow(&row_gw, "--", COLOR_WHITE);
        return;
    }

    char ip[16];
    UI_FormatIp(ip, sizeof(ip), net->ip);
    snprintf(buf, size, "%s/%u", ip, UI_MaskPrefix(net->mask));
    UI_UpdateRow(&row_ip, buf, COLOR_WHITE);

    UI_FormatIp(buf, size, net->gw);
    UI_UpdateRow(&row_gw, buf, COLOR_WHITE);
}

void UI_Redraw(uint16_t node_id) {
    ILI9341_FillColor(COLOR_BLACK);
    UI_DrawStatic(node_id);
    UI_InvalidateAll();
    last_ui_update = 0;  // refresh the data rows on the next UI_Update()
}

void UI_Update(uint32_t now) {
    if (now - last_ui_update < UI_REFRESH_MS) { return; }
    last_ui_update = now;

    char buf[UI_VALUE_CHARS + 1];

    GPS_GetFormattedLatitude(gps_last_valid_fix.latitude, buf, sizeof(buf));
    UI_UpdateRow(&row_lat, buf, COLOR_YELLOW);

    GPS_GetFormattedLongitude(gps_last_valid_fix.longitude, buf, sizeof(buf));
    UI_UpdateRow(&row_lon, buf, COLOR_YELLOW);

    GPS_GetFormattedTime(gps_last_valid_fix.time_utc, buf, sizeof(buf));
    UI_UpdateRow(&row_utc, buf, COLOR_CYAN);

    snprintf(buf, sizeof(buf), "%02d", gps_last_valid_fix.satellites);
    UI_UpdateRow(&row_sat, buf, COLOR_WHITE);

    if (gps_last_valid_fix.is_stale) {
        UI_UpdateRow(&row_sta, "STALE/LOST", COLOR_RED);
    } else if (gps_last_valid_fix.timestamp_ms == 0) {
        UI_UpdateRow(&row_sta, "WAITING FIX", COLOR_RED);
    } else {
        UI_UpdateRow(&row_sta, "ACTIVE FIX", COLOR_GREEN);
    }

    char age[6];  // " !99s" plus terminator

    if (!bmeSensorInitialized) {
        UI_UpdateRow(&row_env, "BME280 INIT FAILED", COLOR_RED);
    } else {
        UI_FormatStaleAge(age, sizeof(age), bme_data.is_stale, bme_data.timestamp_ms, now);
        snprintf(buf, sizeof(buf), "%.1fC %.1fhPa %.1f%%%s", bme_data.temperature, bme_data.pressure, bme_data.humidity,
                 age);
        UI_UpdateRow(&row_env, buf, bme_data.is_stale ? COLOR_RED : COLOR_WHITE);
    }

    if (!colorSensorInitialized) {
        UI_UpdateRow(&row_clr, "TCS3472 INIT FAILED", COLOR_RED);
        UI_UpdateRow(&row_lum, "TCS3472 INIT FAILED", COLOR_RED);
    } else {
        UI_FormatStaleAge(age, sizeof(age), colorSensorData.isStale, colorSensorData.lastTimestamp, now);
        snprintf(buf, sizeof(buf), "C%u R%u G%u B%u%s", colorSensorData.clear, colorSensorData.red,
                 colorSensorData.green, colorSensorData.blue, age);
        UI_UpdateRow(&row_clr, buf, colorSensorData.isStale ? COLOR_RED : COLOR_WHITE);

        snprintf(buf, sizeof(buf), "%.2f%s", log10f((float) colorSensorData.clear + 1.0f), age);
        UI_UpdateRow(&row_lum, buf, colorSensorData.isStale ? COLOR_RED : COLOR_WHITE);
    }

    UI_UpdateNetRows(buf, sizeof(buf));
}
