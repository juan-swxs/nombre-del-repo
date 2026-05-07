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

/* ── Configuración WiFi ─────────────────────────────────── */
#define WIFI_SSID "Galaxy A21 sFB42"
#define WIFI_PASS "12246778"
#define WIFI_MAX_RETRY 10

/* ── GPIOs válvula biestable 5/2 ────────────────────────── */
#define RELAY_A_GPIO GPIO_NUM_18 /* bobina avanzar  */
#define RELAY_B_GPIO GPIO_NUM_19 /* bobina retornar */

#define BISTABLE_PULSE_MS 1000
#define DAC_AVANCE 20
#define DAC_RETORNO 200

/* ¿Relés activo-LOW? */
#define RELAY_ACTIVE_LOW 0
#if RELAY_ACTIVE_LOW
#define RELAY_ON 0
#define RELAY_OFF 1
#else
#define RELAY_ON 1
#define RELAY_OFF 0
#endif

/* ── Constantes internas ────────────────────────────────── */
static const char *TAG = "DraftCtrl";
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static char s_device_ip[20] = "0.0.0.0";

static dac_oneshot_handle_t dac_handle;
static TaskHandle_t profile_task_handle = NULL;

static volatile bool g_stop_requested = false;
static volatile int g_active_profile = -1;

typedef struct
{
    const char *nombre;
    int avance_ms;  /* tiempo de avance (calibra el ángulo)  */
    int retorno_ms; /* espera tras pulso B para llegar a 0°  */
} perfil_angulo_t;

static const perfil_angulo_t PERFILES[] = {
    /* nombre      avance_ms   retorno_ms */
    {"ang_ch1", 400, 500},   /* ch=1 — ángulo pequeño  */
    {"ang_ch2", 980, 1400}, /* ch=2 — ángulo medio    */
    {"ang_ch3", 1800, 2200}, /* ch=3 — ángulo grande   */
};
#define NUM_PERFILES (sizeof(PERFILES) / sizeof(PERFILES[0]))

/* ── Archivos web embebidos ─────────────────────────────── */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char style_css_start[] asm("_binary_style_css_start");
extern const char style_css_end[] asm("_binary_style_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");

/* ================================================================
   GPIO — Relés
   ================================================================ */

static void relays_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RELAY_A_GPIO) | (1ULL << RELAY_B_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(RELAY_A_GPIO, RELAY_OFF);
    gpio_set_level(RELAY_B_GPIO, RELAY_OFF);
    ESP_LOGI(TAG, "Relés inicializados — A y B en OFF");
}

/*
 * Pulso de 1 segundo a la bobina indicada.
 * Garantiza que la otra bobina esté apagada antes de pulsar.
 *   bobina=1 → RELAY_A (avanzar)
 *   bobina=2 → RELAY_B (retornar)
 */
static void bistable_pulse(int bobina)
{
    if (bobina != 1 && bobina != 2)
        return;

    gpio_set_level(RELAY_A_GPIO, RELAY_OFF);
    gpio_set_level(RELAY_B_GPIO, RELAY_OFF);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_num_t pin = (bobina == 1) ? RELAY_A_GPIO : RELAY_B_GPIO;
    const char *nombre = (bobina == 1) ? "A (avanzar)" : "B (retornar)";

    gpio_set_level(pin, RELAY_ON);
    ESP_LOGI(TAG, "Pulso bobina %s → ON (%d ms)", nombre, BISTABLE_PULSE_MS);
    vTaskDelay(pdMS_TO_TICKS(BISTABLE_PULSE_MS));
    gpio_set_level(pin, RELAY_OFF);
    ESP_LOGI(TAG, "Pulso bobina %s → OFF", nombre);
}

/* ================================================================
   DAC
   ================================================================ */

static esp_err_t init_dac(void)
{
    dac_oneshot_config_t config = {.chan_id = DAC_CHAN_1};
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&config, &dac_handle));

    /* IMPORTANTE: arranca en 0V — motor completamente quieto */
    dac_oneshot_output_voltage(dac_handle, 0);
    ESP_LOGI(TAG, "DAC inicializado → GPIO 26, salida = 0V (motor quieto)");
    return ESP_OK;
}

/* ================================================================
   Parada de emergencia
   ================================================================ */

static void emergency_stop(void)
{
    g_stop_requested = true;

    /* 1. Cortar presión inmediatamente */
    dac_oneshot_output_voltage(dac_handle, 0);
    gpio_set_level(RELAY_A_GPIO, RELAY_OFF);
    gpio_set_level(RELAY_B_GPIO, RELAY_OFF);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 2. Retornar con presión máxima de seguridad */
    ESP_LOGW(TAG, "EMERGENCIA — retornando a 0° con DAC=%d", DAC_RETORNO);
    dac_oneshot_output_voltage(dac_handle, DAC_RETORNO);
    bistable_pulse(2);
    vTaskDelay(pdMS_TO_TICKS(2500));
    dac_oneshot_output_voltage(dac_handle, 0);

    g_active_profile = -1;
    ESP_LOGW(TAG, "PARO DE EMERGENCIA — ejecutado");
}

