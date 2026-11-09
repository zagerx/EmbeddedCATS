/*
 * drive.h — 一个 402 从站的运行期（对外契约）
 *
 * 一个 drive = 一个从站 = 一对 0x600/0x580 = 一个轴。
 *
 * 模型：**意图投递 + 服务线程同步执行**
 *   任意线程   投递意图（drive_request_enable / drive_pp_start / drive_halt …）
 *              只写槽位，不阻塞；
 *   服务线程   drive_service() 取出意图，**同步跑完整条流程**（发帧、等状态字），
 *              期间阻塞在对象读写上（condvar 睡眠，让出 CPU）；
 *   任意线程   读事实（is_enabled / feedback）、取结论（take_result）；
 *   安全动作   drive_cancel() 打断在途流程 —— 等待点立刻返回，不空转。
 *
 * 本层不知道：CAN 帧与 COB-ID（master/canopen）、业务状态机（statemachine）、
 * 单位 mm（axis）。
 */

#ifndef DRIVE_DRIVE_H
#define DRIVE_DRIVE_H

#include <drive/od_access.h>

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 配置结论 */
enum drive_cfg_state {
	DRIVE_CFG_PENDING = 0, /* 还没配 / 正在配 */
	DRIVE_CFG_CONFIGURED,  /* 参数已写入并读回校验通过 */
	DRIVE_CFG_FAILED
};

/* 使能结论（事实，不是流程） */
enum drive_en_state {
	DRIVE_EN_NOT_ENABLED = 0,
	DRIVE_EN_ENABLED,
	DRIVE_EN_FAILED
};

/* 上一次流程的结论；take_result() 取走后回 NONE。被取消的流程不留结论 */
enum drive_result {
	DRIVE_RESULT_NONE = 0,
	DRIVE_RESULT_DONE,
	DRIVE_RESULT_FAILED
};

/* 请求槽：一次只受理一件事（安全动作可抢占） */
enum drive_req {
	DRIVE_REQ_NONE = 0,
	DRIVE_REQ_CONFIG,
	DRIVE_REQ_ENABLE,
	DRIVE_REQ_DISABLE,
	DRIVE_REQ_PP,
	DRIVE_REQ_HALT,
	DRIVE_REQ_QUICK_STOP
};

/* 反馈有效位 */
enum {
	DRIVE_FB_STATUSWORD = 0x01,
	DRIVE_FB_POSITION = 0x02,
	DRIVE_FB_VELOCITY = 0x04,
	DRIVE_FB_ERROR_CODE = 0x08
};

struct drive {
	/* ---- 装配期写入，之后只读 ---- */
	const struct od_access *od;
	uint8_t node_id;

	/* ---- 请求槽（任意线程写，服务线程取） ---- */
	pthread_mutex_t lock;
	enum drive_req req;         /* 待执行 */
	enum drive_req running;     /* 服务线程正在执行哪一件 */
	uint8_t cancel;             /* 打断在途流程（安全动作） */
	int32_t tgt, vel, acc, dec; /* 只有 PP 用 */

	/* ---- 事实与结论（lock 下发布） ---- */
	enum drive_cfg_state cfg_state;
	enum drive_en_state en_state;
	uint8_t failed;
	enum drive_result result;
	uint32_t abort_code;
	uint16_t controlword; /* 最近一次写进 6040 的值（bit4/bit7/bit8 按位改） */

	uint16_t statusword;
	int32_t position;
	int32_t velocity;
	uint16_t error_code;
	uint8_t fb_valid;

	/* ---- 反馈轮转（服务线程空闲时读） ---- */
	uint8_t mon_slot;
	uint32_t mon_last_ms;
};

void drive_init(struct drive *d, const struct od_access *od, uint8_t node_id);

/* ---- 意图：0 = 已受理；1 = 有别的请求在跑；-1 = 参数/装配错误 ---- */
int drive_request_config(struct drive *d);
int drive_request_enable(struct drive *d);
int drive_request_disable(struct drive *d);
int drive_pp_start(struct drive *d, int32_t target, int32_t vel, int32_t acc, int32_t dec);
int drive_halt(struct drive *d);       /* PP 的 bit8 减速停，保持使能 */
int drive_quick_stop(struct drive *d); /* 急停第一阶段：0x02 → 等无输出 → 0x00 */

/*
 * 打断在途流程：正在等的对象事务立刻返回失败，服务线程尽快退出当前流程。
 * 只给安全动作用（halt / 急停），不要拿来"重试"。
 */
void drive_cancel(struct drive *d);

/* ---- 服务线程：取出意图并同步执行完（每拍调一次） ---- */
void drive_service(struct drive *d, uint32_t now_ms);

/* ---- 事实（任意线程） ---- */
int drive_is_configured(const struct drive *d);
int drive_is_enabled(const struct drive *d);
int drive_failed(const struct drive *d);
int drive_busy(const struct drive *d);
enum drive_result drive_take_result(struct drive *d);
uint32_t drive_abort_code(const struct drive *d);

uint16_t drive_feedback_statusword(const struct drive *d);
int32_t drive_feedback_position(const struct drive *d);
int32_t drive_feedback_velocity(const struct drive *d);
uint16_t drive_feedback_error_code(const struct drive *d);
uint8_t drive_feedback_valid(const struct drive *d);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRIVE_DRIVE_H */
