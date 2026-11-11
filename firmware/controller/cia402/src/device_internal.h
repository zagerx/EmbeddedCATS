/*
 * device_internal.h — cia402 状态机的内部契约
 *
 * 设备层是一个 hsm 状态机：状态函数在 states/ 下，一流程族一个文件。
 * 本头给出四样东西：
 *   1. 业务事件（派生自 struct sm_event）；
 *   2. 超时常量；
 *   3. states/ 里共用的原语：dev_read / dev_write / dev_finish / dev_fail；
 *   4. 全部状态函数的声明。
 *
 * Author: zager
 */

#ifndef CIA402_DEVICE_INTERNAL_H
#define CIA402_DEVICE_INTERNAL_H

#include <cia402/device.h>
#include <cia402/od.h>
#include <cia402/pds.h>

/* 单笔对象事务的协议栈超时 */
#define CIA402_DEVICE_SDO_TIMEOUT_MS 200u
/* 反馈轮转：单个对象超时与两轮之间的间隔 */
#define CIA402_DEVICE_MON_TIMEOUT_MS 100u
#define CIA402_DEVICE_MON_INTERVAL_MS 50u
/* 使能/断使能每步等状态字的期限 */
#define CIA402_DEVICE_STEP_TIMEOUT_MS 300u
/* 减速停与快速停的业务期限 */
#define CIA402_DEVICE_HALT_TIMEOUT_MS 3000u
#define CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS 5000u
/* PP 触发到到位的期限：按最长行程留余量（10 mm/s 走 500 mm 也要 50 s） */
#define CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS 60000u

/* ---- 业务事件 ---- */

enum cia402_sig {
	CIA402_SIG_REQUEST = SM_SIG_USER, /* 请求槽里有新请求，载荷 req */
	CIA402_SIG_SDO_DONE,                /* 一笔事务完成，载荷 result */
	CIA402_SIG_CANCEL,              /* 抢占/超时/取消，载荷 reason */
	CIA402_SIG_TICK                 /* 每拍一次，空闲时用于反馈节奏 */
};

struct request_evt {
	struct sm_event super;
	struct cia402_request req;
};

struct sdo_done_evt {
	struct sm_event super;
	struct sdo_result result;
};

struct cancel_evt {
	struct sm_event super;
	uint8_t reason; /* enum cia402_cancel_reason */
};

/* ---- 状态函数共用原语（device.c） ---- */

/* 发起一笔读；被拒时登记一个失败事件，由下一拍分发 */
void dev_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms);
/* 发起一笔写；写 6040 时暂存期望值，事务成功后才落到控制字镜像 */
void dev_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
	       uint32_t timeout_ms);
/* 结束当前流程：写结论与失败原因，调用方随后迁回 st_idle */
void dev_finish(struct cia402_device *d, enum cia402_result result, enum cia402_fail fail);
/* 事务失败时的统一收尾 */
void dev_fail(struct cia402_device *d, const struct sdo_result *result);
/* 缓存最近一次状态字（读事务的通用动作）*/
void dev_cache_statusword(struct cia402_device *d, uint16_t statusword);
/* 取当前在执行的请求类别（判断是不是某条流程）*/
enum cia402_kind dev_running_kind(const struct cia402_device *d);
/* 请求类别 → 对外报告的阶段 */
enum cia402_phase dev_phase_of(enum cia402_kind kind);

/* ---- 状态函数 ---- */

sm_ret_t device_initial(struct sm_hsm *const me, const struct sm_event *const e);

sm_ret_t st_idle(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_monitor(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_running(struct sm_hsm *const me, const struct sm_event *const e);

sm_ret_t st_config_write_mode(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_config_read_mode(struct sm_hsm *const me, const struct sm_event *const e);

sm_ret_t st_enable_read_pds(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_reset_low(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_reset_high(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_read_reset(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_write_shutdown(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_read_ready(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_write_switch_on(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_read_switched_on(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_write_enable_op(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable_read_enabled(struct sm_hsm *const me, const struct sm_event *const e);

sm_ret_t st_disable_write(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_disable_read(struct sm_hsm *const me, const struct sm_event *const e);

sm_ret_t st_pp_write_velocity(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_pp_write_accel(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_pp_write_decel(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_pp_write_target(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_pp_trigger_high(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_pp_trigger_low(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_pp_read_status(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_pp_read_position(struct sm_hsm *const me, const struct sm_event *const e);

sm_ret_t st_halt_write(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_halt_read(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_halt_clear(struct sm_hsm *const me, const struct sm_event *const e);

sm_ret_t st_qstop_write(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_qstop_read(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_qstop_disable(struct sm_hsm *const me, const struct sm_event *const e);

#endif /* CIA402_DEVICE_INTERNAL_H */
