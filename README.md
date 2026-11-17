# ESP32-S31-Korvo GIF 动图轮播 + RGB 滑条调色示例

基于官方 [ESP32-S31-Korvo BSP](../../common_components/esp32_s31_korvo)（本仓库
`common_components`）实现的 LVGL 动图轮播与板载 WS2812 RGB 灯滑条调色示例。

## 功能

| 区域 | 内容 |
|------|------|
| 左侧 (0,0)-(399,429) | 4 个 GIF 动图循环播放（自动无限循环，可切换） |
| 左侧底部 ◀ ▶ | 触摸箭头按钮，切换上一个/下一个动图 |
| 板载按键 SET | 单击切换到下一个动图 |
| 板载按键 MODE | 单击切换到上一个动图 |
| 右侧预览圆 | 实时显示当前 LED 颜色（初始黑色=灯灭） |
| 右侧 R/G/B 滑条 | 左右拖动三个颜色滑条，实时自定义板载 RGB 灯颜色 |

## GIF 动图文件

4 个动图存放于项目根目录 `GIF/` 文件夹，编译时嵌入固件 Flash：

| 文件 | 尺寸 | 画布 |
|------|------|------|
| cat.gif | 442KB | 179×229 |
| ganyu.gif | 577KB | 160×214 |
| qingwa.gif | 27KB | 93×93 |
| sanyu.gif | 334KB | 320×293 |

动图会按画布比例自动缩放居中显示在左侧区域（小图最多放大 4 倍）。

### 添加/替换动图

1. 将新动图放入 `GIF/` 文件夹；
2. 在 `main/CMakeLists.txt` 增加一行：
   `target_add_binary_data(${COMPONENT_LIB} "../GIF/你的文件.gif" BINARY RENAME_TO xxx_gif)`
3. 在 `app_main.c` 中：
   - `GIF_ASSET_DECLARE(xxx_gif)` 声明链接符号；
   - `gif_assets_init()` 中调用 `gif_asset_init()` 注册；
   - 修改 `GIF_COUNT` 宏。

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
  切换动图 = 调用 `lv_gif_set_src()` 换源，自动关闭旧源。
- **按键**：官方 BSP `bsp_iot_button_create()` + iot_button 事件回调
  （`BUTTON_SINGLE_CLICK`）。回调运行在按键任务上下文，操作 LVGL 前通过
  `bsp_display_lock()` 加锁，无轮询。
- **RGB 滑条**：LVGL `lv_slider`，`LV_EVENT_VALUE_CHANGED` 事件实时驱动
  `bsp_led_set_rgb()`（先 `bsp_led_clear()` 再点亮）。滑条范围 0~255，
  若想限流亮度可将 `app_main.c` 中 `UI_COLOR_MAX` 改为 204（80%）。

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
   或重试下载（已下载组件有缓存）。本示例依赖 `esp32_s31_korvo` 本地路径
   组件 + 注册表组件（esp_lvgl_adapter、lvgl 等）。

4. **烧录提示 Flash 容量不匹配**
   本配置按 8MB Flash 设置（`CONFIG_ESPTOOLPY_FLASHSIZE_8MB`）。若实际
   板卡为 16MB，可改回 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`。

5. **触摸无反应**
   触摸由 BSP 自动初始化（GT1151，兼容 GT911 协议）。检查 I2C 线序与
   排线是否插紧，重新上电。

6. **动图不播放 / 切换无反应**
   确认 `GIF/` 下文件为合法 GIF89a 文件且未被改名；添加新动图后检查
   `GIF_COUNT` 与 `gif_assets_init()` 是否同步更新。

7. **板载按键无反应**
   按键为 ADC 分压检测（VOLUP/VOLDOWN/MODE/SET），确认 `bsp_iot_button_create`
   调用成功（启动日志输出 button count）；按键回调中 LVGL 加锁失败会被跳过，
   连续快速按键时偶发属正常。
