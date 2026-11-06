#pragma once

/* ==========================================================================
 * OV2640 摄像头引脚定义（按 main/doc/SCH_原理图.pdf 网表核对）
 *
 *   数据位 8 根：CAM_D2~CAM_D9 -> DVP D0~D7（OV2640 8bit 模式输出 Y2~Y9）
 *   PCLK=34 / XCLK=37 / VSYNC=41 / HREF=39
 *   SCCB（SIOD/SIOC）与 ES8311 共用 GPIO0/1，因此这里给 -1：
 *     esp32-camera 会走 SCCB_Use_Port(XIAOZHI_CAM_SCCB_I2C_PORT)，
 *     复用 xiaozhi_audio.c 已经创建好的 I2C 总线，不再自己新建总线。
 *   RESET/PWDN 只有硬件上下拉，没有 GPIO 控制（不可软复位/软断电）。
 * ========================================================================== */

#define XIAOZHI_CAM_PIN_PWDN   (-1) /* 硬件下拉，无需控制 */
#define XIAOZHI_CAM_PIN_RESET  (-1) /* 硬件上拉，仅能用 SCCB 软复位 */

#define XIAOZHI_CAM_PIN_XCLK   37

/* -1 = 不新建 I2C 总线，复用工程已有的总线（见文件头说明） */
#define XIAOZHI_CAM_PIN_SIOD   (-1)
#define XIAOZHI_CAM_PIN_SIOC   (-1)

/* 端口号必须与 xiaozhi_audio.c 里 i2c_new_master_bus() 使用的端口一致 */
#define XIAOZHI_CAM_SCCB_I2C_PORT 0

#define XIAOZHI_CAM_PIN_D7     38 /* CAM_D9 */
#define XIAOZHI_CAM_PIN_D6     36 /* CAM_D8 */
#define XIAOZHI_CAM_PIN_D5     35 /* CAM_D7 */
#define XIAOZHI_CAM_PIN_D4     33 /* CAM_D6 */
#define XIAOZHI_CAM_PIN_D3     10 /* CAM_D5 */
#define XIAOZHI_CAM_PIN_D2     12 /* CAM_D4 */
#define XIAOZHI_CAM_PIN_D1     11 /* CAM_D3 */
#define XIAOZHI_CAM_PIN_D0     9  /* CAM_D2 */

#define XIAOZHI_CAM_PIN_VSYNC  41
#define XIAOZHI_CAM_PIN_HREF   39
#define XIAOZHI_CAM_PIN_PCLK   34
