// =============================================================
//  CarDash Monitor — main.c
//  ESP32 + SIM800L crash detection & web dashboard
//
//  Wiring:
//    UART1 TX  -> GPIO 17  (to SIM800L RX)
//    UART1 RX  -> GPIO 16  (from SIM800L TX)
//    PWRKEY    -> GPIO 4   (SIM800L PWRKEY)
//
//  WiFi AP:  SSID = CAR_DASH  |  PASS = 12345678
//  Dashboard: http://192.168.4.1/
//  GPS JSON:  http://192.168.4.1/gps
//  Crash API: http://192.168.4.1/crash
// =============================================================

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "esp_http_server.h"

// ─── CONFIG ──────────────────────────────────────────────────
#define UART_NUM        UART_NUM_1
#define TX_PIN          17
#define RX_PIN          16
#define BUF_SIZE        1024
#define PWRKEY_PIN      4

#define WIFI_SSID       "CAR_DASH"
#define WIFI_PASS       "12345678"
#define MAX_CONN        4

#define SMS_NUMBER      "+601121457455"

// Crash auto-clear after this many seconds (0 = never auto-clear)
#define CRASH_CLEAR_SEC 5

static const char *TAG = "CARDASH";

// ─── GLOBAL STATE ─────────────────────────────────────────────
static float  g_speed = 0.0f;
static float  g_lat   = 3.0f;
static float  g_lon   = 101.0f;
static int    g_crash = 0;
// ─── GPS DATA FLAG ──────────────────────────────────────────
static volatile int g_has_real_gps = 0;
// Crash auto-clear timestamp (esp_timer ticks, microseconds)
static int64_t g_crash_time = 0;

// ─── MOVEMENT STATE ──────────────────────────────────────────
static int    g_moving = 0; // 0 = static, 1 = moving
static float  g_last_lat = 0.0f;
static float  g_last_lon = 0.0f;
static int    g_static_count = 0;

// Call this after updating g_lat/g_lon to update movement state
static void update_movement_state(void) {
    float dlat = g_lat - g_last_lat;
    float dlon = g_lon - g_last_lon;
    float dist = dlat * dlat + dlon * dlon;
    // If moved more than ~0.00001 deg (~1m), consider moving
    if (dist > 0.0000001f) {
        g_moving = 1;
        g_static_count = 0;
        g_last_lat = g_lat;
        g_last_lon = g_lon;
    } else {
        g_static_count++;
        // If static for 3+ checks, set to static
        if (g_static_count >= 3) g_moving = 0;
    }
}

// ─── FORWARD DECLARATIONS ─────────────────────────────────────
static void trigger_crash(void);

// ─── UART ─────────────────────────────────────────────────────
static void uart_init(void)
{
    uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TX_PIN, RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1 ready  TX=GPIO%d  RX=GPIO%d  BAUD=115200", TX_PIN, RX_PIN);
}

// ─── MODEM POWER ──────────────────────────────────────────────
static void pwrkey_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PWRKEY_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PWRKEY_PIN, 1);
    ESP_LOGI(TAG, "PWRKEY GPIO%d configured", PWRKEY_PIN);
}

