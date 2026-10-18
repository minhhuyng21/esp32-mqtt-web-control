
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "mqtt_client.h"

#include "password.h"

/* Embedded EMQX CA certificate */
extern const uint8_t emqxsl_ca_pem_start[]
    asm("_binary_emqxsl_ca_pem_start");

/* Hardware configuration */
#define LED_GPIO 16

/* MQTT topics */
#define TOPIC_SET   "home/led1/set"
#define TOPIC_STATE "home/led1/state"

static const char *TAG = "ESP32_MQTT";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_started = false;

/* Handle MQTT events */
static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
)
{
    esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to EMQX Cloud");

        esp_mqtt_client_subscribe(
            mqtt_client,
            TOPIC_SET,
            1
        );

        ESP_LOGI(TAG, "Subscribed to: %s", TOPIC_SET);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscription successful");
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(
            TAG,
            "Received MQTT message, topic=%.*s, data=%.*s",
            event->topic_len,
            event->topic,
            event->data_len,
            event->data
        );

        if (
            event->topic_len == strlen(TOPIC_SET) &&
            strncmp(
                event->topic,
                TOPIC_SET,
                event->topic_len
            ) == 0
        ) {
            const char *state = NULL;

            /* Copy payload into a null-terminated buffer */
            char command[32];
            int len = event->data_len;

            if (len > 0 && len < sizeof(command)) {
                memcpy(command, event->data, len);
                command[len] = '\0';

                /* Remove trailing whitespace */
                while (len > 0 &&
                    (command[len - 1] == '\n' ||
                        command[len - 1] == '\r' ||
                        command[len - 1] == ' ' ||
                        command[len - 1] == '\t')) {
                    command[--len] = '\0';
                }

                /* Remove leading whitespace */
                char *cmd = command;
                while (*cmd == ' ' || *cmd == '\t') {
                    cmd++;
                }

                if (strcmp(cmd, "ON") == 0) {
                    gpio_set_level(LED_GPIO, 1);
                    state = "ON";
                } else if (strcmp(cmd, "OFF") == 0) {
                    gpio_set_level(LED_GPIO, 0);
                    state = "OFF";
                }
            }

            if (state != NULL) {
                ESP_LOGI(TAG, "LED state: %s", state);

                esp_mqtt_client_publish(
                    mqtt_client,
                    TOPIC_STATE,
                    state,
                    0,
                    1,
                    0
                );
            } else {
                ESP_LOGW(
                    TAG,
                    "Invalid LED command, len=%d, payload=[%.*s]",
                    event->data_len,
                    event->data_len,
                    event->data
                );
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT event error");

        if (event->error_handle != NULL) {
            ESP_LOGE(
                TAG,
                "MQTT error type: %d",
                event->error_handle->error_type
            );

            if (
                event->error_handle->esp_tls_last_esp_err
                != ESP_OK
            ) {
                ESP_LOGE(
                    TAG,
                    "TLS error: 0x%x",
                    event->error_handle->esp_tls_last_esp_err
                );
            }
        }
        break;

    default:
        break;
    }
}

/* Initialize and start MQTT client */
static void start_mqtt(void)
{
    if (mqtt_started) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,

        /* Enable TLS certificate verification */
        .broker.verification.certificate = (const char *)emqxsl_ca_pem_start,

        .credentials.username = MQTT_USERNAME,

        .credentials.authentication.password =
            MQTT_PASSWORD,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    ESP_ERROR_CHECK(
        mqtt_client != NULL ? ESP_OK : ESP_FAIL
    );

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_mqtt_client_start(mqtt_client)
    );

    mqtt_started = true;

    ESP_LOGI(TAG, "Starting MQTT client...");
}

/* Handle Wi-Fi and IP events */
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START
    ) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
        esp_wifi_connect();
    }
    else if (
        event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED
    ) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying...");
        esp_wifi_connect();
    }
    else if (
        event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    ) {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "Got IP: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        start_mqtt();
    }
}

/* Application entry point */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 MQTT application");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();

    if (
        ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    else {
        ESP_ERROR_CHECK(ret);
    }

    /* Initialize network interface */
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    /* Initialize Wi-Fi */
    wifi_init_config_t wifi_cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&wifi_cfg)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    /* Configure Wi-Fi credentials */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    /* Configure LED GPIO */
    ESP_ERROR_CHECK(
        gpio_reset_pin(LED_GPIO)
    );

    ESP_ERROR_CHECK(
        gpio_set_direction(
            LED_GPIO,
            GPIO_MODE_OUTPUT
        )
    );

    ESP_ERROR_CHECK(
        gpio_set_level(LED_GPIO, 0)
    );

    /* Start Wi-Fi */
    ESP_ERROR_CHECK(
        esp_wifi_start()
    );
}
