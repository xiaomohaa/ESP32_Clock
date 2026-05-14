
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <nvs_flash.h>

#include "lvgl_bsp.h"
#include "lvgl.h"
#include "power_bsp.h"

#define TAG "clock"

/* rainbow colors for time digits */
static const char *s_rainbow_hex[] = {
    "ff0000","ff8000","ffff00","00ff00",
    "00ffff","0080ff","8000ff","ff00ff","ff0080",
};
#define RAINBOW_N 9

/* WiFi networks - try in order */
typedef struct {
    const char *ssid;
    const char *password;
} wifi_credential_t;

static const wifi_credential_t s_wifi_list[] = {
    { "GX_T",   "ap119119" },
    { "LAB721", "lab721721" },
};
#define WIFI_LIST_SIZE (sizeof(s_wifi_list) / sizeof(s_wifi_list[0]))

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static int s_current_wifi_idx = 0;
static int s_retry_count = 0;
#define WIFI_MAX_RETRY_PER_NET  3

I2cMasterBus user_i2cbus(7, 8, 0);
DisplayPort *user_display = NULL;

/* time canvas: 240x80, rendered at 48px, zoomed for large display */
#define TC_W  240
#define TC_H  80
static lv_color_t tc_buf[TC_W * TC_H];
static lv_obj_t *time_canvas;

/* seconds canvas: 80x80 */
#define SC_W  80
#define SC_H  80
static lv_color_t sc_buf[SC_W * SC_H];
static lv_obj_t *sec_canvas;

/* date canvas: 240x50 */
#define DC_W  240
#define DC_H  50
static lv_color_t dc_buf[DC_W * DC_H];
static lv_obj_t *date_canvas;

/* weekday canvas: 200x40 */
#define WC_W  200
#define WC_H  40
static lv_color_t wc_buf[WC_W * WC_H];
static lv_obj_t *weekday_canvas;

static lv_obj_t *wifi_label;
static lv_obj_t *battery_canvas;
static lv_obj_t *battery_pct_label;
static lv_obj_t *battery_pct_shadow;
static volatile bool s_time_synced = false;
static lv_anim_t s_wifi_anim;

/* battery canvas: 44x22 px, rounded style */
#define BAT_W  44
#define BAT_H  22
#define BAT_NUB_W 4
#define BAT_BORDER 2
#define BAT_RADIUS 5
static lv_color_t bat_canvas_buf[BAT_W * BAT_H];

/* ---------- WiFi ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY_PER_NET) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retry WiFi [%s] (%d/%d)",
                     s_wifi_list[s_current_wifi_idx].ssid,
                     s_retry_count, WIFI_MAX_RETRY_PER_NET);
        } else {
            /* try next network */
            s_current_wifi_idx++;
            if (s_current_wifi_idx < (int)WIFI_LIST_SIZE) {
                s_retry_count = 0;
                wifi_config_t cfg = {};
                strlcpy((char *)cfg.sta.ssid,
                        s_wifi_list[s_current_wifi_idx].ssid,
                        sizeof(cfg.sta.ssid));
                strlcpy((char *)cfg.sta.password,
                        s_wifi_list[s_current_wifi_idx].password,
                        sizeof(cfg.sta.password));
                cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
                ESP_LOGI(TAG, "trying next WiFi: %s",
                         s_wifi_list[s_current_wifi_idx].ssid);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "all WiFi networks failed");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected: %s  IP: " IPSTR,
                 s_wifi_list[s_current_wifi_idx].ssid,
                 IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_and_connect(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id, got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));

    /* start with first network */
    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid,
            s_wifi_list[0].ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password,
            s_wifi_list[0].password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init done, connecting to: %s", s_wifi_list[0].ssid);
}

/* ---------- SNTP ---------- */

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synced");
    s_time_synced = true;
}

