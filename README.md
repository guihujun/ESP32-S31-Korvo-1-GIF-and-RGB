# ESP32-S31-Korvo GIF 动图 + RGB 灯控制示例

基于官方 [ESP32-S31-Korvo BSP](../../common_components/esp32_s31_korvo)（本仓库
`common_components`）实现的 LVGL 动图显示与板载 WS2812 RGB 灯控制示例。

## 功能

| 区域 | 内容 |
|------|------|
| 左侧 (0,0)-(399,479) | 循环播放的 GIF 动图（自动无限循环） |
| 右侧 (400,0)-(799,479) | 三个直径 120 像素的圆形按钮，垂直等距排列 |
| 顶部红色按钮 | 按下点亮板载 RGB 灯为红色，松开后保持 |
| 中间绿色按钮 | 按下点亮板载 RGB 灯为绿色，松开后保持 |
| 底部蓝色按钮 | 按下点亮板载 RGB 灯为蓝色，松开后保持 |
| 初始状态 | GIF 自动播放，RGB 灯熄灭 |

按钮按下时有触摸反馈：缩放至 90% 并变暗（150ms 平滑过渡）。

## 架构说明（与需求映射）

- **显示初始化**：`bsp_display_start_with_config()`，RGB 面板帧缓冲由 BSP 自动
  分配到 PSRAM（`fb_in_psram=true`），刷新由 RGB 外设 DMA 完成；
  `tear_avoid_mode = TRIPLE_PARTIAL`（三重部分刷新缓冲，优于双缓冲）；
  LVGL 部分绘制缓冲区高度 100 行（800×100）。
- **LVGL 任务**：`esp_lv_adapter`（本 BSP 使用的官方 LVGL 移植，取代旧的
  esp_lvgl_port）创建，绑定 CPU0、优先级 4、栈 8192。在调用 BSP 显示初始化
  之前先调用 `esp_lv_adapter_init()` 指定任务参数。
- **GIF 动图**：LVGL 官方 `lv_gif` 部件（LVGL v9.6）。GIF 数据通过
  `target_add_binary_data` 以二进制数组嵌入固件 Flash（.rodata 段），
  解码后的帧缓冲由 LVGL 分配器路由到 PSRAM（`CONFIG_LV_USE_CLIB_MALLOC=y`）。
- **RGB 灯**：板载 WS2812，官方 BSP 接口 `bsp_led_init()` / `bsp_led_clear()` /
  `bsp_led_set_rgb()`，亮度 80%（分量值 204）。先熄灭再点亮对应颜色。

## 替换自己的 GIF

1. 将你的 GIF 文件覆盖 `main/sample_anim.gif`（文件名保持不变，无需改代码）。
2. 建议尺寸接近 400×480。若动图尺寸匹配，删除 `app_main.c` 中
   `create_gif_area()` 里的 `lv_image_set_scale()` 调用。
3. 动图过大会占用 Flash，注意 `partitions.csv` 中 factory 分区为 6MB。

## 编译与烧录

环境要求：ESP-IDF master（≥6.1.0，本示例要求），目标芯片 `esp32s31`。
VSCode 中请确认 ESP-IDF 配置为 `D:\esp\v6.1\esp-idf`。

```bash
cd examples/esp32-s31-korvo/examples/gif_led_demo
idf.py set-target esp32s31   # 已通过 sdkconfig.defaults 设置，可跳过
idf.py build
idf.py -p COM6 flash monitor
```

## 关键 menuconfig 选项

`sdkconfig.defaults` 已包含全部必要选项，直接编译即可。手动确认入口：

| 选项 | 路径 | 说明 |
|------|------|------|
| `CONFIG_LV_USE_GIF=y` | Component config → LVGL configuration → Use GIF (lv_gif) | 启用 GIF 动图组件（必需） |
| `CONFIG_SPIRAM=y` | Component config → ESP PSRAM | 使能 PSRAM |
| `CONFIG_LV_USE_CLIB_MALLOC=y` | LVGL configuration | LVGL 大块内存经系统 malloc 路由到 PSRAM |
| `CONFIG_IDF_TARGET="esp32s31"` | 芯片型号 | 目标芯片 |

## 常见问题

1. **链接错误 `undefined reference to lv_gif_create`**
   未启用 `CONFIG_LV_USE_GIF`，执行 `idf.py menuconfig` 确认或删除
   `sdkconfig` 后重新构建（`idf.py fullclean && idf.py build`）。

2. **`bsp_display_start_with_config()` 返回 NULL / 绘制缓冲内存不足**
   RGB 部分刷新模式的 LVGL 绘制缓冲位于内部 SRAM，800×100×2B = 160KB。
   若启动失败，可将 `app_main.c` 中 `UI_DISPLAY_BUFFER_HEIGHT` 降至 50。

3. **组件下载失败（网络）**
   组件注册表或 file 服务器不可达时，可配置镜像：
   `$env:IDF_COMPONENT_MIRROR_URL = "https://components.espressif.com"`，
   或重试下载。本示例依赖 `esp32_s31_korvo` 本地路径组件 + 注册表组件
   （esp_lvgl_adapter、lvgl 等）。

4. **烧录提示 Flash 容量不匹配**
   本配置按 8MB Flash 设置（`CONFIG_ESPTOOLPY_FLASHSIZE_8MB`）。若实际
   板卡为 16MB，可改回 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`。

5. **触摸无反应**
   触摸由 BSP 自动初始化（GT1151，兼容 GT911 协议）。检查 I2C 线序与
   排线是否插紧，重新上电。

6. **动图不播放**
   确认 `main/sample_anim.gif` 为合法 GIF89a 文件。示例动图解码到 PSRAM，
   确认 `CONFIG_SPIRAM=y` 已生效（启动日志无 PSRAM 错误）。
