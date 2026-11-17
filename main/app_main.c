/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_main.c
 * @brief ESP32-S31-Korvo GIF 动图轮播 + RGB 滑条调色示例
 *
 * 屏幕布局（800x480）：
 *   - 左侧 (0,0)-(399,429)：4 个 GIF 动图循环播放，可切换
 *   - 左侧底部 (0,430)-(399,479)：◀ ▶ 两个箭头按钮切换动图
 *   - 右侧 (400,0)-(799,479)：LED 颜色预览圆 + R/G/B 三个颜色滑条
 *
 * 交互方式：
 *   - 触摸箭头 ◀ ▶：切换上一个/下一个动图
 *   - 板载按键 SET：下一个动图；MODE：上一个动图
 *   - 拖动 R/G/B 滑条：实时自定义板载 RGB 灯颜色（初始全灭）
 *
 * 实现要点：
 *   - 显示/触摸/按键初始化全部由官方 BSP (esp32_s31_korvo) 完成，无手写驱动
 *   - 仅一个 LVGL 任务：CPU0 / 优先级 4 / 栈 8192（由 esp_lvgl_adapter 创建）
 *   - RGB 面板帧缓冲位于 PSRAM（BSP 内部 fb_in_psram=true），
 *     GIF 解码帧缓冲由 LVGL 分配器路由到 PSRAM
 *   - 4 个 GIF 数据以二进制数组形式存放在 Flash，运行时由 lv_gif 解码播放
 */

#include <stdint.h>
#include <inttypes.h>
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
#define UI_GIF_AREA_W        (BSP_LCD_H_RES / 2)          /* 左侧半屏 400 */
#define UI_GIF_VIEW_H        430                           /* 动图可视高度（底部留 50 给箭头） */

#define UI_ARROW_D           56                            /* 箭头按钮直径 */
#define UI_ARROW_Y           436                           /* 箭头按钮 Y */
#define UI_ARROW_L_X         140                           /* ◀ 箭头 X */
#define UI_ARROW_R_X         260                           /* ▶ 箭头 X */

#define UI_RIGHT_X           400
#define UI_RIGHT_W           (BSP_LCD_H_RES - UI_RIGHT_X)  /* 右侧区域宽 400 */

#define UI_PREVIEW_D         90                            /* LED 颜色预览圆直径 */
#define UI_PREVIEW_Y         15
#define UI_SLIDER_W          360                           /* 颜色滑条宽 */
#define UI_SLIDER_H          20                            /* 颜色滑条高 */
#define UI_SLIDER_X          (UI_RIGHT_X + (UI_RIGHT_W - UI_SLIDER_W) / 2)  /* 420 */
#define UI_SLIDER_Y_R        125                           /* R 滑条 Y */
#define UI_SLIDER_Y_G        235                           /* G 滑条 Y */
#define UI_SLIDER_Y_B        345                           /* B 滑条 Y */
#define UI_COLOR_MAX         255                           /* 颜色分量最大值 */

/* LVGL 缩放值单位：256 = 100%，小动图最多放大 4 倍 */
#define UI_GIF_SCALE_MAX     (4 * 256)

/* ==========================================================================
 * LVGL 任务配置（要求：仅一个 LVGL 任务，绑定 CPU0，优先级 4，栈 8192）
 * ========================================================================== */

#define LVGL_TASK_STACK_SIZE 8192
#define LVGL_TASK_PRIORITY   4
#define LVGL_TASK_CORE_ID    0

/* LVGL 部分绘制缓冲区高度：BSP_LCD_H_RES(800) x 100 像素 */
#define UI_DISPLAY_BUFFER_HEIGHT 100

/* ==========================================================================
 * GIF 动图资产（4 个文件存放于项目根目录 GIF/ 文件夹）
 *
 * TODO: 添加/替换自己的 GIF 动图：
 *   1. 将动图文件放入 GIF/ 文件夹，文件名与下方 target_add_binary_data
 *      （main/CMakeLists.txt）中的名称保持一致
 *   2. 动图数据存放在 Flash（.rodata 段），解码后的帧缓冲由 LVGL
 *      自动分配到 PSRAM，无需手动拷贝
 * ========================================================================== */

#define GIF_COUNT 4

/* 由 main/CMakeLists.txt 中 target_add_binary_data() 生成的链接符号 */
#define GIF_ASSET_DECLARE(name) \
    extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
    extern const uint32_t name##_length asm(#name "_length");