static void sntp_init_and_wait(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    /* wait up to 30s for sync */
    for (int i = 0; i < 30 && !s_time_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (s_time_synced) {
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        ESP_LOGI(TAG, "time: %04d-%02d-%02d %02d:%02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        ESP_LOGW(TAG, "NTP sync timeout, using uptime");
    }
}

/* background task: WiFi -> NTP -> done */
static void time_sync_task(void *arg)
{
    wifi_init_and_connect();

    /* wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        sntp_init_and_wait();
    } else {
        ESP_LOGW(TAG, "WiFi not available, clock shows uptime");
    }

    vTaskDelete(NULL);
}

/* ---------- LVGL clock UI ---------- */

static const char *weekday_en(int wday)
{
    static const char *names[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    return (wday >= 0 && wday <= 6) ? names[wday] : "???";
}

static void draw_rainbow_on_canvas(lv_obj_t *canvas, const lv_font_t *font,
                                   lv_coord_t w, const char *text, int shift)
{
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    int len = strlen(text);

    /* measure total width */
    lv_coord_t total_w = 0;
    for (int i = 0; i < len; i++) {
        char ch[2] = { text[i], '\0' };
        lv_point_t sz;
        lv_txt_get_size(&sz, ch, font, 0, 0, w, LV_TEXT_FLAG_NONE);
        total_w += sz.x;
    }

    /* center horizontally */
    lv_coord_t char_x = (w - total_w) / 2;

    for (int i = 0; i < len; i++) {
        int ci = (i + shift) % RAINBOW_N;
        lv_color_t color = lv_color_hex(
            strtol(s_rainbow_hex[ci], NULL, 16));

        char ch[2] = { text[i], '\0' };
        lv_point_t sz;
        lv_txt_get_size(&sz, ch, font, 0, 0, w, LV_TEXT_FLAG_NONE);

        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.color = color;
        dsc.opa   = LV_OPA_COVER;
        dsc.font  = font;
        lv_canvas_draw_text(canvas, char_x, 0, w, &dsc, ch);

        char_x += sz.x;
    }
}

static void clock_update_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (s_time_synced) {
        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%02d : %02d", ti.tm_hour, ti.tm_min);
        draw_rainbow_on_canvas(time_canvas, &lv_font_montserrat_48, TC_W, tbuf, ti.tm_sec);

        char sbuf[4];
        snprintf(sbuf, sizeof(sbuf), "%02d", ti.tm_sec);
        draw_rainbow_on_canvas(sec_canvas, &lv_font_montserrat_48, SC_W, sbuf, ti.tm_sec);

        char dbuf[40];
        snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        draw_rainbow_on_canvas(date_canvas, &lv_font_montserrat_22, DC_W, dbuf, ti.tm_sec);

        draw_rainbow_on_canvas(weekday_canvas, &lv_font_montserrat_16, WC_W,
                               weekday_en(ti.tm_wday), ti.tm_sec);
    } else {
        int total_sec = (int)(esp_timer_get_time() / 1000000);
        int sec = total_sec % 60;
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d",
                 total_sec / 3600, (total_sec % 3600) / 60);
        draw_rainbow_on_canvas(time_canvas, &lv_font_montserrat_48, TC_W, tbuf, sec);

        char sbuf[4];
        snprintf(sbuf, sizeof(sbuf), "%02d", sec);
        draw_rainbow_on_canvas(sec_canvas, &lv_font_montserrat_48, SC_W, sbuf, sec);

        draw_rainbow_on_canvas(date_canvas, &lv_font_montserrat_22, DC_W, "--", sec);
        draw_rainbow_on_canvas(weekday_canvas, &lv_font_montserrat_16, WC_W, "--", sec);
    }

    lv_obj_invalidate(time_canvas);
    lv_obj_invalidate(sec_canvas);
    lv_obj_invalidate(date_canvas);
    lv_obj_invalidate(weekday_canvas);
}

static void battery_canvas_redraw(int pct)
{
    lv_color_t white = lv_color_white();
    lv_color_t red   = lv_color_make(0xFF, 0x00, 0x00);
    lv_color_t black = lv_color_black();

    lv_canvas_fill_bg(battery_canvas, black, LV_OPA_COVER);

    int body_w = BAT_W - BAT_NUB_W;
    int inner_x = BAT_BORDER;
    int inner_y = BAT_BORDER;
    int inner_w = body_w - BAT_BORDER * 2;
    int inner_h = BAT_H - BAT_BORDER * 2;

    /* battery body: rounded outline */
    lv_draw_rect_dsc_t border_dsc;
    lv_draw_rect_dsc_init(&border_dsc);
    border_dsc.bg_opa      = LV_OPA_TRANSP;
    border_dsc.border_color = white;
    border_dsc.border_width = BAT_BORDER;
    border_dsc.border_opa   = LV_OPA_COVER;
    border_dsc.radius       = BAT_RADIUS;
    lv_canvas_draw_rect(battery_canvas, 0, 0, body_w, BAT_H, &border_dsc);

    /* terminal nub (right side, rounded) */
    lv_draw_rect_dsc_t nub_dsc;
    lv_draw_rect_dsc_init(&nub_dsc);
    nub_dsc.bg_color  = white;
    nub_dsc.bg_opa    = LV_OPA_COVER;
    nub_dsc.border_opa = LV_OPA_TRANSP;
    nub_dsc.radius    = 2;
    int nub_h = BAT_H / 3;
    int nub_y = (BAT_H - nub_h) / 2;
    lv_canvas_draw_rect(battery_canvas, body_w, nub_y, BAT_NUB_W, nub_h, &nub_dsc);

    /* fill bar: rounded, white when >= 20%, red when < 20% */
    if (pct > 0) {
        int fill_w = (inner_w * pct) / 100;
        if (fill_w < 1) fill_w = 1;
        lv_draw_rect_dsc_t fill_dsc;
        lv_draw_rect_dsc_init(&fill_dsc);
        fill_dsc.bg_color  = (pct >= 20) ? white : red;
        fill_dsc.bg_opa    = LV_OPA_COVER;
        fill_dsc.border_opa = LV_OPA_TRANSP;
        fill_dsc.radius    = (fill_w >= inner_w) ? BAT_RADIUS - 1 : 2;
        lv_canvas_draw_rect(battery_canvas, inner_x, inner_y, fill_w, inner_h, &fill_dsc);
    }

    /* charging: green lightning bolt */
    if (Axp2101_IsCharging() && pct >= 0) {
        lv_color_t green = lv_color_make(0x00, 0xFF, 0x00);
        lv_draw_label_dsc_t lbl_dsc;
        lv_draw_label_dsc_init(&lbl_dsc);
        lbl_dsc.color = green;
        lv_canvas_draw_text(battery_canvas,
            body_w / 2 - 5, BAT_H / 2 - 7, 16, &lbl_dsc, LV_SYMBOL_CHARGE);
    }
}

static void wifi_blink_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void wifi_anim_ready_cb(lv_anim_t *a)
{
    /* restart if still not connected */
    if (!s_time_synced) {
        lv_anim_start(&s_wifi_anim);
    }
}

static void status_bar_update_cb(lv_timer_t *timer)
{
    /* WiFi icon: solid when connected, blinking when not */
    if (s_time_synced) {
        lv_anim_del(wifi_label, NULL);
        lv_obj_set_style_opa(wifi_label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(wifi_label, lv_color_white(), 0);
    } else {
        if (lv_anim_get(wifi_label, NULL) == NULL) {
            lv_anim_start(&s_wifi_anim);
        }
    }

    /* Battery canvas + percentage (with bold shadow) */
    int pct = Axp2101_GetBatteryPercent();
    battery_canvas_redraw(pct >= 0 ? pct : 0);

    if (pct >= 0) {
        lv_label_set_text_fmt(battery_pct_label, "%d%%", pct);
        lv_label_set_text_fmt(battery_pct_shadow, "%d%%", pct);
    } else {
        lv_label_set_text(battery_pct_label, "--");
        lv_label_set_text(battery_pct_shadow, "--");
    }

    /* dynamic layout: battery left of percentage, WiFi left of battery */
    lv_obj_align_to(battery_pct_shadow, battery_pct_label, LV_ALIGN_TOP_LEFT, -1, -1);
    lv_obj_align_to(battery_canvas, battery_pct_label, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_align_to(wifi_label, battery_canvas, LV_ALIGN_OUT_LEFT_MID, -10, 0);
}

static void clock_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(0x10, 0x10, 0x10), 0);

    /* time: canvas 240x80 rendered at 48px, zoomed for large display */
    time_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(time_canvas, tc_buf, TC_W, TC_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(time_canvas, lv_color_black(), LV_OPA_COVER);
    lv_img_set_zoom(time_canvas, 660);
    lv_img_set_pivot(time_canvas, 0, 0);
    lv_obj_align(time_canvas, LV_ALIGN_CENTER, -240, -70);

    /* seconds: canvas 80x80, Montserrat 48, zoomed to match time height */
    sec_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(sec_canvas, sc_buf, SC_W, SC_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(sec_canvas, lv_color_black(), LV_OPA_COVER);
    lv_img_set_zoom(sec_canvas, 350);
    lv_img_set_pivot(sec_canvas, 0, 0);
    lv_obj_align(sec_canvas, LV_ALIGN_CENTER, 180, -20);

    /* date: canvas 240x50, Montserrat 22, zoomed */
    date_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(date_canvas, dc_buf, DC_W, DC_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(date_canvas, lv_color_black(), LV_OPA_COVER);
    lv_img_set_zoom(date_canvas, 400);
    lv_img_set_pivot(date_canvas, 0, 0);
    lv_obj_align(date_canvas, LV_ALIGN_CENTER, -60, 150);

    /* weekday: canvas 200x40, Montserrat 16, zoomed */
    weekday_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(weekday_canvas, wc_buf, WC_W, WC_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(weekday_canvas, lv_color_black(), LV_OPA_COVER);
    lv_img_set_zoom(weekday_canvas, 400);
    lv_img_set_pivot(weekday_canvas, 0, 0);
    lv_obj_align(weekday_canvas, LV_ALIGN_CENTER, -60, 180);

    /* Right side: percentage | battery | WiFi (right to left) */
    /* Battery percentage (fixed right margin, Montserrat 22, faux-bold) */
    battery_pct_shadow = lv_label_create(scr);
    lv_obj_set_style_text_font(battery_pct_shadow, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(battery_pct_shadow, lv_color_white(), 0);
    lv_label_set_text(battery_pct_shadow, "");
    lv_obj_align(battery_pct_shadow, LV_ALIGN_TOP_RIGHT, -34, 9);

    battery_pct_label = lv_label_create(scr);
    lv_obj_set_style_text_font(battery_pct_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(battery_pct_label, lv_color_white(), 0);
    lv_label_set_text(battery_pct_label, "");
    lv_obj_align(battery_pct_label, LV_ALIGN_TOP_RIGHT, -35, 8);

    /* Battery canvas (positioned dynamically relative to percentage) */
    battery_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(battery_canvas, bat_canvas_buf, BAT_W, BAT_H, LV_IMG_CF_TRUE_COLOR);

    /* WiFi icon (positioned dynamically relative to battery canvas) */
    wifi_label = lv_label_create(scr);
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(wifi_label, lv_color_white(), 0);
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);

    /* setup WiFi blink animation (255→0→255, 800ms cycle) */
    lv_anim_init(&s_wifi_anim);
    lv_anim_set_var(&s_wifi_anim, wifi_label);
    lv_anim_set_values(&s_wifi_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&s_wifi_anim, 500);
    lv_anim_set_playback_time(&s_wifi_anim, 500);
    lv_anim_set_repeat_count(&s_wifi_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_ready_cb(&s_wifi_anim, wifi_anim_ready_cb);
    lv_anim_set_exec_cb(&s_wifi_anim, (lv_anim_exec_xcb_t)wifi_blink_cb);

    /* Timers */
    lv_timer_create(clock_update_cb, 1000, NULL);
    lv_timer_create(status_bar_update_cb, 10000, NULL);

    /* first update immediately */
    clock_update_cb(NULL);
    status_bar_update_cb(NULL);
}

/* ---------- status report ---------- */

static void status_report_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        ESP_LOGI(TAG, "synced=%d time=%02d:%02d:%02d heap=%lu",
                 s_time_synced,
                 ti.tm_hour, ti.tm_min, ti.tm_sec,
                 (unsigned long)esp_get_free_heap_size());
    }
}

/* ---------- main ---------- */

extern "C" void app_main(void)
{
    /* NVS required for WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "boot OK");

    /* hardware init */
    Custom_PmicPortInit(&user_i2cbus, 0x34);
    user_display = new DisplayPort(user_i2cbus, 480, 480);
    user_display->DisplayPort_TouchInit();
    Lvgl_PortInit(*user_display);

    /* clock UI */
    if (Lvgl_lock(-1) == ESP_OK) {
        clock_ui_init();
        Lvgl_unlock();
    }

    /* WiFi + NTP in background */
    xTaskCreatePinnedToCore(time_sync_task, "time_sync", 6 * 1024, NULL, 5, NULL, 0);

    /* periodic status */
    xTaskCreatePinnedToCore(status_report_task, "status", 2 * 1024, NULL, 2, NULL, 0);
}
