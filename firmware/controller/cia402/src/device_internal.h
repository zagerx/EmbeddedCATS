/*
 * device_internal.h — cia402 状态机的内部契约
 *
 * 设备层是一个 hsm 状态机：states/ 提供骨架，operations/ 每个操作只占一个
 * 状态，内部用 operation.stage 顺序推进一串事务。
 * 本头给出四样东西：
 *   1. 业务事件（派生自 struct sm_event）；
 *   2. 超时常量；
 *   3. states/ 与 operations/ 里共用的原语：dev_read / dev_write / dev_finish / dev_fail；
 *   4. 全部状态函数的声明。
 *
 * 状态层次只有两层：sm_top → st_operation_active → 具体操作。
 * st_idle 直接挂在 sm_top 下，不继承「被抢占」语义。
 *
 * Author: zager
 */

#ifndef CIA402_DEVICE_INTERNAL_H
#define CIA402_DEVICE_INTERNAL_H

#include <cia402/device_instance.h>
#include <cia402/od.h>
#include <cia402/pds.h>

/* 单笔对象事务的协议栈超时 */
#define CIA402_DEVICE_SDO_TIMEOUT_MS 200u
/* 使能/断使能每步等状态字的期限 */
#define CIA402_DEVICE_STEP_TIMEOUT_MS 300u
/* 减速停的业务期限 */
#define CIA402_DEVICE_HALT_TIMEOUT_MS 3000u
/* PP 触发到到位的期限：按最长行程留余量（10 mm/s 走 500 mm 也要 50 s） */
#define CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS 60000u
/* 回零的业务期限：找开关的时间取决于行程和人手操作，给足余量 */
#define CIA402_DEVICE_HOMING_TIMEOUT_MS 30000u

/* ---- 业务事件 ---- */

enum cia402_sig {
	CIA402_SIG_REQUEST = SM_SIG_USER, /* 邮箱里有新请求，载荷 req */
	CIA402_SIG_SDO_DONE,              /* 一笔事务完成，载荷 result */
	CIA402_SIG_CANCEL,                /* 抢占/超时/取消，载荷 reason */
	CIA402_SIG_TICK                   /* 每拍一次，空闲时消费 */
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
	struct sdo_result result; /* 被取消的那笔事务的原始结果，全部为 0 表示无在途事务 */
	uint8_t reason; /* enum cia402_cancel_reason */
};

/* ---- 状态函数共用原语（device.c） ---- */

/* 发起一笔读；受理后才由 cia402_poll 取结果 */
enum sdo_start_result dev_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms);
/* 发起一笔写；6040 仅在事务成功后更新控制字镜像 */
enum sdo_start_result dev_write(struct cia402_device *d, uint16_t index, uint32_t value,
				uint8_t size, uint32_t timeout_ms);
/* 带子索引的写：数组类对象（如 6099h 的回零速度）用 */
enum sdo_start_result dev_write_sub(struct cia402_device *d, uint16_t index, uint8_t sub,
				    uint32_t value, uint8_t size, uint32_t timeout_ms);
/* 写不进控制字镜像的一次性 6040 指令，例如在线变位 0x103F */
enum sdo_start_result dev_write_pulse(struct cia402_device *d, uint16_t value, uint32_t timeout_ms);
/* 未受理事务的立即收尾；只登记取消请求，不产生操作结局 */
void dev_not_started(struct cia402_device *d, enum sdo_start_result started);
/* 发起读写；未受理时登记取消请求，返回值恒为「事件已消费」故不返回 */
void dev_start_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms);
void dev_start_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
		     uint32_t timeout_ms);
void dev_start_pulse(struct cia402_device *d, uint16_t value, uint32_t timeout_ms);
/* 成功结束；失败结束并保存诊断；被取消的流程不交付结局 */
void dev_done(struct cia402_device *d);
void dev_finish(struct cia402_device *d, enum cia402_fail fail);
/* 事务失败时的统一收尾 */
void dev_fail(struct cia402_device *d, const struct sdo_result *result);

/* ---- 状态函数 ---- */

sm_ret_t device_initial(struct sm_hsm *const me, const struct sm_event *const e);

/* 骨架：无流程、流程进行中的公共规则 */
sm_ret_t st_idle(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_operation_active(struct sm_hsm *const me, const struct sm_event *const e);

/*
 * 每个操作一个状态，内部按 operation.stage 顺序推进；
 * 具体步骤枚举只存在于各自的 operations 文件里。
 */
sm_ret_t st_config(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_enable(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_disable(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_move(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_halt(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_home(struct sm_hsm *const me, const struct sm_event *const e);
/* 回零中止链：被抢占或失败后先停电机、落 bit4、切回 PP，与主流程分离 */
sm_ret_t st_home_abort(struct sm_hsm *const me, const struct sm_event *const e);

/* 急停：写一帧即进终态，没有顺序推进可言，保持两个状态 */
sm_ret_t st_qstop_write(struct sm_hsm *const me, const struct sm_event *const e);
sm_ret_t st_qstop_hold(struct sm_hsm *const me, const struct sm_event *const e);

#endif /* CIA402_DEVICE_INTERNAL_H */
