/*
 * cats_mech.h — 机械换算参数（整机常量，编译期固定）
 *
 * 为什么是宏而不是运行期配置：
 *   这两个值是**这台机器的固件常量**（同型号双轴驱动器 + 同规格滑台），
 *   不是运行期可调项。写成宏以后，轴层直接拿来做换算，装配层不用再传参数，
 *   也就没有"没配过就拒绝指令"的分支。
 *
 * 换算式：
 *   pulse = round(mm * PULSES_PER_REV / MM_PER_REV)   （四舍五入、远离零）
 *   mm    = pulse * MM_PER_REV / PULSES_PER_REV
 *
 * 当前取值：
 *   PULSES_PER_REV = 10000：一圈 = 10000 指令单位，**运动实测**得来
 *     （命令 2 圈 = 20000，`6064h` 增量 19921 / 19997；见
 *     `docs/02_驱动器说明/驱动器配置基准.md`）。
 *     这是**指令单位**，不是编码器计数（`608Fh` = 131072 与它无关）。
 *   MM_PER_REV = 5 mm：滑台导程，**尚未实测**，暂时沿用 5 mm/圈。
 *     它不影响"圈数 ⇄ 指令单位"的换算，只影响界面上 mm 与实际位移的对应。
 */

#ifndef CATS_MECH_H
#define CATS_MECH_H

/* 电机转 1 圈对应的指令单位数（运动实测：10000） */
#define CATS_AXIS_PULSES_PER_REV 10000

/* 电机转 1 圈对应的滑台位移，单位 mm（尚未实测，暂按 5 mm/圈） */
#define CATS_AXIS_MM_PER_REV 5.0f

/*
 * 回零参数（2026-11-20 台架实测可用值，两轴相同）
 *
 * 速度单位是指令单位/s，1 圈 = 10000；方法 21 = 原点开关 + 不用 Z 信号 + 负向起始。
 * 台架建议值偏慢：2000 = 0.2 圈/s，人手遮挡开关来得及。实机按行程重算。
 */
#define CATS_AXIS_HOME_METHOD 21
#define CATS_AXIS_HOME_SPEED_SEARCH 2000
#define CATS_AXIS_HOME_SPEED_CREEP 500
#define CATS_AXIS_HOME_ACCEL 2000

#endif /* CATS_MECH_H */
