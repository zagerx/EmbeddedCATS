/*
 * drive_internal.h — drive 层内部接口（不对外）
 *
 * drive.c（请求/服务/反馈）、drive_seq.c（402 流程与 0x6040/0x6041）、
 * drive_pp.c（PP 运动）、backends/od_sdo.c（阻塞对象读写）之间怎么互相调用。
 */

#ifndef DRIVE_INTERNAL_H
#define DRIVE_INTERNAL_H

#include <drive/drive.h>
#include <ds402/pds.h>

/* 一次对象事务的等待上限 */
#define DRIVE_SDO_TIMEOUT_MS 200u
/* 反馈读的等待上限（比事务短：读不到就跳过这一轮） */
#define DRIVE_MON_TIMEOUT_MS 100u
/* 反馈轮转间隔 */
#define DRIVE_MON_INTERVAL_MS 50u
/* 402 序列里每一步"等状态字"的上限（每步独立，卡在哪一步看得见） */
#define DRIVE_STEP_TIMEOUT_MS 300u
/* Halt / 急停的等待上限 */
#define DRIVE_HALT_TIMEOUT_MS 3000u
#define DRIVE_QUICK_STOP_TIMEOUT_MS 5000u
/* PP 到位等待上限 */
#define DRIVE_PP_ARRIVE_TIMEOUT_MS 5000u

/* ---- 对象读写（drive_seq.c）：失败时把 abort code 记进 d ---- */
int drive_od_write(struct drive *d, uint16_t index, uint8_t sub, uint32_t value, uint8_t size);
int drive_od_read(struct drive *d, uint16_t index, uint8_t sub, uint32_t *value, uint8_t *len);

/* 写控制字并记住当前值（bit4 / bit7 / bit8 后续按位改） */
int drive_write_controlword(struct drive *d, uint16_t value);

/* 读状态字：顺手更新 statusword 与 fb_valid */
int drive_read_statusword(struct drive *d, uint16_t *out);

/* 时钟：来自 od_access，不碰平台 */
uint32_t drive_now_ms(struct drive *d);

/* 在途流程是否被要求取消（安全动作） */
int drive_cancelled(struct drive *d);

/* ---- 402 流程（drive_seq.c） ---- */

/*
 * 走一条序列表：先读状态字 →（故障则复位）→ 起点快进 → 逐条"写控制字 + 等状态字"。
 * 每步独立超时；任何时刻被取消或读到故障即失败。0 = 成功。
 */
int drive_seq_run(struct drive *d, const struct ds402_seq_step *seq, uint8_t count,
		  uint32_t step_timeout_ms);

int drive_do_config(struct drive *d); /* 6060=PP + 读 6061 校验 */
int drive_do_enable(struct drive *d);
int drive_do_disable(struct drive *d);
int drive_do_halt(struct drive *d);       /* bit8：减速停，保持使能 */
int drive_do_quick_stop(struct drive *d); /* 0x02 → 等无输出 → 0x00 */

/* ---- PP 运动（drive_pp.c） ---- */
int drive_pp_move(struct drive *d, int32_t target, int32_t vel, int32_t acc, int32_t dec);

#endif /* DRIVE_INTERNAL_H */
