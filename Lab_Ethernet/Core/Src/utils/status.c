#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "utils/status.h"
#include "utils/gps_parser.h"
#include "utils/bme280.h"
#include "utils/tcs3472.h"
#include "utils/net.h"

static device_status_t device_status;

/*
 * Private helpers to add while implementing (static, in this file):
 *
 *   static void  status_format_time(char* dst, size_t len);   GPS date + time -> "YYYY-MM-DDThh:mm:ssZ"
 *   static float status_estimate_lux(void);                   TCS3472 C/R/G/B -> lux (or document the raw value)
 *   static int   status_write_age(char* dst, size_t len, uint32_t ts_ms, uint32_t now);
 *                                                             "0.3" or "null" when ts_ms == 0
 */

static bool status_all_digits(const char* str, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (str[i] < '0' || str[i] > '9') return false;  // also stops at '\0' of a short string
    }
    return true;
}

/*
 * GPS date "ddmmyy" + time "hhmmss[.sss]" (NMEA RMC fields, from gps_current_fix)
 * -> ISO 8601 "20yy-mm-ddThh:mm:ssZ". Fractional seconds are dropped.
 * Writes "" if either field is missing or not numeric; the JSON then has "time_utc":null.
 * gps_current_fix is used rather than gps_last_valid_fix: the receiver often knows
 * UTC before it has a position fix.
 */
static void status_format_time(char* dst, size_t len) {
    const char* d = gps_current_fix.date_utc;
    const char* t = gps_current_fix.time_utc;

    if (len == 0) return;
    dst[0] = '\0';
    if (!status_all_digits(d, 6) || !status_all_digits(t, 6)) return;

    snprintf(dst, len, "20%.2s-%.2s-%.2sT%.2s:%.2s:%.2sZ", d + 4, d + 2, d, t, t + 2, t + 4);
}

static float status_estimate_lum_index(void) {
    const float lum_index = log10f((float) colorSensorData.clear + 1.0f);
    return lum_index;
}

void Status_Init(uint16_t node_id) {
    memset(&device_status, 0, sizeof(device_status));
    snprintf(device_status.device_id, sizeof(device_status.device_id), "bp-401-%04x", node_id);
}

void Status_Update(uint32_t now) {
    // TODO: copy gps_last_valid_fix, bme_data and colorSensorData into device_status, set snapshot_ms
    device_status_t* s = &device_status;

    // GPS
    const GPS_FixData_t* g = &gps_last_valid_fix;
    s->gps.valid = (g->timestamp_ms != 0) || (g->latitude != 0.0 || g->longitude != 0.0);  // live or from flash
    s->gps.lat = g->latitude;
    s->gps.lon = g->longitude;
    s->gps.fix = g->fix_status;
    s->gps.sats = g->satellites;
    s->gps.timestamp_ms = g->timestamp_ms;
    status_format_time(s->time_utc, sizeof(s->time_utc));

    // Environment
    s->env.valid = (bme_data.timestamp_ms != 0);
    s->env.t_c = bme_data.temperature;
    s->env.p_hpa = bme_data.pressure;
    s->env.rh_pct = bme_data.humidity;
    s->env.lum_valid = (colorSensorData.lastTimestamp != 0);
    s->env.lum = status_estimate_lum_index();

    // Oldest of the sensors that have delivered data; 0 if none has
    uint32_t ts = 0;
    if (s->env.valid) ts = bme_data.timestamp_ms;
    if (s->env.lum_valid && (ts == 0 || colorSensorData.lastTimestamp < ts)) ts = colorSensorData.lastTimestamp;
    s->env.timestamp_ms = ts;

    s->snapshot_ms = now;
}

const device_status_t* Status_Get(void) {
    return &device_status;
}

/* ---------- JSON writer ----------
 * Appends into a fixed buffer and remembers if anything did not fit, so the
 * format code can be written top to bottom without checking every call.
 * Decimals are printed with integer math only: newlib-nano prints nothing for
 * %f unless the project links with -u _printf_float.
 */
typedef struct {
    char* buf;
    size_t len;
    size_t pos;
    bool overflow;
} json_writer_t;

static void jw_printf(json_writer_t* w, const char* fmt, ...) {
    if (w->overflow) return;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(w->buf + w->pos, w->len - w->pos, fmt, args);
    va_end(args);

    if (n < 0 || (size_t) n >= w->len - w->pos) {
        w->overflow = true;
        return;
    }
    w->pos += (size_t) n;
}

// Fixed-point number with `decimals` digits after the point (0..6), or null if invalid
static void jw_fixed(json_writer_t* w, bool valid, double value, uint8_t decimals) {
    static const uint32_t pow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000};

    if (!valid || !isfinite(value) || decimals > 6 || fabs(value) * pow10[decimals] > 4.0e9) {
        jw_printf(w, "null");
        return;
    }

    uint32_t scaled = (uint32_t) (fabs(value) * pow10[decimals] + 0.5);
    uint32_t int_part = scaled / pow10[decimals];
    uint32_t frac_part = scaled % pow10[decimals];
    const char* sign = (value < 0 && scaled != 0) ? "-" : "";

    if (decimals == 0) {
        jw_printf(w, "%s%lu", sign, (unsigned long) int_part);
    } else {
        jw_printf(w, "%s%lu.%0*lu", sign, (unsigned long) int_part, (int) decimals, (unsigned long) frac_part);
    }
}