GIF_ASSET_DECLARE(cat_gif)
GIF_ASSET_DECLARE(ganyu_gif)
GIF_ASSET_DECLARE(qingwa_gif)
GIF_ASSET_DECLARE(sanyu_gif)

/** GIF 资产描述：图像描述符 + 画布尺寸 */
typedef struct {
    const char *name;           /* 动图文件名（用于日志） */
    lv_image_dsc_t dsc;         /* LVGL 图像描述符 */
    uint16_t gif_w;             /* 动图画布宽 */
    uint16_t gif_h;             /* 动图画布高 */
} gif_asset_t;

static gif_asset_t s_gif_assets[GIF_COUNT];
static int s_gif_index = 0;                 /* 当前播放的动图索引 */
static lv_obj_t *s_gif_obj;                 /* lv_gif 部件对象 */
static lv_obj_t *s_led_preview;             /* LED 颜色预览圆 */
static lv_obj_t *s_sliders[3];              /* R/G/B 三个滑条对象 */

/* ==========================================================================
 * 初始化函数
 * ========================================================================== */

/**
 * @brief 初始化单个 GIF 资产
 *
 * 填充图像描述符并解析 GIF 头部中的画布尺寸。
 * 描述符必须在运行时初始化（data_size 为链接期符号，非编译期常量）。
 *
 * @param[out] asset 资产对象
 * @param[in]  name  文件名（用于日志）
 * @param[in]  data  Flash 中的动图数据指针
 * @param[in]  size  数据字节数
 */
static void gif_asset_init(gif_asset_t *asset, const char *name, const uint8_t *data, uint32_t size)
{
    asset->name = name;
    asset->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;    /* 标记为 LVGL 变量型图像源 */
    asset->dsc.header.cf = LV_COLOR_FORMAT_RAW;         /* GIF 原始字节流 */
    asset->dsc.header.w = 0;                            /* 实际尺寸由 GIF 解码器解析 */
    asset->dsc.header.h = 0;
    asset->dsc.data_size = size;
    asset->dsc.data = data;
    /* GIF 逻辑屏幕尺寸位于文件头字节 6~9（小端序） */
    asset->gif_w = (uint16_t)(data[6] | (data[7] << 8));
    asset->gif_h = (uint16_t)(data[8] | (data[9] << 8));
}

/**
 * @brief 初始化全部 4 个 GIF 资产
 */
static void gif_assets_init(void)
{
    gif_asset_init(&s_gif_assets[0], "cat.gif",    cat_gif_start,    cat_gif_length);
    gif_asset_init(&s_gif_assets[1], "ganyu.gif",  ganyu_gif_start,  ganyu_gif_length);
    gif_asset_init(&s_gif_assets[2], "qingwa.gif", qingwa_gif_start, qingwa_gif_length);
    gif_asset_init(&s_gif_assets[3], "sanyu.gif",  sanyu_gif_start,  sanyu_gif_length);
}

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

    /* 点亮目标颜色 */
    return bsp_led_set_rgb(BSP_LED_STATUS, red, green, blue);
}

/* ==========================================================================
 * GIF 动图切换
 * ========================================================================== */

/**
 * @brief 切换到指定动图并播放（必须在持有 LVGL 锁时调用）
 *
 * 索引自动循环回绕，动图按画布比例缩放居中显示在左侧区域。
 *
 * @param[in] index 目标动图索引（任意整数，自动回绕）
 */
static void gif_show(int index)
{
    const gif_asset_t *asset;
    uint32_t scale;

    /* 索引循环回绕 */
    index = (index + GIF_COUNT) % GIF_COUNT;
    s_gif_index = index;
    asset = &s_gif_assets[index];

    /* 切换动图源：lv_gif 自动关闭旧源并打开新源，无限循环播放 */
    lv_gif_set_src(s_gif_obj, &asset->dsc);

    /* 按画布比例计算缩放值（256 = 100%），上限 4 倍避免小图过度放大 */
    scale = 256U * UI_GIF_VIEW_H / asset->gif_h;
    if (scale > 256U * UI_GIF_AREA_W / asset->gif_w) {
        scale = 256U * UI_GIF_AREA_W / asset->gif_w;
    }
    if (scale > UI_GIF_SCALE_MAX) {
        scale = UI_GIF_SCALE_MAX;
    }
    lv_image_set_scale(s_gif_obj, scale);

    /* 缩放围绕画布中心进行，避免视觉偏移 */
    lv_obj_set_style_transform_pivot_x(s_gif_obj, asset->gif_w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_gif_obj, asset->gif_h / 2, LV_PART_MAIN);

    /* 动图居中显示在左侧区域 */
    lv_obj_center(s_gif_obj);

    ESP_LOGI(TAG, "gif [%d/%d]: %s (%ux%u, scale=%" PRIu32 "/256)",
             s_gif_index + 1, GIF_COUNT, asset->name, asset->gif_w, asset->gif_h, scale);
}

