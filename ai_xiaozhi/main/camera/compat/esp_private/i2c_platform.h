/* ==========================================================================
 * 兼容垫片：esp_private/i2c_platform.h
 *
 * 用途：让 espressif/esp32-camera 的 driver/sccb-ng.c 能在 ESP-IDF 5.3 下编译。
 * 背景：
 *   - sccb-ng.c 里 `#if (ESP_IDF_VERSION_MAJOR <= 5)` 会 include 该私有头。
 *   - 该头文件在 IDF 5.4 才随 esp_driver_i2c 一起提供，5.3.4 中并不存在；
 *     而 sccb-ng.c 实际并未使用其中任何符号（只用 driver/i2c_master.h 的公开 API，
 *     如 i2c_master_get_bus_handle / i2c_master_transmit_receive 等）。
 *   - 因此这里提供一个空的占位头，使上游源码无需修改即可编译。
 *
 * 注意：仅本工程的 esp32-camera 目标通过 target_include_directories 使用该目录
 *       （见根目录 CMakeLists.txt）。升级到 IDF >= 5.4 后本文件不再被引用，可删除。
 * ========================================================================== */
#pragma once
