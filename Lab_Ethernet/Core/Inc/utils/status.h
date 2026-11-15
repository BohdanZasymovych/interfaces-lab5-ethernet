#ifndef STATUS_H
#define STATUS_H

/*
 * Device status: the "single source of truth" for the network (manual, step 1).
 *
 * Status_Update() copies the latest GPS and sensor values into one snapshot
 * in the main loop. The HTTP server only formats this snapshot and never reads
 * I2C or UART itself, so a request can't stall the sensors and /status stays fast.
 *
 * Sources:
 *   gps_last_valid_fix  (gps_parser.h)  lat, lon, fix, sats, time, timestamp_ms
 *   bme_data            (bme280.h)      temperature, pressure, humidity, timestamp_ms
 *   colorSensorData     (tcs3472.h)     clear channel -> "lux", lastTimestamp
 *
 * JSON produced by Status_FormatJson() (one line, shown wrapped here):
 *
 *   {"proto_ver":1,
 *    "device_id":"bp-401-1a2b",
 *    "time_utc":"2025-08-23T12:34:56Z",
 *    "gps":{"lat":49.8397,"lon":24.0297,"fix":3,"sats":10},
 *    "env":{"t_c":24.3,"p_hpa":1007.8,"rh_pct":47.2,"lux":356},
 *    "stale_age_s":{"gps":0.3,"env":0.7},
 *    "stale":{"gps":false,"env":false}}
 *
 *   - proto_ver:   STATUS_PROTO_VER. Increase it when the schema changes.
 *   - stale_age_s: seconds since the last valid reading of each block, or
 *                  null if that block never delivered data.
 *   - Values that never arrived are written as null, not as 0.
 *   - Optional "net" object (mode, ip, mask, gw, mac) from NET_GetInfo().
 *   - The TCS3472 measures no lux. Either compute an estimate from C/R/G/B
 *     or document what "lux" contains in the README.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATUS_PROTO_VER     1u     // Schema version sent as "proto_ver"
#define STATUS_STALE_MS      3000u  // A block older than this is marked stale (same as the display)
#define STATUS_JSON_MAX_LEN  512u   // Buffer size the HTTP server should use for the /status body
#define STATUS_DEVICE_ID_LEN 16u
#define STATUS_TIME_UTC_LEN  21u  // "YYYY-MM-DDThh:mm:ssZ" + '\0'

typedef struct {
    bool valid;   // false until the first valid fix
    double lat;   // degrees, negative = S
    double lon;   // degrees, negative = W
    uint8_t fix;  // 0 = none, 2 = 2D, 3 = 3D
    uint8_t sats;
    uint32_t timestamp_ms;  // HAL_GetTick() of the last valid fix, 0 = never
} status_gps_t;

typedef struct {
    bool valid;  // false until the first successful BME280 read
    float t_c;
    float p_hpa;
    float rh_pct;
    bool lum_valid;  // false until the first successful TCS3472 read
    float lum;
    uint32_t timestamp_ms;  // HAL_GetTick() of the oldest of the two sensors' last reads, 0 = never
} status_env_t;

typedef struct {
    char device_id[STATUS_DEVICE_ID_LEN];  // e.g. "bp-401-1a2b", from the same UID-derived id as the display
    char time_utc[STATUS_TIME_UTC_LEN];    // ISO 8601 from GPS date + time, "" if unknown
    status_gps_t gps;
    status_env_t env;
    uint32_t snapshot_ms;  // HAL_GetTick() of the last Status_Update()
} device_status_t;

/**
 * Initialise the snapshot. Call once at start-up.
 *
 * @param node_id  16-bit id already shown on the display (main.c), used to
 *                 build device_id.
 */
void Status_Init(uint16_t node_id);

/**
 * Copy the latest values from the GPS and sensor modules into the snapshot.
 * Call from the main loop after the sensor reads (the 1 s block) and after
 * GPS sentences are parsed. Only copies, never touches I2C or UART.
 *
 * @param now  HAL_GetTick() of the current main-loop pass.
 */
void Status_Update(uint32_t now);

/**
 * @return the current snapshot, for modules that want the values directly.
 */
const device_status_t* Status_Get(void);

/**
 * Format the snapshot as the /status JSON body described above.
 * stale_age_s is computed from `now`, so it is fresh even between updates.
 *
 * @param buf  Output buffer, at least STATUS_JSON_MAX_LEN bytes recommended.
 * @param len  Size of buf.
 * @param now  HAL_GetTick() at the time of the request.
 * @return length of the JSON without the terminating '\0', or 0 if it did not fit.
 */
size_t Status_FormatJson(char* buf, size_t len, uint32_t now);

#endif