// Age in seconds with one decimal, or null if the block never delivered data
static void jw_age(json_writer_t* w, uint32_t ts_ms, uint32_t now) {
    if (ts_ms == 0) {
        jw_printf(w, "null");
        return;
    }
    uint32_t tenths = (now - ts_ms + 50u) / 100u;
    jw_printf(w, "%lu.%lu", (unsigned long) (tenths / 10u), (unsigned long) (tenths % 10u));
}

static void jw_ip(json_writer_t* w, const uint8_t ip[4]) {
    jw_printf(w, "\"%u.%u.%u.%u\"", ip[0], ip[1], ip[2], ip[3]);
}

static bool status_is_stale(uint32_t ts_ms, uint32_t now) {
    return ts_ms == 0 || (now - ts_ms) > STATUS_STALE_MS;
}

size_t Status_FormatJson(char* buf, size_t len, uint32_t now) {
    if (buf == NULL || len == 0) return 0;

    const device_status_t* s = &device_status;
    const net_info_t* net = NET_GetInfo();
    json_writer_t w = {.buf = buf, .len = len, .pos = 0, .overflow = false};

    jw_printf(&w, "{\"proto_ver\":%u,\"device_id\":\"%s\",", STATUS_PROTO_VER, s->device_id);

    if (s->time_utc[0] != '\0') {
        jw_printf(&w, "\"time_utc\":\"%s\",", s->time_utc);
    } else {
        jw_printf(&w, "\"time_utc\":null,");
    }

    jw_printf(&w, "\"gps\":{\"lat\":");
    jw_fixed(&w, s->gps.valid, s->gps.lat, 6);
    jw_printf(&w, ",\"lon\":");
    jw_fixed(&w, s->gps.valid, s->gps.lon, 6);
    jw_printf(&w, ",\"fix\":%u,\"sats\":%u},", s->gps.fix, s->gps.sats);

    jw_printf(&w, "\"env\":{\"t_c\":");
    jw_fixed(&w, s->env.valid, s->env.t_c, 1);
    jw_printf(&w, ",\"p_hpa\":");
    jw_fixed(&w, s->env.valid, s->env.p_hpa, 1);
    jw_printf(&w, ",\"rh_pct\":");
    jw_fixed(&w, s->env.valid, s->env.rh_pct, 1);
    jw_printf(&w, ",\"lum\":");
    jw_fixed(&w, s->env.lum_valid, s->env.lum, 2);
    jw_printf(&w, "},");

    jw_printf(&w, "\"stale_age_s\":{\"gps\":");
    jw_age(&w, s->gps.timestamp_ms, now);
    jw_printf(&w, ",\"env\":");
    jw_age(&w, s->env.timestamp_ms, now);
    jw_printf(&w, "},");

    jw_printf(&w, "\"stale\":{\"gps\":%s,\"env\":%s},", status_is_stale(s->gps.timestamp_ms, now) ? "true" : "false",
              status_is_stale(s->env.timestamp_ms, now) ? "true" : "false");

    jw_printf(&w, "\"net\":{\"mode\":\"%s\",\"ip\":", NET_ModeName(net->mode));
    jw_ip(&w, net->ip);
    jw_printf(&w, ",\"mask\":");
    jw_ip(&w, net->mask);
    jw_printf(&w, ",\"gw\":");
    jw_ip(&w, net->gw);
    jw_printf(&w, ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}}", net->mac[0], net->mac[1], net->mac[2], net->mac[3],
              net->mac[4], net->mac[5]);

    if (w.overflow) {
        buf[0] = '\0';
        return 0;
    }
    return w.pos;
}

/* ---------- Compact CBOR writer ---------- */
typedef struct {
    uint8_t* buf;
    size_t len;
    size_t pos;
    bool overflow;
} cbor_writer_t;

static void cbor_put_byte(cbor_writer_t* w, uint8_t value) {
    if (w->pos >= w->len) {
        w->overflow = true;
        return;
    }
    w->buf[w->pos++] = value;
}

static void cbor_put_bytes(cbor_writer_t* w, const uint8_t* data, size_t len) {
    if (w->overflow || len > w->len - w->pos) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
}

static void cbor_put_uint(cbor_writer_t* w, uint32_t value) {
    if (value < 24u) {
        cbor_put_byte(w, (uint8_t) value);
    } else if (value <= 0xFFu) {
        cbor_put_byte(w, 0x18u);
        cbor_put_byte(w, (uint8_t) value);
    } else {
        cbor_put_byte(w, 0x1Au);
        cbor_put_byte(w, (uint8_t) (value >> 24));
        cbor_put_byte(w, (uint8_t) (value >> 16));
        cbor_put_byte(w, (uint8_t) (value >> 8));
        cbor_put_byte(w, (uint8_t) value);
    }
}

static void cbor_put_map(cbor_writer_t* w, uint8_t count) {
    cbor_put_byte(w, (uint8_t) (0xA0u | count));
}

