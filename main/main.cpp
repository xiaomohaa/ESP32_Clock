
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

#include "qmi8658.h"
#include "pcf85063a.h"
#include <math.h>
#include "lvgl_bsp.h"
#include "lvgl.h"
#include "power_bsp.h"

#define TAG "clock"
#define MADCTL_TEST_MODE 0

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
static lv_obj_t *brightness_label;
static volatile bool s_time_synced = false;
static volatile bool s_ntp_done = false;
static volatile float s_accel_x, s_accel_y, s_accel_z;
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

/* ---------- PCF85063A RTC ---------- */

static pcf85063a_dev_t s_rtc;

static void rtc_init_dev(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_err_t ret = pcf85063a_init(&s_rtc, user_i2cbus.Get_I2cBusHandle(), PCF85063A_ADDRESS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063A init failed (%d)", ret);
        return;
    }
    ESP_LOGI(TAG, "PCF85063A OK");

    /* read RTC time and set system clock */
    pcf85063a_datetime_t dt = {};
    pcf85063a_get_time_date(&s_rtc, &dt);
    if (dt.year > 2020) {
        struct tm ti = {};
        ti.tm_year = dt.year - 1900;
        ti.tm_mon  = dt.month - 1;
        ti.tm_mday = dt.day;
        ti.tm_hour = dt.hour;
        ti.tm_min  = dt.min;
        ti.tm_sec  = dt.sec;
        time_t t = mktime(&ti);
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "RTC time: %04d-%02d-%02d %02d:%02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
        s_time_synced = true;
    }
}

