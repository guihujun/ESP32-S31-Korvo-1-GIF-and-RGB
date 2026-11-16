/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_main.c
 * @brief ESP32-S31-Korvo GIF 动图显示 + RGB 灯控制示例
 *
 * 屏幕布局（800x480）：
 *   - 左侧 (0,0)-(399,479)：循环播放的 GIF 动图
 *   - 右侧 (400,0)-(799,479)：三个垂直等距排列的圆形按钮（红/绿/蓝）
 *
 * 按钮功能：
 *   - 按下红色按钮：板载 RGB 灯亮红色，松开后保持红色
 *   - 按下绿色按钮：板载 RGB 灯亮绿色，松开后保持绿色
 *   - 按下蓝色按钮：板载 RGB 灯亮蓝色，松开后保持蓝色
 *   - 按下瞬间按钮缩放至 90% 并变暗，提供触摸反馈
 *
 * 实现要点：
 *   - 显示/触摸初始化全部由官方 BSP (esp32_s31_korvo) 完成，无任何手写驱动
 *   - 仅一个 LVGL 任务：CPU0 / 优先级 4 / 栈 8192（由 esp_lvgl_adapter 创建）
 *   - RGB 面板帧缓冲位于 PSRAM（BSP 内部 fb_in_psram=true），
 *     GIF 解码帧缓冲由 LVGL 分配器路由到 PSRAM
 *   - GIF 数据以二进制数组形式存放在 Flash，运行时由 lv_gif 解码播放
 */

#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s31_korvo.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"

static const char *TAG = "gif_led_demo";

/* ==========================================================================
 * 布局常量
 * ========================================================================== */

#define UI_GIF_AREA_X        0
#define UI_GIF_AREA_Y        0
#define UI_GIF_AREA_W        (BSP_LCD_H_RES / 2)                                   /* 左侧半屏 400 */
#define UI_GIF_AREA_H        (BSP_LCD_V_RES)                                       /* 480 */

#define UI_BTN_DIAMETER      120                                                    /* 按钮直径 */
#define UI_BTN_CENTER_X      (UI_GIF_AREA_W + (BSP_LCD_H_RES - UI_GIF_AREA_W) / 2) /* 右侧区域水平居中 600 */
#define UI_BTN_CENTER_Y_R    100                                                    /* 顶部红色按钮圆心 Y */
#define UI_BTN_CENTER_Y_G    240                                                    /* 中间绿色按钮圆心 Y */
#define UI_BTN_CENTER_Y_B    380                                                    /* 底部蓝色按钮圆心 Y */
#define UI_BTN_BORDER_W      3                                                      /* 白色边框宽度 */

/* LVGL 缩放值单位：256 = 100% */
#define UI_SCALE_NORMAL      256
#define UI_SCALE_PRESSED     ((UI_SCALE_NORMAL * 90) / 100)                        /* 按下缩放至 90% */

#define UI_PRESS_ANIM_MS     150                                                    /* 按压缩放/变色过渡时长 */

/* ==========================================================================
 * LVGL 任务配置（要求：仅一个 LVGL 任务，绑定 CPU0，优先级 4，栈 8192）
 * ========================================================================== */

#define LVGL_TASK_STACK_SIZE 8192
#define LVGL_TASK_PRIORITY   4
#define LVGL_TASK_CORE_ID    0

/* LVGL 部分绘制缓冲区高度：BSP_LCD_H_RES(800) x 100 像素 */
#define UI_DISPLAY_BUFFER_HEIGHT 100

/* ==========================================================================
 * RGB LED 配置（板载 WS2812，通过官方 BSP 接口控制）
 * ========================================================================== */

#define LED_BRIGHTNESS_PERCENT 80
#define LED_VAL_MAX            255U
/* 80% 亮度对应的分量值：255 * 80% = 204，避免过亮 */
#define LED_VAL_BRIGHT         ((LED_VAL_MAX * LED_BRIGHTNESS_PERCENT) / 100U)

/* ==========================================================================
 * GIF 动图嵌入数据
 *
 * TODO: 替换为自己的 GIF 动图：
 *   1. 将你的 GIF 文件覆盖 main/sample_anim.gif（无需改动 CMakeLists.txt）
 *   2. 动图建议尺寸接近 400x480；若尺寸匹配，可删除下方 create_gif_area()
 *      中的 lv_image_set_scale() 调用
 *   3. 动图数据存放在 Flash（.rodata 段），解码后的帧缓冲由 LVGL
 *      自动分配到 PSRAM，无需手动拷贝
 * ========================================================================== */

/* 由 main/CMakeLists.txt 中 target_add_binary_data() 生成的链接符号 */
extern const uint8_t sample_anim_gif_start[] asm("_binary_sample_anim_gif_start");
/* 嵌入数据的字节长度（汇编 .long 常量，可安全用于静态初始化） */
extern const uint32_t sample_anim_gif_length asm("sample_anim_gif_length");

/** GIF 图像描述符：运行时初始化（data_size 为链接期符号，非编译期常量） */
static lv_image_dsc_t s_gif_img_dsc;

