#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"         

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "driver/dac_oneshot.h"

/* ── Configuración ──────────────────────────────────────── */
#define WIFI_SSID       "Galaxy A21 sFB42"
#define WIFI_PASS       "12246778"
#define WIFI_MAX_RETRY  10

/* GPIOs de los relés (activo en HIGH — ajusta si tu módulo es activo en LOW) */
#define RELAY_1_GPIO    GPIO_NUM_26
#define RELAY_2_GPIO    GPIO_NUM_27
#define RELAY_3_GPIO    GPIO_NUM_14

/* ¿Los relés son activo en LOW? Cambia a 1 si tu módulo de relé se activa con 0V */
#define RELAY_ACTIVE_LOW  0

/* ── Macros de nivel lógico ─────────────────────────────── */
#if RELAY_ACTIVE_LOW
  #define RELAY_ON  0
  #define RELAY_OFF 1
#else
  #define RELAY_ON  1
  #define RELAY_OFF 0
#endif

/* ── Constantes internas ────────────────────────────────── */
static const char *TAG = "DraftCtrl";
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static char s_device_ip[20] = "0.0.0.0";
dac_oneshot_handle_t dac_handle;

/* ── Estructuras ────────────────────────────────── */
typedef struct {
    int tiempo_ms;
    int rele1;
    int rele2;
    int dac_valor;
} step_t;

typedef struct {
    const char* nombre;
    step_t* steps;
    int num_steps;
} perfil_t;


// 80/20 - suave
step_t perfil_80_20_steps[] = {
    {2000, 1, 0, 50},
    {5000, 1, 0, 40},
    {2000, 0, 0, 30},
};

// 70/30 - equilibrado
step_t perfil_70_30_steps[] = {
    {2000, 1, 0, 60},
    {3000, 1, 0, 70},
    {2000, 0, 1, 80},
};

// 50/50 - espumoso
step_t perfil_50_50_steps[] = {
    {1000, 0, 1, 90},
    {3000, 0, 1, 100},
    {2000, 0, 1, 80},
};

/* ================================================================
   ARCHIVOS WEB EMBEBIDOS
   En producción usa esp_vfs_spiffs o LittleFS.
   Aquí se incluyen como strings externos generados con:
     xxd -i index.html > index_html.h
   Para simplificar el ejemplo se referencia con extern.
   Si prefieres, pega el HTML/CSS/JS aquí como raw strings.
   ================================================================ */

/* Alternativamente, puedes usar SPIFFS:
   idf.py menuconfig → Component config → SPIFFS → habilitar
   y subir los archivos con:
     idf.py -p /dev/ttyUSB0 spiffs_create_partition_image
*/

/* Declaraciones externas de los archivos embebidos.
   Añade en CMakeLists.txt:
     target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "index.html" TEXT)
     target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "style.css"  TEXT)
     target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "app.js"     TEXT)
*/
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char style_css_start[]  asm("_binary_style_css_start");
extern const char style_css_end[]    asm("_binary_style_css_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");

/* ================================================================
   GPIO — Relés
   ================================================================ */

static const gpio_num_t RELAY_GPIOS[3] = {
    RELAY_1_GPIO,
    RELAY_2_GPIO,
    RELAY_3_GPIO,
};

static void relays_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RELAY_1_GPIO) |
                        (1ULL << RELAY_2_GPIO) |
                        (1ULL << RELAY_3_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Todos los relés apagados al inicio */
    for (int i = 0; i < 3; i++) {
        gpio_set_level(RELAY_GPIOS[i], RELAY_OFF);
    }
    ESP_LOGI(TAG, "Relés inicializados — todos OFF");
}

static void relay_set(int ch, int state)
{
    if (ch < 1 || ch > 3) return;
    int level = state ? RELAY_ON : RELAY_OFF;
    gpio_set_level(RELAY_GPIOS[ch - 1], level);
    ESP_LOGI(TAG, "Relé %d → %s (GPIO %d = %d)",
             ch, state ? "ON" : "OFF", RELAY_GPIOS[ch - 1], level);
}

static void relays_all_off(void)
{
    for (int i = 1; i <= 3; i++) relay_set(i, 0);
    ESP_LOGW(TAG, "PARO DE EMERGENCIA — todos los relés OFF");
}

