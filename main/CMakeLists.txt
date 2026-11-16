idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s31_korvo esp_lvgl_adapter heap
)

# ------------------------------------------------------------------
# 将 GIF 动图文件以二进制形式嵌入固件（数据存放于 Flash 的 .rodata 段）
#
# 生成符号（供 app_main.c 引用）：
#   _binary_sample_anim_gif_start / _binary_sample_anim_gif_end
#
# TODO: 替换自己的动图：
#   将你的 GIF 文件覆盖 main/sample_anim.gif 即可，无需修改本文件。
# ------------------------------------------------------------------
target_add_binary_data(${COMPONENT_LIB} "sample_anim.gif" BINARY RENAME_TO sample_anim_gif)