static void cbor_put_text(cbor_writer_t* w, const char* text) {
    size_t len = strlen(text);
    if (len >= 24u) {
        cbor_put_byte(w, 0x78u);
        cbor_put_byte(w, (uint8_t) len);
    } else {
        cbor_put_byte(w, (uint8_t) (0x60u | len));
    }
    cbor_put_bytes(w, (const uint8_t*) text, len);
}

static void cbor_put_key(cbor_writer_t* w, const char* key) {
    cbor_put_text(w, key);
}

static void cbor_put_null(cbor_writer_t* w) {
    cbor_put_byte(w, 0xF6u);
}

static void cbor_put_bool(cbor_writer_t* w, bool value) {
    cbor_put_byte(w, value ? 0xF5u : 0xF4u);
}

static void cbor_put_float(cbor_writer_t* w, bool valid, float value) {
    uint32_t bits;
    if (!valid || !isfinite(value)) {
        cbor_put_null(w);
        return;
    }
    memcpy(&bits, &value, sizeof(bits));
    cbor_put_byte(w, 0xFAu);  // IEEE-754 binary32, network byte order
    cbor_put_byte(w, (uint8_t) (bits >> 24));
    cbor_put_byte(w, (uint8_t) (bits >> 16));
    cbor_put_byte(w, (uint8_t) (bits >> 8));
    cbor_put_byte(w, (uint8_t) bits);
}

static void cbor_put_ip(cbor_writer_t* w, const uint8_t ip[4]) {
    char text[16];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    cbor_put_text(w, text);
}

static void cbor_put_mac(cbor_writer_t* w, const uint8_t mac[6]) {
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cbor_put_text(w, text);
}

size_t Status_FormatCbor(uint8_t* buf, size_t len, uint32_t now) {
    if (buf == NULL || len == 0) return 0;

    const device_status_t* s = &device_status;
    const net_info_t* net = NET_GetInfo();
    cbor_writer_t w = {.buf = buf, .len = len, .pos = 0, .overflow = false};

    cbor_put_map(&w, 8);
    cbor_put_key(&w, "proto_ver");
    cbor_put_uint(&w, STATUS_PROTO_VER);
    cbor_put_key(&w, "device_id");
    cbor_put_text(&w, s->device_id);
    cbor_put_key(&w, "time_utc");
    if (s->time_utc[0] != '\0')
        cbor_put_text(&w, s->time_utc);
    else
        cbor_put_null(&w);

    cbor_put_key(&w, "gps");
    cbor_put_map(&w, 4);
    cbor_put_key(&w, "lat");
    cbor_put_float(&w, s->gps.valid, (float) s->gps.lat);
    cbor_put_key(&w, "lon");
    cbor_put_float(&w, s->gps.valid, (float) s->gps.lon);
    cbor_put_key(&w, "fix");
    cbor_put_uint(&w, s->gps.fix);
    cbor_put_key(&w, "sats");
    cbor_put_uint(&w, s->gps.sats);

    cbor_put_key(&w, "env");
    cbor_put_map(&w, 4);
    cbor_put_key(&w, "t_c");
    cbor_put_float(&w, s->env.valid, s->env.t_c);
    cbor_put_key(&w, "p_hpa");
    cbor_put_float(&w, s->env.valid, s->env.p_hpa);
    cbor_put_key(&w, "rh_pct");
    cbor_put_float(&w, s->env.valid, s->env.rh_pct);
    cbor_put_key(&w, "lum");
    cbor_put_float(&w, s->env.lum_valid, s->env.lum);

    cbor_put_key(&w, "stale_age_s");
    cbor_put_map(&w, 2);
    cbor_put_key(&w, "gps");
    if (s->gps.timestamp_ms == 0)
        cbor_put_null(&w);
    else
        cbor_put_float(&w, true, (float) (now - s->gps.timestamp_ms) / 1000.0f);
    cbor_put_key(&w, "env");
    if (s->env.timestamp_ms == 0)
        cbor_put_null(&w);
    else
        cbor_put_float(&w, true, (float) (now - s->env.timestamp_ms) / 1000.0f);

    cbor_put_key(&w, "stale");
    cbor_put_map(&w, 2);
    cbor_put_key(&w, "gps");
    cbor_put_bool(&w, status_is_stale(s->gps.timestamp_ms, now));
    cbor_put_key(&w, "env");
    cbor_put_bool(&w, status_is_stale(s->env.timestamp_ms, now));

    cbor_put_key(&w, "net");
    cbor_put_map(&w, 5);
    cbor_put_key(&w, "mode");
    cbor_put_text(&w, NET_ModeName(net->mode));
    cbor_put_key(&w, "ip");
    cbor_put_ip(&w, net->ip);
    cbor_put_key(&w, "mask");
    cbor_put_ip(&w, net->mask);
    cbor_put_key(&w, "gw");
    cbor_put_ip(&w, net->gw);
    cbor_put_key(&w, "mac");
    cbor_put_mac(&w, net->mac);

    if (w.overflow) {
        buf[0] = 0;
        return 0;
    }
    return w.pos;
}