/* ================================================================
   WiFi
   ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Reintentando WiFi (%d/%d)…", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "No se pudo conectar al AP");
        }

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_device_ip, sizeof(s_device_ip),
                 IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "IP obtenida: %s", s_device_ip);
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando a: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado — IP: %s", s_device_ip);
    } else {
        ESP_LOGE(TAG, "Fallo de conexión WiFi");
    }
}


esp_err_t init_dac(void)
{
    dac_oneshot_config_t config = {
        .chan_id = DAC_CHAN_1
    };

    ESP_ERROR_CHECK(dac_oneshot_new_channel(&config, &dac_handle));

    return ESP_OK;
}

/* ================================================================
   HTTP — Helpers
   ================================================================ */

/* Agrega cabeceras CORS para que el browser no bloquee */
static esp_err_t set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

/* Lee el body completo del request */
static int read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)buf_len) return -1;

    int received = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received,
                                 MIN(remaining, (int)(buf_len - received - 1)));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        received  += ret;
        remaining -= ret;
    }
    buf[received] = '\0';
    return received;
}

/* ================================================================
   HTTP — Handlers
   ================================================================ */

/* GET / → index.html */
static esp_err_t handler_root(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = index_html_end - index_html_start;
    httpd_resp_send(req, index_html_start, len);
    return ESP_OK;
}

/* GET /style.css */
static esp_err_t handler_css(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    size_t len = style_css_end - style_css_start;
    httpd_resp_send(req, style_css_start, len);
    return ESP_OK;
}

/* GET /app.js */
static esp_err_t handler_js(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    size_t len = app_js_end - app_js_start;
    httpd_resp_send(req, app_js_start, len);
    return ESP_OK;
}

/* GET /ping → {"status":"ok","ip":"x.x.x.x","uptime_ms":...} */
static esp_err_t handler_ping(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"ip\":\"%s\",\"uptime_ms\":%llu}",
             s_device_ip,
             (unsigned long long)(esp_timer_get_time() / 1000ULL));

    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /relay → {"ch":1-3,"state":0|1} */
static esp_err_t handler_relay(httpd_req_t *req)
{
    set_cors_headers(req);

    /* Preflight OPTIONS */
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body inválido");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON inválido");
        return ESP_FAIL;
    }

    cJSON *ch_item    = cJSON_GetObjectItem(root, "ch");
    cJSON *state_item = cJSON_GetObjectItem(root, "state");

    if (!cJSON_IsNumber(ch_item) || !cJSON_IsNumber(state_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Campos faltantes");
        return ESP_FAIL;
    }

    int ch    = (int)ch_item->valuedouble;
    int st    = (int)state_item->valuedouble;
    cJSON_Delete(root);

    if (ch < 1 || ch > 3) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Canal inválido (1-3)");
        return ESP_FAIL;
    }

    relay_set(ch, st);

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"ch\":%d,\"state\":%d}", ch, st);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /stop → {"all":1} */
static esp_err_t handler_stop(httpd_req_t *req)
{
    set_cors_headers(req);

    if (req->method == HTTP_OPTIONS) {
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    relays_all_off();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"all_off\":true}");
    return ESP_OK;
}

/* ================================================================
   HTTP — Inicio del servidor
   ================================================================ */

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    config.server_port      = 80;
    config.stack_size       = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error al iniciar servidor HTTP");
        return NULL;
    }

    /* Registrar rutas */
    httpd_uri_t uris[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = handler_root  },
        { .uri = "/style.css",.method = HTTP_GET,  .handler = handler_css   },
        { .uri = "/app.js",   .method = HTTP_GET,  .handler = handler_js    },
        { .uri = "/ping",     .method = HTTP_GET,  .handler = handler_ping  },
        { .uri = "/relay",    .method = HTTP_POST, .handler = handler_relay },
        { .uri = "/relay",    .method = HTTP_OPTIONS,.handler=handler_relay  },
        { .uri = "/stop",     .method = HTTP_POST, .handler = handler_stop  },
        { .uri = "/stop",     .method = HTTP_OPTIONS,.handler=handler_stop   },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }

    ESP_LOGI(TAG, "Servidor HTTP activo en http://%s/", s_device_ip);
    return server;
}

/* ================================================================
   app_main
   ================================================================ */

void app_main(void)
{
    /* NVS (requerido por WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== Draft Control Firmware ===");
    ESP_LOGI(TAG, "IDF: %s", esp_get_idf_version());

    esp_err_t err = init_dac();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "DAC listo para usar");
    } else {
        ESP_LOGE(TAG, "Error al inicializar DAC: %d", err);
    }
    

    /* Inicializar relés primero — todos OFF */
    relays_init();

    /* Conectar WiFi */
    wifi_init_sta();

    /* Iniciar servidor HTTP */
    start_webserver();

    /* Loop de heartbeat */
    while (true) {
        ESP_LOGD(TAG, "Heap libre: %lu bytes",
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}