/* ================================================================
   Tarea de ejecución de perfiles de ángulo
   ================================================================ */

static void profile_task(void *pvParam)
{
    while (true)
    {
        /* Espera notificación con índice de perfil (valor = idx+1) */
        uint32_t notif = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int idx = (int)(notif - 1);

        if (idx < 0 || idx >= (int)NUM_PERFILES)
        {
            ESP_LOGE(TAG, "Índice de perfil inválido: %d", idx);
            continue;
        }

        const perfil_angulo_t *p = &PERFILES[idx];
        g_active_profile = idx;
        g_stop_requested = false;

        ESP_LOGI(TAG, "▶ Canal %d [%s] — avance=%d ms, retorno=%d ms",
                 idx + 1, p->nombre, p->avance_ms, p->retorno_ms);

        /* ── PASO 1: DAC = 0 — asegurar motor quieto antes de todo ── */
        dac_oneshot_output_voltage(dac_handle, 0);
        vTaskDelay(pdMS_TO_TICKS(50));

        if (g_stop_requested)
            goto retorno;

        /* ── PASO 2: Pulso RELAY_A — habilitar dirección avance ── */
        bistable_pulse(1);

        if (g_stop_requested)
            goto retorno;

        /* ── PASO 3: DAC = DAC_AVANCE — presión baja, motor gira ── */
        dac_oneshot_output_voltage(dac_handle, DAC_AVANCE);
        ESP_LOGI(TAG, "  DAC=%d (%.2fV) — motor avanzando",
                 DAC_AVANCE, DAC_AVANCE * 3.3f / 255.0f);

        /* ── PASO 4: Espera avance_ms en chunks de 50 ms (abortable) ── */
        {
            int remaining = p->avance_ms;
            while (remaining > 0 && !g_stop_requested)
            {
                int chunk = MIN(remaining, 50);
                vTaskDelay(pdMS_TO_TICKS(chunk));
                remaining -= chunk;
            }
        }

        /* ── PASO 5: DAC = 0 — cortar presión, motor se detiene ── */
        dac_oneshot_output_voltage(dac_handle, 0);
        ESP_LOGI(TAG, "  DAC=0 — motor detenido en ángulo objetivo");

        if (g_stop_requested)
            goto retorno;

        /* ── PASO 6: Pausa de estabilización mecánica ── */
        vTaskDelay(pdMS_TO_TICKS(4000));

    retorno:
        /* ── PASO 7-9: Retorno a 0° ────────────────────────────── */
        /* ── RETORNO CORREGIDO ── */

        // 1. Cortar presión PRIMERO
        dac_oneshot_output_voltage(dac_handle, 0);
        vTaskDelay(pdMS_TO_TICKS(100)); // asegurar que el motor para

        // 2. Pulso RELAY_B — válvula conmuta SIN presión
        bistable_pulse(2);

        // 3. Esperar que la válvula termine de conmutar mecánicamente
        vTaskDelay(pdMS_TO_TICKS(200));

        // 4. AHORA sí subir presión — ya está en dirección correcta
        dac_oneshot_output_voltage(dac_handle, DAC_RETORNO);

        // 5. Esperar retorno_ms
        {
            int remaining = p->retorno_ms;
            while (remaining > 0)
            {
                vTaskDelay(pdMS_TO_TICKS(MIN(remaining, 100)));
                remaining -= MIN(remaining, 100);
            }
        }

        // 6. Cortar presión al final
        dac_oneshot_output_voltage(dac_handle, 0);

        if (!g_stop_requested)
        {
            ESP_LOGI(TAG, "✔ Canal %d completado y retornado a 0°", idx + 1);
            bistable_pulse(1);
        }
        else
        {
            ESP_LOGW(TAG, "⚠ Canal %d abortado — retornado a 0°", idx + 1);
        }

        g_active_profile = -1;
    }
}

/* ================================================================
   WiFi
   ================================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_count < WIFI_MAX_RETRY)
        {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Reintentando WiFi (%d/%d)…", s_retry_count, WIFI_MAX_RETRY);
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "No se pudo conectar al AP");
        }
    }
    else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_device_ip, sizeof(s_device_ip), IPSTR, IP2STR(&ev->ip_info.ip));
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

    esp_event_handler_instance_t inst_any, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
        ESP_LOGI(TAG, "WiFi conectado — IP: %s", s_device_ip);
    else
        ESP_LOGE(TAG, "Fallo de conexión WiFi");
}

/* ================================================================
   HTTP — Helpers
   ================================================================ */

