#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "init_board.h"
#include "lvgl.h"
#include "nvs_flash.h"

#define CAM_W 240
#define CAM_H 240
#define MAX_FACE_BOXES 5
#define WIFI_CONNECTED_BIT BIT0

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
} face_box_t;

typedef struct {
    char *buffer;
    int buffer_size;
    int length;
} http_response_t;

static const char *TAG = "remote_face";
static EventGroupHandle_t wifi_event_group;
static lv_obj_t *camera_img;
static lv_obj_t *status_label;
static lv_obj_t *count_label;
static lv_obj_t *box_lines[MAX_FACE_BOXES];
static lv_color_t *cam_buf;
static uint8_t *detect_buf;
static lv_img_dsc_t cam_img;
static face_box_t boxes[MAX_FACE_BOXES];
static int box_count;
static volatile bool detect_busy;
static TaskHandle_t detect_task_handle;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_REMOTE_FACE_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_REMOTE_FACE_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void ui_set_status(const char *text)
{
    if (lvgl_lock(100)) {
        lv_label_set_text(status_label, text);
        lvgl_unlock();
    }
}

static void ui_update_boxes(void)
{
    static lv_point_t points[MAX_FACE_BOXES][5];
    char count_text[16];

    if (!lvgl_lock(100)) {
        return;
    }

    lv_img_set_src(camera_img, &cam_img);
    snprintf(count_text, sizeof(count_text), "faces: %d", box_count);
    lv_label_set_text(count_label, count_text);

    for (int i = 0; i < MAX_FACE_BOXES; i++) {
        if (i < box_count) {
            points[i][0] = (lv_point_t){boxes[i].x1, boxes[i].y1};
            points[i][1] = (lv_point_t){boxes[i].x1, boxes[i].y2};
            points[i][2] = (lv_point_t){boxes[i].x2, boxes[i].y2};
            points[i][3] = (lv_point_t){boxes[i].x2, boxes[i].y1};
            points[i][4] = (lv_point_t){boxes[i].x1, boxes[i].y1};
            lv_line_set_points(box_lines[i], points[i], 5);
            lv_obj_clear_flag(box_lines[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(box_lines[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lvgl_unlock();
}

static esp_err_t parse_detect_response(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *faces = cJSON_GetObjectItem(root, "faces");
    box_count = 0;
    if (cJSON_IsArray(faces)) {
        cJSON *face = NULL;
        cJSON_ArrayForEach(face, faces) {
            if (box_count >= MAX_FACE_BOXES) {
                break;
            }
            cJSON *x1 = cJSON_GetObjectItem(face, "x1");
            cJSON *y1 = cJSON_GetObjectItem(face, "y1");
            cJSON *x2 = cJSON_GetObjectItem(face, "x2");
            cJSON *y2 = cJSON_GetObjectItem(face, "y2");
            if (!cJSON_IsNumber(x1) || !cJSON_IsNumber(y1) || !cJSON_IsNumber(x2) || !cJSON_IsNumber(y2)) {
                continue;
            }
            boxes[box_count].x1 = x1->valueint;
            boxes[box_count].y1 = y1->valueint;
            boxes[box_count].x2 = x2->valueint;
            boxes[box_count].y2 = y2->valueint;
            box_count++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *response = (http_response_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && response && evt->data && evt->data_len > 0) {
        int copy_len = evt->data_len;
        int remain = response->buffer_size - response->length - 1;
        if (copy_len > remain) {
            copy_len = remain;
        }
        if (copy_len > 0) {
            memcpy(response->buffer + response->length, evt->data, copy_len);
            response->length += copy_len;
            response->buffer[response->length] = '\0';
        }
    }

    return ESP_OK;
}

static esp_err_t send_frame_for_detection(const uint8_t *data, int len)
{
    char response[1024] = {0};
    http_response_t response_ctx = {
        .buffer = response,
        .buffer_size = sizeof(response),
        .length = 0,
    };
    esp_http_client_config_t config = {
        .url = CONFIG_REMOTE_FACE_DETECT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_REMOTE_FACE_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response_ctx,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Image-Width", "240");
    esp_http_client_set_header(client, "X-Image-Height", "240");
    esp_http_client_set_header(client, "X-Image-Format", "RGB565");
    esp_http_client_set_header(client, "X-Image-Endian", "BE");
    esp_http_client_set_post_field(client, (const char *)data, len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status=%d response_len=%d", status, response_ctx.length);
        if (status == 200 && response_ctx.length > 0) {
            err = parse_detect_response(response, response_ctx.length);
        } else {
            ESP_LOGW(TAG, "HTTP status=%d response_len=%d", status, response_ctx.length);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111820), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xEAF2F8), 0);

    camera_img = lv_img_create(screen);
    lv_obj_set_pos(camera_img, 0, 0);
    lv_img_set_src(camera_img, &cam_img);

    static lv_style_t box_style;
    lv_style_init(&box_style);
    lv_style_set_line_width(&box_style, 3);
    lv_style_set_line_color(&box_style, lv_palette_main(LV_PALETTE_GREEN));

    for (int i = 0; i < MAX_FACE_BOXES; i++) {
        box_lines[i] = lv_line_create(screen);
        lv_obj_add_style(box_lines[i], &box_style, 0);
        lv_obj_add_flag(box_lines[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *status_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(status_panel);
    lv_obj_set_pos(status_panel, 0, 240);
    lv_obj_set_size(status_panel, 240, 80);
    lv_obj_set_style_bg_color(status_panel, lv_color_hex(0x111820), 0);
    lv_obj_set_style_bg_opa(status_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(status_panel, lv_color_hex(0xEAF2F8), 0);

    status_label = lv_label_create(status_panel);
    lv_obj_set_pos(status_label, 10, 14);
    lv_obj_set_width(status_label, 220);
    lv_label_set_text(status_label, "connecting");

    count_label = lv_label_create(status_panel);
    lv_obj_set_pos(count_label, 10, 46);
    lv_label_set_text(count_label, "faces: 0");
}

static void detect_task(void *param)
{
    ui_set_status("waiting wifi");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ui_set_status("remote detect ready");

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_err_t err = send_frame_for_detection(detect_buf, CAM_W * CAM_H * 2);
        ui_set_status(err == ESP_OK ? "remote detect ok" : "remote detect failed");
        ui_update_boxes();
        detect_busy = false;
    }
}

static void camera_preview_task(void *param)
{
    uint32_t last_detect_ms = 0;

    while (1) {
        uint32_t now = lv_tick_get();
        camera_fb_t *pic = esp_camera_fb_get();
        if (pic) {
            int copy_len = CAM_W * CAM_H * 2;
            if (pic->len >= copy_len) {
                memcpy(cam_buf, pic->buf, copy_len);
                ui_update_boxes();
                if (!detect_busy && lv_tick_elaps(last_detect_ms) >= CONFIG_REMOTE_FACE_DETECT_INTERVAL_MS) {
                    last_detect_ms = now;
                    detect_busy = true;
                    memcpy(detect_buf, pic->buf, copy_len);
                    xTaskNotifyGive(detect_task_handle);
                }
            }
            esp_camera_fb_return(pic);
        }
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

static void lvgl_task(void *param)
{
    while (1) {
        uint32_t delay_ms = 10;
        if (lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void app_main(void)
{
    lvgl_api_mux = xSemaphoreCreateRecursiveMutex();
    lv_init();
    wifi_init_sta();
    camera_init();
    display_init();
    touch_init();
    lv_port_disp_init();
    lv_port_indev_init();
    lvgl_tick_timer_init(EXAMPLE_LVGL_TICK_PERIOD_MS);
    bsp_brightness_init();
    bsp_brightness_set_level(80);

    cam_buf = heap_caps_malloc(CAM_W * CAM_H * 2, MALLOC_CAP_SPIRAM);
    detect_buf = heap_caps_malloc(CAM_W * CAM_H * 2, MALLOC_CAP_SPIRAM);
    if (!cam_buf || !detect_buf) {
        ESP_LOGE(TAG, "camera buffers alloc failed");
        return;
    }
    memset(cam_buf, 0, CAM_W * CAM_H * 2);
    memset(detect_buf, 0, CAM_W * CAM_H * 2);
    cam_img.header.cf = LV_IMG_CF_TRUE_COLOR;
    cam_img.header.w = CAM_W;
    cam_img.header.h = CAM_H;
    cam_img.data_size = CAM_W * CAM_H * 2;
    cam_img.data = (uint8_t *)cam_buf;

    if (lvgl_lock(-1)) {
        create_ui();
        lvgl_unlock();
    }

    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 1024 * 6, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(detect_task, "detect_task", 1024 * 10, NULL, 3, &detect_task_handle, 0);
    xTaskCreatePinnedToCore(camera_preview_task, "camera_preview", 1024 * 6, NULL, 4, NULL, 0);
}
