/*
 * drive_internal.h — drive 层内部接口（不对外）
 *
 * drive.c（请求/服务/反馈）、drive_sm.c（走到目标状态）、mode_pp.c（PP 运动）
 * 之间怎么互相调用。
 */

#ifndef DRIVE_INTERNAL_H
#define DRIVE_INTERNAL_H

#include <drive/drive.h>

/* 一次对象事务的等待上限 */
#define DRIVE_SDO_TIMEOUT_MS 200u
/* 反馈读的等待上限（比事务短：读不到就跳过这一轮） */
#define DRIVE_MON_TIMEOUT_MS 100u
/* 反馈轮转间隔 */
#define DRIVE_MON_INTERVAL_MS 50u
/* 走到目标状态的总时长上限 */
#define DRIVE_SM_TIMEOUT_MS 1000u
/* Halt / 急停的等待上限 */
#define DRIVE_HALT_TIMEOUT_MS 3000u
#define DRIVE_QUICK_STOP_TIMEOUT_MS 5000u
/* PP 到位等待上限 */
#define DRIVE_PP_ARRIVE_TIMEOUT_MS 5000u

/* ---- 对象读写（drive_sm.c）：失败时把 abort code 记进 d ---- */
int drive_od_write(struct drive *d, uint16_t index, uint8_t sub, uint32_t value, uint8_t size);
int drive_od_read(struct drive *d, uint16_t index, uint8_t sub, uint32_t *value, uint8_t *len);

/* 写控制字并记住当前值（bit4 / bit8 后续按位改） */
int drive_write_controlword(struct drive *d, uint16_t value);

/* 从当前状态走到 target（阻塞）：发命令 → 读状态字 → 再规划 */
int drive_sm_run_to(struct drive *d, enum ds402_state target, uint32_t timeout_ms);

#endif /* DRIVE_INTERNAL_H */
