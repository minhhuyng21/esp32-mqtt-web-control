
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "password.h"
#define LED_GPIO 4


static const char *TAG = "APP";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_started = false;

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to MQTT broker");

        esp_mqtt_client_subscribe(
            mqtt_client, "home/led1/set", 0);

        ESP_LOGI(TAG, "Subscribed: home/led1/set");
        break;

    
    case MQTT_EVENT_DATA:
        if (event->topic_len == strlen("home/led1/set") &&
            strncmp(event->topic, "home/led1/set",
                    event->topic_len) == 0) {

            const char *state = NULL;

            if (event->data_len == 2 &&
                strncmp(event->data, "ON", 2) == 0) {

                gpio_set_level(LED_GPIO, 1);
                state = "ON";

            } else if (event->data_len == 3 &&
                    strncmp(event->data, "OFF", 3) == 0) {

                gpio_set_level(LED_GPIO, 0);
                state = "OFF";
            }

            if (state != NULL) {
                ESP_LOGI(TAG, "LED state: %s", state);

                esp_mqtt_client_publish(
                    mqtt_client,
                    "home/led1/state",
                    state,
                    0,
                    1,
                    0
                );
            }
        }
        break;


    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT event error");
        break;

    default:
        break;
    }
}

static void start_mqtt(void)
{
    if (mqtt_started) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(mqtt_client != NULL ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL));

    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    mqtt_started = true;
    ESP_LOGI(TAG, "Starting MQTT client...");
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "Got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        start_mqtt();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,
            &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    ESP_ERROR_CHECK(esp_wifi_start());
}
