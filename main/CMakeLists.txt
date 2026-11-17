idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s31_korvo esp_lvgl_adapter heap
)

# ------------------------------------------------------------------
# 将 GIF 动图文件以二进制形式嵌入固件（数据存放于 Flash 的 .rodata 段）
#
# 每个文件生成符号（供 app_main.c 的 GIF_ASSET_DECLARE 宏引用）：
#   _binary_<name>_start / _binary_<name>_end / <name>_length
#
# TODO: 添加/替换自己的动图：
#   将 GIF 文件放入项目根目录 GIF/ 文件夹，在下方增加一行
#   target_add_binary_data 即可，然后在 app_main.c 中：
#   1. 用 GIF_ASSET_DECLARE(<name>_gif) 声明符号
#   2. 在 gif_assets_init() 中调用 gif_asset_init() 注册
#   3. 修改 GIF_COUNT 宏
# ------------------------------------------------------------------
target_add_binary_data(${COMPONENT_LIB} "../GIF/cat.gif"    BINARY RENAME_TO cat_gif)
target_add_binary_data(${COMPONENT_LIB} "../GIF/ganyu.gif"  BINARY RENAME_TO ganyu_gif)
target_add_binary_data(${COMPONENT_LIB} "../GIF/qingwa.gif" BINARY RENAME_TO qingwa_gif)
target_add_binary_data(${COMPONENT_LIB} "../GIF/sanyu.gif"  BINARY RENAME_TO sanyu_gif)
