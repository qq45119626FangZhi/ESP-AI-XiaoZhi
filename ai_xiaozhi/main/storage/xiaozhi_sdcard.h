#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ==========================================================================
 * SD 卡存储（M1）
 *   1. SDMMC 1-bit 挂载：CLK=GPIO14 / CMD=GPIO13 / D0=GPIO15
 *      （无 CD 检测脚、无 DAT3 上拉，只能靠挂载/读写失败判断拔卡）
 *      时钟 20MHz；挂载失败自动降频重试（20→10→5MHz，成功后保持该档，
 *      卸载/拔卡后回到最高档重探），见 xiaozhi_sdcard.c 的 XIAOZHI_SD_FREQ_FALLBACK
 *   2. 上电自动挂载 + 5s 周期守护（拔卡后自动重挂载），挂载日志含容量与写速
 *   3. 目录约定（见 Doc/整体规划.md 4.2）：
 *        /sdcard/DCIM/2026-11-05/EVT_XXXX.avi | SNAP_XXXX.jpg
 *        /sdcard/LOG/event.csv
 *        /sdcard/CONFIG/recorder.json
 *   4. 循环覆盖：空间不足时按文件名（= 时间顺序）删除 DCIM 下最旧的文件
 *   5. 全部实现由 XIAOZHI_SD_ENABLE 开关控制，=0 时退化为空实现，
 *      不占用 SDMMC 外设、不创建任务，完全不影响既有语音链路
 * ========================================================================== */

/* ---------------- 编译期开关 ---------------- */

/* SD 卡存储总开关：0 = 全部接口为空实现（等同未接入本模块） */
#define XIAOZHI_SD_ENABLE 1

/* 挂载点（VFS 前缀，带前导 /，不带结尾 /） */
#define XIAOZHI_SD_MOUNT_POINT "/sdcard"

/* 1 = 上电自动挂载并启动重挂载守护任务；0 = 只有上层显式调用才挂载 */
#define XIAOZHI_SD_AUTO_MOUNT 1

/* 1 = 挂载失败时格式化（会清空整卡数据！默认 0 保守） */
#define XIAOZHI_SD_FORMAT_IF_FAIL 0

/* 1 = 挂载成功后做一次写速自检（写 512KB 再删除），用于 M1 验收
 * 当前置 0：调试三模切换期间不向卡写任何数据 */
#define XIAOZHI_SD_SPEED_TEST 0

/* 守护任务重试周期（ms） */
#define XIAOZHI_SD_RETRY_MS 5000

/* 循环覆盖时保留的最小剩余空间（循环删除阈值，默认 500MB） */
#define XIAOZHI_SD_KEEP_FREE_BYTES (500ULL * 1024 * 1024)

/* ---------------- 对外 API ---------------- */

/* 初始化（main.c 初始化区调用一次）：创建互斥量、按需挂载、启动守护任务 */
esp_err_t xiaozhi_sdcard_init(void);

/* 挂载 / 卸载（可被上层业务反复调用，内部互斥、幂等） */
esp_err_t xiaozhi_sdcard_mount(void);
esp_err_t xiaozhi_sdcard_unmount(void);

/* 是否已挂载可用（读写前先判断，未挂载时不要调用文件接口） */
bool xiaozhi_sdcard_ready(void);

/* 容量统计（字节）；未挂载返回 0（会同步做一次 FATFS 查询，勿在 HTTP 任务里高频调用） */
uint64_t xiaozhi_sdcard_total_bytes(void);
uint64_t xiaozhi_sdcard_free_bytes(void);

/* 容量缓存（非阻塞）：由挂载流程和 5 s 守护任务刷新，HTTP 等任务直接读，
 * 避免卡异常时在 httpd 任务里阻塞 FATFS 把网页服务卡死。 */
bool xiaozhi_sdcard_cache_ready(void);
uint64_t xiaozhi_sdcard_free_bytes_cached(void);
uint64_t xiaozhi_sdcard_total_bytes_cached(void);

/* 确保目录存在（自动建父目录不做，path 的父目录必须已存在） */
esp_err_t xiaozhi_sdcard_ensure_dir(const char *path);

/* 循环覆盖：剩余空间不足 keep_free_bytes 时，删除 DCIM 下最旧的文件直到满足 */
esp_err_t xiaozhi_sdcard_reclaim(uint64_t keep_free_bytes);

/* 写速自检（写 512KB → flush → 删除），返回 ESP_OK 表示卡可正常读写 */
esp_err_t xiaozhi_sdcard_selftest(void);

/* 挂载点字符串（"/sdcard"），方便上层拼路径 */
const char *xiaozhi_sdcard_root(void);

/* 拔卡自动重挂载开关（模式独占场景下，上层卸载后应关掉，避免被自动挂回来） */
void xiaozhi_sdcard_set_auto_recover(bool enable);
