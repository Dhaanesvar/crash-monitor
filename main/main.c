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

// ---------------- CONFIG ----------------
#define UART_NUM UART_NUM_1
#define TX_PIN   17
#define RX_PIN   16
#define BUF_SIZE 1024
#define PWRKEY_PIN 4

static const char *TAG = "APP";

// ---------------- SIM DATA ----------------
float lat = 3.0738;
float lon = 101.5183;
float speed = 0;
int crash = 0;

// ---------------- UART ----------------
void uart_init()
{
    uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1
    };

    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &config);
    uart_set_pin(UART_NUM, TX_PIN, RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

// ---------------- MODEM ----------------
void pwrkey_init()
{
    gpio_set_direction(PWRKEY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PWRKEY_PIN, 1);
}

void modem_power_on()
{
    gpio_set_level(PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level(PWRKEY_PIN, 1);

    ESP_LOGI(TAG, "Waiting modem...");
    vTaskDelay(pdMS_TO_TICKS(15000));
}

// ---------------- SMS ----------------
void send_sms()
{
    char msg[128];

    sprintf(msg, "CRASH!\nLat: %.6f\nLon: %.6f", lat, lon);

    uart_write_bytes(UART_NUM, "AT+CMGF=1\r", 10);
    vTaskDelay(pdMS_TO_TICKS(1000));

    uart_write_bytes(UART_NUM, "AT+CMGS=\"+601121457455\"\r", 25);
    vTaskDelay(pdMS_TO_TICKS(2000));

    uart_write_bytes(UART_NUM, msg, strlen(msg));

    char ctrlz = 0x1A;
    uart_write_bytes(UART_NUM, &ctrlz, 1);

    ESP_LOGI(TAG, "SMS SENT");
}

// ---------------- GPS SIMULATION ----------------
void gps_sim_task(void *arg)
{
    while (1)
    {
        // BIG movement so you can SEE it
        lat += ((rand() % 10) - 5) * 0.0005;
        lon += ((rand() % 10) - 5) * 0.0005;

        speed = 40 + (rand() % 40);

        ESP_LOGI("SIM", "LAT %.6f LON %.6f SPEED %.2f", lat, lon, speed);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------------- CRASH ----------------
void trigger_crash()
{
    crash = 1;
    ESP_LOGE(TAG, "CRASH!");
    send_sms();
}

// ---------------- WEB ----------------
httpd_handle_t server = NULL;

esp_err_t data_handler(httpd_req_t *req)
{
    char resp[256];

    sprintf(resp,
        "{\"lat\":%.6f,\"lon\":%.6f,\"speed\":%.2f,\"crash\":%d}",
        lat, lon, speed, crash);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t crash_handler(httpd_req_t *req)
{
    trigger_crash();
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------- HTML (GUARANTEED MAP) ----------------
const char *html_page =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"

"<style>"
"body{margin:0;background:#000;color:#fff;font-family:Arial;text-align:center;}"
"h2{margin-top:20px;}"

/* GAUGE CONTAINER */
".gauge{"
" width:250px;height:250px;border-radius:50%;"
" background:conic-gradient("
" green 0deg 100deg,"
" yellow 100deg 140deg,"
" red 140deg 180deg);"
" margin:30px auto;position:relative;"
"}"

/* INNER CIRCLE */
".inner{"
" position:absolute;width:200px;height:200px;"
" background:#000;border-radius:50%;"
" top:25px;left:25px;"
"}"

/* NEEDLE */
".needle{"
" width:4px;height:100px;background:white;"
" position:absolute;bottom:50%;left:50%;"
" transform-origin:bottom;"
" transform:rotate(0deg);"
" transition: transform 0.5s ease-out;"
"}"

/* CENTER DOT */
".center{"
" width:10px;height:10px;background:white;"
" border-radius:50%;position:absolute;"
" top:50%;left:50%;transform:translate(-50%,-50%);"
"}"

"</style>"
"</head><body>"

"<h2>🚗 RPM Dashboard</h2>"

/* GAUGE */
"<div class='gauge'>"
"<div class='inner'></div>"
"<div id='needle' class='needle'></div>"
"<div class='center'></div>"
"</div>"

/* DATA DISPLAY */
"<h3>Speed: <span id='speed'>0</span> km/h</h3>"
"<h3>RPM: <span id='rpm'>0</span></h3>"

"<button onclick='crash()'>💥 Crash</button>"

"<script>"

"let currentAngle = 0;"

// UPDATE LOOP
"async function update(){"
" let r = await fetch('/data');"
" let d = await r.json();"

// SPEED DISPLAY
" document.getElementById('speed').innerText = d.speed.toFixed(1);"

// RPM CALCULATION
" let rpm = d.speed * 100;"
" if(rpm > 8000) rpm = 8000;"

// DISPLAY RPM
" document.getElementById('rpm').innerText = Math.floor(rpm);"

// TARGET ANGLE (0–180 deg)
" let targetAngle = (rpm / 8000) * 180;"

// SMOOTH ANIMATION
" currentAngle += (targetAngle - currentAngle) * 0.2;"

// APPLY ROTATION
" document.getElementById('needle').style.transform = "
"  'rotate(' + currentAngle + 'deg)';"

// CRASH EFFECT
" if(d.crash==1){"
"   document.body.style.background = 'darkred';"
" }"
"}"

// CRASH BUTTON
"function crash(){fetch('/crash');}"

// LOOP
"setInterval(update,200);"

"</script></body></html>";

esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &config);

    httpd_uri_t root = { .uri="/", .method=HTTP_GET, .handler=root_handler };
    httpd_uri_t data = { .uri="/data", .method=HTTP_GET, .handler=data_handler };
    httpd_uri_t crash = { .uri="/crash", .method=HTTP_GET, .handler=crash_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &data);
    httpd_register_uri_handler(server, &crash);
}

// ---------------- WIFI ----------------
void wifi_init()
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap = {
        .ap = {
            .ssid = "CRASH_MONITOR",
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
}

// ---------------- MAIN ----------------
void app_main()
{
    nvs_flash_init();

    srand((unsigned int)esp_timer_get_time());

    uart_init();
    pwrkey_init();
    modem_power_on();

    wifi_init();
    start_webserver();

    xTaskCreate(gps_sim_task, "gps_sim", 4096, NULL, 5, NULL);
}