static void modem_power_on(void)
{
    ESP_LOGI(TAG, "Toggling SIM800L PWRKEY...");
    gpio_set_level(PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level(PWRKEY_PIN, 1);

    ESP_LOGI(TAG, "Waiting for modem to boot (15 s)...");
    vTaskDelay(pdMS_TO_TICKS(15000));
    ESP_LOGI(TAG, "Modem should be ready");
}

// ─── AT HELPER ────────────────────────────────────────────────
static void at_send(const char *cmd, uint32_t delay_ms)
{
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    if (delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ─── SMS ──────────────────────────────────────────────────────
static void send_sms(void)
{
    char msg[160];
    snprintf(msg, sizeof(msg),
             "CRASH ALERT!\nSpeed: %.1f km/h\nLat: %.6f\nLon: %.6f",
             g_speed, g_lat, g_lon);

    ESP_LOGI(TAG, "Sending SMS to %s ...", SMS_NUMBER);

    // Set text mode
    at_send("AT+CMGF=1\r", 1000);

    // Set recipient — format: AT+CMGS="<number>"\r
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r", SMS_NUMBER);
    at_send(cmd, 2000);

    // Message body
    uart_write_bytes(UART_NUM, msg, strlen(msg));

    // Ctrl-Z to send
    const char ctrlz = 0x1A;
    uart_write_bytes(UART_NUM, &ctrlz, 1);

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "SMS sent: %s", msg);
}

// ─── CRASH ────────────────────────────────────────────────────
static void trigger_crash(void)
{
    if (g_crash) {
        return;   // already in crash state
    }
    g_crash      = 1;
    g_crash_time = esp_timer_get_time();

    ESP_LOGE(TAG, "*** CRASH DETECTED at %.1f km/h ***", g_speed);
    send_sms();
}

// ─── SIMULATION TASK ─────────────────────────────────────────
// Simulates speed, GPS drift, and random crash events.
// Replace with real GPS/accelerometer reads in production.
static void sim_task(void *arg)
{
    ESP_LOGI(TAG, "Simulation task started");

    while (1) {
        // Only simulate if no real GPS data
        if (!g_has_real_gps) {
            // --- Speed: 20–120 km/h random walk ---
            g_speed = 20.0f + (float)(rand() % 101);

            // --- GPS: small random walk around Klang Valley ---
            g_lat += ((float)((rand() % 2001) - 1000)) / 100000.0f;
            g_lon += ((float)((rand() % 2001) - 1000)) / 100000.0f;
            if (g_lat >  90.0f) g_lat =  90.0f;
            if (g_lat < -90.0f) g_lat = -90.0f;
            if (g_lon >  180.0f) g_lon =  180.0f;
            if (g_lon < -180.0f) g_lon = -180.0f;

            // --- Crash: 1% chance per second ---
            if ((rand() % 100) == 0) {
                trigger_crash();
            }

            // --- Auto-clear crash after CRASH_CLEAR_SEC seconds ---
#if CRASH_CLEAR_SEC > 0
            if (g_crash) {
                int64_t elapsed_us = esp_timer_get_time() - g_crash_time;
                if (elapsed_us > (int64_t)CRASH_CLEAR_SEC * 1000000LL) {
                    g_crash = 0;
                    ESP_LOGI(TAG, "Crash state cleared");
                }
            }
#endif

            update_movement_state();

            ESP_LOGI(TAG, "SIM  speed=%.1f  lat=%.6f  lon=%.6f  crash=%d",
                     g_speed, g_lat, g_lon, g_crash);

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// ─── EMBEDDED HTML ────────────────────────────────────────────
// monitor.html is baked into the firmware by EMBED_TXTFILES in main/CMakeLists.txt.
// The linker auto-generates these symbols from the filename:
//   monitor.html  ->  _binary_monitor_html_start / _binary_monitor_html_end
extern const uint8_t monitor_html_start[] asm("_binary_monitor_html_start");
extern const uint8_t monitor_html_end[]   asm("_binary_monitor_html_end");

// ─── HTTP HANDLERS ────────────────────────────────────────────

// GET /  — serves the dashboard HTML
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)monitor_html_start,
                    monitor_html_end - monitor_html_start);
    return ESP_OK;
}

// GET /gps — returns live telemetry as JSON
// Response: {"speed":75.00,"lat":3.123456,"lng":101.654321,"crash":0}
static esp_err_t gps_handler(httpd_req_t *req)
{
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"speed\":%.2f,\"lat\":%.6f,\"lng\":%.6f,\"crash\":%d,\"moving\":%d}",
             g_speed, g_lat, g_lon, g_crash, g_moving);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /crash — manually triggers a crash event (for demo/testing)
static esp_err_t crash_handler(httpd_req_t *req)
{
    trigger_crash();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"crash_triggered\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ─── HTTP SERVER ──────────────────────────────────────────────
static httpd_handle_t server = NULL;

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size        = 8192;

    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t uri_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_uri_t uri_gps = {
        .uri     = "/gps",
        .method  = HTTP_GET,
        .handler = gps_handler,
    };
    httpd_uri_t uri_crash = {
        .uri     = "/crash",
        .method  = HTTP_GET,
        .handler = crash_handler,
    };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_gps);
    httpd_register_uri_handler(server, &uri_crash);

    ESP_LOGI(TAG, "HTTP server started");
    ESP_LOGI(TAG, "  GET /       -> Dashboard UI");
    ESP_LOGI(TAG, "  GET /gps    -> JSON telemetry");
    ESP_LOGI(TAG, "  GET /crash  -> Trigger crash");
}

// ─── WIFI AP ──────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Client connected  AID=%d  MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                 e->aid,
                 e->mac[0], e->mac[1], e->mac[2],
                 e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "Client disconnected  AID=%d", e->aid);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = WIFI_SSID,
            .ssid_len        = strlen(WIFI_SSID),
            .password        = WIFI_PASS,
            .max_connection  = MAX_CONN,
            .authmode        = WIFI_AUTH_WPA_WPA2_PSK,
            .beacon_interval = 100,
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started  SSID=\"%s\"  IP=192.168.4.1", WIFI_SSID);
}