static esp_err_t set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

static int read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)buf_len)
        return -1;

    int received = 0;
    while (remaining > 0)
    {
        int ret = httpd_req_recv(req, buf + received,
                                 MIN(remaining, (int)(buf_len - received - 1)));
        if (ret <= 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            return -1;
        }
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';
    return received;
}

/* ================================================================
   HTTP — Handlers
   ================================================================ */

static esp_err_t handler_root(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t handler_css(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, style_css_start, style_css_end - style_css_start);
    return ESP_OK;
}

static esp_err_t handler_js(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, app_js_start, app_js_end - app_js_start);
    return ESP_OK;
}

/* GET /ping */
static esp_err_t handler_ping(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    const char *perfil_activo = (g_active_profile >= 0)
                                    ? PERFILES[g_active_profile].nombre
                                    : "idle";

    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"ip\":\"%s\",\"uptime_ms\":%llu,\"perfil\":\"%s\"}",
             s_device_ip,
             (unsigned long long)(esp_timer_get_time() / 1000ULL),
             perfil_activo);

    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/*
 * POST /relay  { "ch": 1-3, "state": 0|1 }
 *
 *   state=1  →  ejecutar perfil de ángulo del canal
 *   state=0  →  detener y retornar a 0°
 */
static esp_err_t handler_relay(httpd_req_t *req)
{
    set_cors_headers(req);

    if (req->method == HTTP_OPTIONS)
    {
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char body[128] = {0};
    if (read_body(req, body, sizeof(body)) < 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body inválido");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON inválido");
        return ESP_FAIL;
    }

    cJSON *ch_item = cJSON_GetObjectItem(root, "ch");
    cJSON *state_item = cJSON_GetObjectItem(root, "state");

    if (!cJSON_IsNumber(ch_item) || !cJSON_IsNumber(state_item))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Campos faltantes");
        return ESP_FAIL;
    }

    int ch = (int)ch_item->valuedouble;
    int st = (int)state_item->valuedouble;
    cJSON_Delete(root);

    if (ch < 1 || ch > 3)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Canal inválido (1-3)");
        return ESP_FAIL;
    }

    if (st == 0)
    {
        /* Detener perfil activo y retornar */
        if (g_active_profile >= 0)
        {
            g_stop_requested = true;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        ESP_LOGI(TAG, "Canal %d — stop solicitado", ch);
    }
    else
    {
        /* Lanzar perfil del canal */
        int idx = ch - 1;

        if (g_active_profile >= 0)
        {
            g_stop_requested = true;
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        g_stop_requested = false;
        xTaskNotify(profile_task_handle, (uint32_t)(idx + 1), eSetValueWithOverwrite);

        ESP_LOGI(TAG, "Canal %d → perfil [%s] lanzado", ch, PERFILES[idx].nombre);
    }

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"ch\":%d,\"state\":%d}", ch, st);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /stop → parada de emergencia */
static esp_err_t handler_stop(httpd_req_t *req)
{
    set_cors_headers(req);

    if (req->method == HTTP_OPTIONS)
    {
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    emergency_stop();

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
    config.max_uri_handlers = 10;
    config.server_port = 80;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Error al iniciar servidor HTTP");
        return NULL;
    }

    httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_root},
        {.uri = "/style.css", .method = HTTP_GET, .handler = handler_css},
        {.uri = "/app.js", .method = HTTP_GET, .handler = handler_js},
        {.uri = "/ping", .method = HTTP_GET, .handler = handler_ping},
        {.uri = "/relay", .method = HTTP_POST, .handler = handler_relay},
        {.uri = "/relay", .method = HTTP_OPTIONS, .handler = handler_relay},
        {.uri = "/stop", .method = HTTP_POST, .handler = handler_stop},
        {.uri = "/stop", .method = HTTP_OPTIONS, .handler = handler_stop},
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
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
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== Draft Control Firmware v3.0 — Motor Angular ===");
    ESP_LOGI(TAG, "IDF: %s", esp_get_idf_version());

    /* DAC en 0V — motor completamente quieto al arrancar */
    ESP_ERROR_CHECK(init_dac());

    /* Relés ambos OFF */
    relays_init();

    /* WiFi */
    wifi_init_sta();

    /* Tarea de perfiles angulares */
    xTaskCreate(profile_task, "profile_task", 4096, NULL, 5, &profile_task_handle);
    ESP_LOGI(TAG, "Tarea de perfiles creada");

    /* Servidor HTTP */
    start_webserver();

    /* Heartbeat */
    while (true)
    {
        ESP_LOGD(TAG, "Heap libre: %lu bytes  |  Canal activo: %s",
                 (unsigned long)esp_get_free_heap_size(),
                 (g_active_profile >= 0) ? PERFILES[g_active_profile].nombre : "idle");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}