static void rtc_save_time(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    pcf85063a_datetime_t dt = {};
    dt.year  = ti.tm_year + 1900;
    dt.month = ti.tm_mon + 1;
    dt.day   = ti.tm_mday;
    dt.hour  = ti.tm_hour;
    dt.min   = ti.tm_min;
    dt.sec   = ti.tm_sec;
    pcf85063a_set_time_date(&s_rtc, dt);
    ESP_LOGI(TAG, "RTC saved: %04d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
}

/* ---------- SNTP ---------- */

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synced");
    s_time_synced = true;
    s_ntp_done = true;
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

    /* wait up to 30s for NTP sync */
    for (int i = 0; i < 30 && !s_ntp_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (s_ntp_done) {
        time_t now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);
        ESP_LOGI(TAG, "NTP time: %04d-%02d-%02d %02d:%02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        rtc_save_time();
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
                                   lv_coord_t w, const char *text,
                                   int shift, bool bold)
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

    /* draw twice with 1px offset for faux-bold */
    int passes = bold ? 2 : 1;
    for (int pass = 0; pass < passes; pass++) {
        lv_coord_t px = char_x + (bold ? pass : 0);
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
            lv_canvas_draw_text(canvas, px, 0, w, &dsc, ch);

            px += sz.x;
        }
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
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
        draw_rainbow_on_canvas(time_canvas, &lv_font_montserrat_48, TC_W, tbuf, ti.tm_sec, false);

        char sbuf[4];
        snprintf(sbuf, sizeof(sbuf), "%02d", ti.tm_sec);
        draw_rainbow_on_canvas(sec_canvas, &lv_font_montserrat_48, SC_W, sbuf, ti.tm_sec, false);

        char dbuf[40];
        snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        draw_rainbow_on_canvas(date_canvas, &lv_font_montserrat_22, DC_W, dbuf, ti.tm_sec, true);

        draw_rainbow_on_canvas(weekday_canvas, &lv_font_montserrat_16, WC_W,
                               weekday_en(ti.tm_wday), ti.tm_sec, true);
    } else {
        int total_sec = (int)(esp_timer_get_time() / 1000000);
        int sec = total_sec % 60;
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d",
                 total_sec / 3600, (total_sec % 3600) / 60);
        draw_rainbow_on_canvas(time_canvas, &lv_font_montserrat_48, TC_W, tbuf, sec, false);

        char sbuf[4];
        snprintf(sbuf, sizeof(sbuf), "%02d", sec);
        draw_rainbow_on_canvas(sec_canvas, &lv_font_montserrat_48, SC_W, sbuf, sec, false);

        draw_rainbow_on_canvas(date_canvas, &lv_font_montserrat_22, DC_W, "--", sec, true);
        draw_rainbow_on_canvas(weekday_canvas, &lv_font_montserrat_16, WC_W, "--", sec, true);
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
    /* brightness */
    lv_label_set_text_fmt(brightness_label, "B:%d", user_display->Get_Brightness());

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
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

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
    lv_obj_align(date_canvas, LV_ALIGN_CENTER, -60, 160);

    /* weekday: canvas 200x40, Montserrat 16, zoomed */
    weekday_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(weekday_canvas, wc_buf, WC_W, WC_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(weekday_canvas, lv_color_black(), LV_OPA_COVER);
    lv_img_set_zoom(weekday_canvas, 400);
    lv_img_set_pivot(weekday_canvas, 0, 0);
    lv_obj_align(weekday_canvas, LV_ALIGN_CENTER, -50, 200);

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

    /* brightness indicator (top-left) */
    brightness_label = lv_label_create(scr);
    lv_obj_set_style_text_font(brightness_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(brightness_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(brightness_label, "B:100");
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 5, 5);

    /* Timers */
    lv_timer_create(clock_update_cb, 1000, NULL);
    lv_timer_create(status_bar_update_cb, 10000, NULL);

    /* first update immediately */
    clock_update_cb(NULL);
    status_bar_update_cb(NULL);
}

/* ---------- auto-rotate via accelerometer ---------- */

static qmi8658_dev_t s_qmi8658;

/* MADCTL test: cycle through candidate values, print to serial */
#if MADCTL_TEST_MODE
static void madctl_test_task(void *arg)
{
    static const uint8_t candidates[] = {
        0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0xF0, 0x30
    };
    int n = sizeof(candidates) / sizeof(candidates[0]);
    int idx = 0;
    vTaskDelay(pdMS_TO_TICKS(3000)); /* wait for display ready */
    printf("=== MADCTL TEST START ===\n");
    while (1) {
        uint8_t val = candidates[idx];
        if (Lvgl_lock(1000) == ESP_OK) {
            user_display->Set_Madctl_Raw(val);
            Lvgl_unlock();
        }
        printf("MADCTL TEST: 0x%02X (%d/%d)\n", val, idx + 1, n);
        idx = (idx + 1) % n;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif

static const char *rot_name(int r)
{
    return (r == 0) ? "0-UP" : (r == 1) ? "1-RIGHT" : (r == 2) ? "2-DOWN" : "3-LEFT";
}

static void rotation_task(void *arg)
{
    esp_err_t ret = qmi8658_init(&s_qmi8658, user_i2cbus.Get_I2cBusHandle(), QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE("rot", "QMI8658 init FAILED (%d)", ret);
        vTaskDelete(NULL);
        return;
    }

    qmi8658_set_accel_range(&s_qmi8658, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(&s_qmi8658, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(&s_qmi8658, true);
    ESP_LOGI("rot", "QMI8658 OK, 8G 500Hz m/s²");

    int cur_rot = 0;
    int rot_count = 0;
    float sx = 0, sy = 0, sz = 0;
    int cnt = 0;
    int loop = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        loop++;

        /* heartbeat every 3s */
        if (loop % 30 == 0) {
            printf("HB %d rot=%d\n", loop, cur_rot);
        }

        /* raw I2C read with timeout */
        uint8_t reg = 0x35;
        uint8_t buf[6];
        esp_err_t err = i2c_master_transmit_receive(s_qmi8658.dev_handle, &reg, 1, buf, 6, 100);
        if (err != ESP_OK) {
            printf("I2C ERR=%d loop=%d\n", err, loop);
            continue;
        }

        int16_t raw_x = (int16_t)(buf[1] << 8 | buf[0]);
        int16_t raw_y = (int16_t)(buf[3] << 8 | buf[2]);
        int16_t raw_z = (int16_t)(buf[5] << 8 | buf[4]);

        float ax_raw = raw_x * 9.8f / 4096.0f;
        float ay_raw = raw_y * 9.8f / 4096.0f;
        float az_raw = raw_z * 9.8f / 4096.0f;

        sx = sx * 0.7f + ax_raw * 0.3f;
        sy = sy * 0.7f + ay_raw * 0.3f;
        sz = sz * 0.7f + az_raw * 0.3f;

        s_accel_x = sx;
        s_accel_y = sy;
        s_accel_z = sz;

        float ax = fabsf(sx), ay = fabsf(sy);
        int new_rot = cur_rot;
        const char *dir = "hold";

        if (ax > 6.87f && ax > ay) {
            new_rot = (sx > 0) ? 3 : 1;
            dir = (sx > 0) ? "R" : "L";
        } else if (ay > 6.87f && ay > ax) {
            new_rot = (sy > 0) ? 0 : 2;
            dir = (sy > 0) ? "UP" : "DN";
        }

        /* print every 1s */
        if (++cnt >= 10) {
            cnt = 0;
            printf("X:%.1f Y:%.1f Z:%.1f | %s %s rc=%d bl=%d\n",
                   sx / 9.8f, sy / 9.8f, sz / 9.8f,
                   rot_name(cur_rot), dir, rot_count,
                   user_display->Get_Brightness());
        }

        if (new_rot != cur_rot) {
            rot_count++;
            if (rot_count >= 3) {
                printf(">>> ROT %s->%s\n", rot_name(cur_rot), rot_name(new_rot));
                cur_rot = new_rot;
                rot_count = 0;
                if (Lvgl_lock(1000) == ESP_OK) {
                    user_display->Set_Rotate(cur_rot);
                    Lvgl_unlock();
                    printf("ROT DONE\n");
                } else {
                    printf("ROT LOCK FAIL\n");
                }
            }
        } else {
            rot_count = 0;
        }
    }
}

/* ---------- status report ---------- */

/* ---------- buttons ---------- */

#define BTN_POWER   GPIO_NUM_18
#define BTN_LEFT    GPIO_NUM_9
#define BTN_RIGHT   GPIO_NUM_10
#define BTN_LONG_PRESS_MS  2000

static void button_task(void *arg)
{
    int hold_ms = 0, pwr_release = 0;
    int left_hold = 0, right_hold = 0, left_release = 0, right_release = 0;
    int brightness = 100;
    ESP_LOGI(TAG, "button task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));

        int pwr = gpio_get_level(BTN_POWER);
        int btn_l = gpio_get_level(BTN_LEFT);
        int btn_r = gpio_get_level(BTN_RIGHT);

        /* power: long press 2s shutdown (debounce: need 3x release to reset) */
        if (pwr == 1) {
            pwr_release = 0;
            hold_ms += 50;
            if (hold_ms >= BTN_LONG_PRESS_MS) {
                ESP_LOGW(TAG, "power long press -> shutdown");
                if (Lvgl_lock(1000) == ESP_OK) {
                    lv_obj_t *scr = lv_scr_act();
                    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
                    Lvgl_unlock();
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                Axp2101_PowerOff();
            }
        } else {
            if (++pwr_release >= 3) {
                hold_ms = 0;
                pwr_release = 0;
            }
        }

        /* left: brightness down (active low) */
        if (btn_l == 0) {
            left_release = 0;
            left_hold += 50;
            if (left_hold >= 200) {
                left_hold = -300;
                brightness -= 10;
                if (brightness < 0) brightness = 0;
                user_display->Set_Backlight(brightness);
                if (Lvgl_lock(100) == ESP_OK) {
                    lv_label_set_text_fmt(brightness_label, "B:%d", brightness);
                    Lvgl_unlock();
                }
            }
        } else {
            if (++left_release >= 3) {
                left_hold = 0;
                left_release = 0;
            }
        }

        /* right: brightness up (active low) */
        if (btn_r == 0) {
            right_release = 0;
            right_hold += 50;
            if (right_hold >= 200) {
                right_hold = -300;
                brightness += 10;
                if (brightness > 100) brightness = 100;
                user_display->Set_Backlight(brightness);
                if (Lvgl_lock(100) == ESP_OK) {
                    lv_label_set_text_fmt(brightness_label, "B:%d", brightness);
                    Lvgl_unlock();
                }
            }
        } else {
            if (++right_release >= 3) {
                right_hold = 0;
                right_release = 0;
            }
        }
    }
}

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

    /* buttons: GPIO9=left, GPIO10=right, GPIO18=power */
    gpio_config_t btn_cfg = {};
    btn_cfg.pin_bit_mask = (1ULL << BTN_POWER) | (1ULL << BTN_LEFT) | (1ULL << BTN_RIGHT);
    btn_cfg.mode = GPIO_MODE_INPUT;
    btn_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&btn_cfg);

    /* hardware init */
    Custom_PmicPortInit(&user_i2cbus, 0x34);
    rtc_init_dev();
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

    /* auto-rotate from accelerometer */
    xTaskCreatePinnedToCore(rotation_task, "rotation", 8 * 1024, NULL, 3, NULL, 0);

#if MADCTL_TEST_MODE
    /* MADCTL test: cycles through candidate values every 3s */
    xTaskCreatePinnedToCore(madctl_test_task, "madctl_test", 4 * 1024, NULL, 4, NULL, 0);
#endif

    /* button monitor */
    xTaskCreatePinnedToCore(button_task, "button", 4 * 1024, NULL, 3, NULL, 0);
}