/**
 * @brief 初始化 GIF 图像描述符
 *
 * 描述符指向 Flash 中的嵌入动图数据，供 lv_gif_set_src() 使用。
 */
static void gif_image_dsc_init(void)
{
    s_gif_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;   /* 标记为 LVGL 变量型图像源 */
    s_gif_img_dsc.header.cf = LV_COLOR_FORMAT_RAW;        /* GIF 原始字节流 */
    s_gif_img_dsc.header.w = 0;                           /* 实际尺寸由 GIF 解码器解析 */
    s_gif_img_dsc.header.h = 0;
    s_gif_img_dsc.data_size = sample_anim_gif_length;
    s_gif_img_dsc.data = (const uint8_t *)sample_anim_gif_start;
}

/* ==========================================================================
 * 按钮定义
 * ========================================================================== */

/** 按钮配置：圆形底色 + 对应 RGB 灯分量（80% 亮度） */
typedef struct {
    lv_color_t color;       /* 按钮填充色 */
    uint8_t led_r;          /* LED 红色分量 */
    uint8_t led_g;          /* LED 绿色分量 */
    uint8_t led_b;          /* LED 蓝色分量 */
} ui_button_cfg_t;

/** 三个按钮：红 / 绿 / 蓝 */
static const ui_button_cfg_t s_button_cfgs[3] = {
    { .color = LV_COLOR_MAKE(0xFF, 0x00, 0x00), .led_r = LED_VAL_BRIGHT, .led_g = 0,                 .led_b = 0 },
    { .color = LV_COLOR_MAKE(0x00, 0xFF, 0x00), .led_r = 0,                 .led_g = LED_VAL_BRIGHT, .led_b = 0 },
    { .color = LV_COLOR_MAKE(0x00, 0x00, 0xFF), .led_r = 0,                 .led_g = 0,                 .led_b = LED_VAL_BRIGHT },
};

/** 按钮按压过渡动画样式（缩放 + 颜色过渡） */
static lv_style_t s_btn_press_trans;

/* ==========================================================================
 * RGB 灯控制
 * ========================================================================== */

/**
 * @brief 点亮板载 RGB 灯
 *
 * 先熄灭所有灯，再点亮目标颜色，避免颜色残留。
 * 全部通过官方 BSP 接口操作，不直接访问 GPIO 寄存器。
 *
 * @param[in] red   红色分量
 * @param[in] green 绿色分量
 * @param[in] blue  蓝色分量
 *
 * @return ESP_OK 成功，否则为 ESP-IDF 错误码
 */
static esp_err_t led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    esp_err_t ret;

    /* 先熄灭所有灯 */
    ret = bsp_led_clear();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 点亮对应颜色的灯 */
    return bsp_led_set_rgb(BSP_LED_STATUS, red, green, blue);
}

/* ==========================================================================
 * LVGL 事件回调
 * ========================================================================== */

/**
 * @brief 按钮按下事件回调：点亮对应颜色的 RGB 灯
 *
 * 按下即点亮，松开后保持（不做任何恢复操作）。
 *
 * @param[in] e LVGL 事件对象，user_data 为按钮索引
 */
static void button_pressed_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    esp_err_t ret;

    if (index < 0 || index >= (int)(sizeof(s_button_cfgs) / sizeof(s_button_cfgs[0]))) {
        return;
    }

    const ui_button_cfg_t *cfg = &s_button_cfgs[index];
    ret = led_set_color(cfg->led_r, cfg->led_g, cfg->led_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set LED color failed: %s", esp_err_to_name(ret));
    }
}

/* ==========================================================================
 * 界面创建（必须在持有 LVGL 锁时调用）
 * ========================================================================== */

/**
 * @brief 创建圆形控制按钮
 *
 * @param[in] parent   父对象
 * @param[in] index    按钮索引（对应 s_button_cfgs）
 * @param[in] center_y 按钮圆心 Y 坐标
 *
 * @return 按钮对象指针
 */
