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
 * 当前取值：**临时值**（2026-11-05 用户提供）
 *   电机转 1 圈 = 1000 个脉冲；电机转 1 圈 ⇒ 滑台位移 5 mm ⇒ 0.005 mm/pulse。
 *   注意：这里的 1000 pulse/rev **与驱动器电子齿轮无关**，电子齿轮后续单独设计，
 *   届时这个常量要跟着更新（只改这一处）。
 */

#ifndef CATS_MECH_H
#define CATS_MECH_H

/* 电机转 1 圈对应的脉冲数（临时值：1000） */
#define CATS_AXIS_PULSES_PER_REV 1000

/* 电机转 1 圈对应的滑台位移，单位 mm（临时值：5 mm/圈） */
#define CATS_AXIS_MM_PER_REV 5.0f

#endif /* CATS_MECH_H */