/**
 * @brief 板载按键回调：SET 下一个动图，MODE 上一个动图
 *
 * 按键回调运行在按键任务上下文，操作 LVGL 对象前必须加锁。
 *
 * @param[in] button_handle iot_button 句柄（未使用）
 * @param[in] user_data     按键枚举值 (bsp_button_t)
 */
static void hw_button_cb(void *button_handle, void *user_data)
{
    bsp_button_t button = (bsp_button_t)(intptr_t)user_data;

    /* 非阻塞加锁：LVGL 繁忙时跳过本次按键 */
    if (!bsp_display_lock(0)) {
        return;
    }

    if (button == BSP_BUTTON_SET) {
        gif_show(s_gif_index + 1);      /* SET：下一个动图 */
    } else if (button == BSP_BUTTON_MODE) {
        gif_show(s_gif_index - 1);      /* MODE：上一个动图 */
    }

    bsp_display_unlock();
}

/* ==========================================================================
 * LVGL 事件回调
 * ========================================================================== */

/**
 * @brief 左箭头点击回调：切换到上一个动图
 *
 * @param[in] e LVGL 事件对象（未使用）
 */
static void arrow_left_cb(lv_event_t *e)
{
    gif_show(s_gif_index - 1);
}

/**
 * @brief 右箭头点击回调：切换到下一个动图
 *
 * @param[in] e LVGL 事件对象（未使用）
 */
static void arrow_right_cb(lv_event_t *e)
{
    gif_show(s_gif_index + 1);
}

/**
 * @brief 颜色滑条值变化回调：实时更新板载 RGB 灯与预览颜色
 *
 * @param[in] e LVGL 事件对象（未使用）
 */
static void slider_changed_cb(lv_event_t *e)
{
    uint8_t rgb[3];
    esp_err_t ret;

    /* 读取三个滑条当前值作为 R/G/B 分量 */
    for (int i = 0; i < 3; i++) {
        rgb[i] = (uint8_t)lv_slider_get_value(s_sliders[i]);
    }

    ret = led_set_color(rgb[0], rgb[1], rgb[2]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set LED color failed: %s", esp_err_to_name(ret));
    }

    /* 同步更新预览圆颜色 */
    lv_obj_set_style_bg_color(s_led_preview, lv_color_make(rgb[0], rgb[1], rgb[2]), LV_PART_MAIN);
}

/* ==========================================================================
 * 界面创建（必须在持有 LVGL 锁时调用）
 * ========================================================================== */

/**
 * @brief 创建左侧 GIF 动图显示区域与箭头按钮
 *
 * @param[in] parent 父对象
 */
static void create_gif_area(lv_obj_t *parent)
{
    /* 左侧黑色容器（底部留出箭头按钮空间） */
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, UI_GIF_AREA_X, UI_GIF_AREA_Y);
    lv_obj_set_size(panel, UI_GIF_AREA_W, UI_GIF_VIEW_H);
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(panel, false);    /* 禁用滚动，动图区域不需要滚动条 */

    /* GIF 动图部件：初始播放第 1 个动图 */
    s_gif_obj = lv_gif_create(panel);
    gif_show(0);

    /* 左侧区域底部的 ◀ ▶ 箭头按钮 */
    lv_obj_t *btn_prev = lv_button_create(parent);
    lv_obj_set_size(btn_prev, UI_ARROW_D, UI_ARROW_D);
    lv_obj_set_pos(btn_prev, UI_ARROW_L_X, UI_ARROW_Y);
    lv_obj_set_style_radius(btn_prev, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x3A3A44), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x5C5C6A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_prev, arrow_left_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_prev = lv_label_create(btn_prev);
    lv_label_set_text(label_prev, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(label_prev, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label_prev);

    lv_obj_t *btn_next = lv_button_create(parent);
    lv_obj_set_size(btn_next, UI_ARROW_D, UI_ARROW_D);
    lv_obj_set_pos(btn_next, UI_ARROW_R_X, UI_ARROW_Y);
    lv_obj_set_style_radius(btn_next, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x3A3A44), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x5C5C6A), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_next, arrow_right_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_next = lv_label_create(btn_next);
    lv_label_set_text(label_next, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(label_next, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label_next);
}