// ─── NMEA PARSER ─────────────────────────────────────────────
// Simple NMEA $GNRMC/$GPGGA parser for Quectel EG91-EX GPS
static void parse_nmea_line(const char *line) {
    // Example: $GNRMC,hhmmss.sss,A,lat,NS,lon,EW,...
    if (strstr(line, "$GNRMC,") == line) {
        char status;
        double lat_raw, lon_raw;
        char ns, ew;
        int n = sscanf(line, "$GNRMC,%*[^,],%c,%lf,%c,%lf,%c,",
                       &status, &lat_raw, &ns, &lon_raw, &ew);
        if (n == 5 && status == 'A') {
            // Convert NMEA format (ddmm.mmmm) to decimal degrees
            int lat_deg = (int)(lat_raw / 100);
            double lat_min = lat_raw - lat_deg * 100;
            double lat = lat_deg + lat_min / 60.0;
            if (ns == 'S') lat = -lat;
            int lon_deg = (int)(lon_raw / 100);
            double lon_min = lon_raw - lon_deg * 100;
            double lon = lon_deg + lon_min / 60.0;
            if (ew == 'W') lon = -lon;
            g_lat = (float)lat;
            g_lon = (float)lon;
            update_movement_state();
            g_has_real_gps = 1;
        }
    }
}

// ─── GPS TASK ───────────────────────────────────────────────
// Reads NMEA lines from EG91-EX and updates g_lat/g_lon
static void gps_task(void *arg) {
    char line[128];
    int idx = 0;
    uint8_t ch;
    ESP_LOGI(TAG, "GPS task started");
    while (1) {
        int len = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (len == 1) {
            if (ch == '\n' || idx >= (int)sizeof(line)-1) {
                line[idx] = '\0';
                if (idx > 6 && (strstr(line, "$GNRMC,") == line)) {
                    parse_nmea_line(line);
                    g_has_real_gps = 1;
                }
                idx = 0;
            } else if (ch != '\r') {
                line[idx++] = ch;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ─── MAIN ─────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "=== CarDash Monitor booting ===");

    // Non-volatile storage (required by WiFi)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erasing and re-initialising...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // Seed RNG from hardware timer
    srand((unsigned int)esp_timer_get_time());

    // Peripherals
    uart_init();
    pwrkey_init();
    modem_power_on();

    // Network + web
    wifi_init();
    start_webserver();

    // Background simulation (replace with real sensor task in production)
    xTaskCreate(sim_task, "sim_task", 4096, NULL, 5, NULL);

    // Start GPS task for real Quectel EG91-EX GPS
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "=== System ready — open http://192.168.4.1 ===");
}