static lv_obj_t *create_color_button(lv_obj_t *parent, int index, lv_coord_t center_y)
{
    const ui_button_cfg_t *cfg = &s_button_cfgs[index];

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_BTN_DIAMETER, UI_BTN_DIAMETER);
    lv_obj_set_pos(btn, UI_BTN_CENTER_X - UI_BTN_DIAMETER / 2, center_y - UI_BTN_DIAMETER / 2);

    /* 圆形外观：纯色填充 + 白色边框 */
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, cfg->color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, UI_BTN_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_white(), LV_PART_MAIN);

    /* 缩放围绕圆心进行，否则会向左上角收缩 */
    lv_obj_set_style_transform_pivot_x(btn, UI_BTN_DIAMETER / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(btn, UI_BTN_DIAMETER / 2, LV_PART_MAIN);

    /* 按下状态：缩放至 90% + 颜色变暗（LV_STATE_PRESSED 由按钮部件自动维护） */
    lv_obj_set_style_transform_scale_x(btn, UI_SCALE_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(btn, UI_SCALE_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_darken(cfg->color, LV_OPA_60), LV_PART_MAIN | LV_STATE_PRESSED);

    /* 过渡动画：进入/离开按下状态时平滑过渡 */
    lv_obj_add_style(btn, &s_btn_press_trans, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_btn_press_trans, LV_STATE_DEFAULT);

    /* 按下时点亮对应颜色的 RGB 灯（事件回调方式，非轮询） */
    lv_obj_add_event_cb(btn, button_pressed_cb, LV_EVENT_PRESSED, (void *)(intptr_t)index);

    return btn;
}

/**
 * @brief 创建左侧 GIF 动图显示区域
 *
 * @param[in] parent 父对象
 *
 * @return 左侧面板对象指针
 */
static lv_obj_t *create_gif_area(lv_obj_t *parent)
{
    /* 左侧半屏黑色容器 */
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, UI_GIF_AREA_X, UI_GIF_AREA_Y);
    lv_obj_set_size(panel, UI_GIF_AREA_W, UI_GIF_AREA_H);
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(panel, false);    /* 禁用滚动，动图区域不需要滚动条 */

    /* GIF 动图部件：创建后自动无限循环播放（解码器播放到文件尾自动回卷） */
    lv_obj_t *gif = lv_gif_create(panel);
    lv_gif_set_src(gif, &s_gif_img_dsc);
    /* TODO: 示例 sample_anim.gif 仅为 60x80，放大 5 倍铺满左侧区域；
     * 换成自己的 400x480 动图后可删除本行 */
    lv_image_set_scale(gif, 5 * UI_SCALE_NORMAL);
    lv_obj_center(gif);                     /* 动图居中显示在左侧区域 */

    return panel;
}

/**
 * @brief 创建整个用户界面
 */
static void create_ui(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* 屏幕背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1B1B1F), LV_PART_MAIN);

    /* 左侧 GIF 动图区域 */
    create_gif_area(scr);

    /* 右侧三个垂直等距排列的圆形按钮：红 / 绿 / 蓝 */
    create_color_button(scr, 0, UI_BTN_CENTER_Y_R);
    create_color_button(scr, 1, UI_BTN_CENTER_Y_G);
    create_color_button(scr, 2, UI_BTN_CENTER_Y_B);
}

/* ==========================================================================
 * 应用入口
 * ========================================================================== */

void app_main(void)
{
    /* ---------- 1. 初始化板载 RGB 灯，初始状态为熄灭 ---------- */
    ESP_ERROR_CHECK(bsp_led_init());
    ESP_ERROR_CHECK(bsp_led_clear());

    /* ---------- 2. 初始化 LVGL 适配器（必须在 BSP 显示初始化之前） ----------
     * 提前调用 esp_lv_adapter_init() 指定 LVGL 任务参数；
     * bsp_display_start_with_config() 检测到适配器已初始化后会直接复用，
     * 从而保证全局只有一个 LVGL 任务，且绑定 CPU0 / 优先级 4 / 栈 8192。
     */
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = LVGL_TASK_STACK_SIZE;
    adapter_cfg.task_priority   = LVGL_TASK_PRIORITY;
    adapter_cfg.task_core_id    = LVGL_TASK_CORE_ID;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    /* ---------- 3. 官方 BSP 初始化显示与触摸 ----------
     * RGB 面板：帧缓冲由 BSP 自动分配到 PSRAM（fb_in_psram=true），
     * 刷新由 RGB 外设的 DMA 完成；tear_avoid_mode 默认 TRIPLE_PARTIAL
     * （三重部分刷新缓冲，比双缓冲更平滑，显式写出以表明意图）。
     */
    bsp_display_config_t display_cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    display_cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    display_cfg.buffer_height   = UI_DISPLAY_BUFFER_HEIGHT;      /* 800 x 100 像素 */
    display_cfg.task_stack_size = LVGL_TASK_STACK_SIZE;
    lv_display_t *disp = bsp_display_start_with_config(&display_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "BSP display start failed");
        return;
    }

    /* ---------- 4. 初始化 GIF 图像描述符与按钮过渡动画样式 ---------- */
    gif_image_dsc_init();
    static const lv_style_prop_t press_trans_props[] = {
        LV_STYLE_TRANSFORM_SCALE_X,
        LV_STYLE_TRANSFORM_SCALE_Y,
        LV_STYLE_BG_COLOR,
        LV_STYLE_PROP_INV,
    };
    lv_style_transition_dsc_t trans_dsc = {
        .props = press_trans_props,
        .time = UI_PRESS_ANIM_MS,
        .path_xcb = lv_anim_path_ease_out,
    };
    lv_style_init(&s_btn_press_trans);
    lv_style_set_transition(&s_btn_press_trans, &trans_dsc);

    /* ---------- 5. 创建界面（在 LVGL 锁保护下进行） ---------- */
    if (bsp_display_lock(-1)) {
        create_ui();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "acquire LVGL lock failed");
        return;
    }

    ESP_LOGI(TAG, "gif_led_demo is ready");

    /* ---------- 6. 主循环 ----------
     * 界面渲染、GIF 播放与触摸处理全部由 LVGL 任务维护，
     * 此处仅保持主任务存活。
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