/**
 * @brief 创建 LED 颜色预览圆
 *
 * @param[in] parent 父对象
 */
static void create_led_preview(lv_obj_t *parent)
{
    s_led_preview = lv_obj_create(parent);
    lv_obj_set_size(s_led_preview, UI_PREVIEW_D, UI_PREVIEW_D);
    lv_obj_set_pos(s_led_preview, UI_RIGHT_X + (UI_RIGHT_W - UI_PREVIEW_D) / 2, UI_PREVIEW_Y);
    lv_obj_set_style_radius(s_led_preview, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_led_preview, lv_color_black(), LV_PART_MAIN);   /* 初始：灯灭 */
    lv_obj_set_style_bg_opa(s_led_preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_led_preview, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_led_preview, lv_color_white(), LV_PART_MAIN);
}

/**
 * @brief 创建单个颜色滑条
 *
 * @param[in] parent        父对象
 * @param[in] index         通道索引（0=R, 1=G, 2=B，对应 s_sliders）
 * @param[in] y             滑条 Y 坐标
 * @param[in] channel_color 该通道的纯色
 */
static void create_rgb_slider(lv_obj_t *parent, int index, lv_coord_t y, lv_color_t channel_color)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, UI_SLIDER_X, y);
    lv_obj_set_size(slider, UI_SLIDER_W, UI_SLIDER_H);
    lv_slider_set_range(slider, 0, UI_COLOR_MAX);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);            /* 初始全灭 */

    /* 滑轨底色：对应通道颜色的暗色 */
    lv_obj_set_style_bg_color(slider, lv_color_darken(channel_color, LV_OPA_80), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    /* 已填充部分：通道纯色 */
    lv_obj_set_style_bg_color(slider, channel_color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    /* 滑钮：白色圆形 */
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);

    /* 值变化事件驱动（非轮询） */
    lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_sliders[index] = slider;
}

/**
 * @brief 创建整个用户界面
 */
static void create_ui(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* 屏幕背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1B1B1F), LV_PART_MAIN);

    /* 左侧：GIF 动图区域 + 箭头按钮 */
    create_gif_area(scr);

    /* 右侧：LED 颜色预览圆 + R/G/B 三个颜色滑条 */
    create_led_preview(scr);
    create_rgb_slider(scr, 0, UI_SLIDER_Y_R, lv_color_hex(0xFF0000));   /* R */
    create_rgb_slider(scr, 1, UI_SLIDER_Y_G, lv_color_hex(0x00FF00));   /* G */
    create_rgb_slider(scr, 2, UI_SLIDER_Y_B, lv_color_hex(0x0000FF));   /* B */
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

    /* ---------- 4. 初始化 GIF 资产（运行时填充描述符） ---------- */
    gif_assets_init();

    /* ---------- 5. 初始化板载按键：SET 下一个动图，MODE 上一个动图 ---------- */
    static button_handle_t hw_buttons[BSP_BUTTON_NUM];
    int hw_button_cnt = 0;
    ESP_ERROR_CHECK(bsp_iot_button_create(hw_buttons, &hw_button_cnt, BSP_BUTTON_NUM));
    iot_button_register_cb(hw_buttons[BSP_BUTTON_SET], BUTTON_SINGLE_CLICK, NULL, hw_button_cb,
                           (void *)(intptr_t)BSP_BUTTON_SET);
    iot_button_register_cb(hw_buttons[BSP_BUTTON_MODE], BUTTON_SINGLE_CLICK, NULL, hw_button_cb,
                           (void *)(intptr_t)BSP_BUTTON_MODE);
    ESP_LOGI(TAG, "%d board buttons created, SET/MODE registered for gif switching", hw_button_cnt);

    /* ---------- 6. 创建界面（在 LVGL 锁保护下进行） ---------- */
    if (bsp_display_lock(-1)) {
        create_ui();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "acquire LVGL lock failed");
        return;
    }

    ESP_LOGI(TAG, "gif_led_demo is ready");

    /* ---------- 7. 主循环 ----------
     * 界面渲染、GIF 播放与触摸处理全部由 LVGL 任务维护，
     * 此处仅保持主任务存活。
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
