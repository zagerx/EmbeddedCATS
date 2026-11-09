/*
 * drive.h — 一个 DS 402 从站实例的运行期
 *
 * 一个 drive = 一对 0x600/0x580 = 一个轴（物理双轴驱动器 = 2 个 drive）。
 *
 * 分工：
 *   请求（任意线程）：drive_request_* 只往请求槽里放一件事，立即返回
 *   执行（服务线程）：drive_service() 取请求、跑阻塞序列（run_to 等）
 *   事实（任意线程读）：is_configured / is_enabled / failed / feedback_*
 *
 * 本层不知道：CAN 帧、COB-ID、业务状态机、mm。
 * 传输（SDO/PDO）藏在 od_access 后面。
 */

#ifndef DRIVE_DRIVE_H
#define DRIVE_DRIVE_H

#include <drive/mode.h>
#include <drive/od_access.h>

#include <ds402/types.h>
#include <ds402/state_machine.h>

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 只借用指针：drive 不认识 canopen 的类型细节 */
struct co_master;
struct co_node;

/* 配置结论 */
enum drive_cfg_state {
	DRIVE_CFG_PENDING = 0, /* 还没配 / 正在配 */
	DRIVE_CFG_CONFIGURED,  /* 参数已写入并读回校验通过 */
	DRIVE_CFG_FAILED
};

/* 使能结论 */
enum drive_en_state {
	DRIVE_EN_NOT_ENABLED = 0,
	DRIVE_EN_ENABLED,
	DRIVE_EN_FAILED
};

/* 上一个过程的结论；被 take_result() 取走后回 NONE */
enum drive_result {
	DRIVE_RESULT_NONE = 0,
	DRIVE_RESULT_DONE,
	DRIVE_RESULT_FAILED
};

/* 请求槽：一次只受理一件事 */
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
	struct co_master *master;
	struct co_node *node;
	const struct od_access *od;
	const struct mode_ops *mode; /* 当前操作模式的插件表（按 6060 选） */
	uint8_t node_id;

	/* ---- 请求槽（任意线程写，服务线程取） ---- */
	pthread_mutex_t lock;
	enum drive_req req;
	enum drive_req active; /* 服务线程当前在执行哪一件 */
	uint8_t busy;          /* 有请求在执行 */
	int32_t pp_target, pp_vel, pp_acc, pp_dec;

	/* ---- 服务线程私有（同样在 lock 下发布，便于上层读） ---- */
	enum drive_cfg_state cfg_state;
	enum drive_en_state en_state;
	uint8_t failed;
	enum drive_result result;
	uint32_t abort_code;
	uint16_t controlword;

	/* 反馈事实 */
	uint16_t statusword;
	int32_t position;
	int32_t velocity;
	uint16_t error_code;
	uint8_t fb_valid;

	/* PP 在途（等 bit10） */
	uint8_t pp_wait;
	uint8_t pp_seen_low;
	uint32_t pp_since_ms;
	int32_t pp_goal;

	/* 反馈轮转（服务线程空闲时读） */
	uint8_t mon_slot;
	uint8_t mon_done_slot;
	uint32_t mon_last_ms;
};

void drive_init(struct drive *d, struct co_master *master, struct co_node *node,
		const struct od_access *od, uint8_t node_id);

/* ---- 请求：0 = 已受理；1 = 有别的请求在跑；-1 = 参数/装配错误 ---- */
int drive_request_config(struct drive *d);
int drive_request_enable(struct drive *d);
int drive_request_disable(struct drive *d);
int drive_pp_start(struct drive *d, int32_t target, int32_t vel, int32_t acc, int32_t dec);
int drive_halt(struct drive *d);
int drive_quick_stop(struct drive *d);

/* ---- 服务线程每拍调用：取请求并执行（可能阻塞），然后刷新反馈 ---- */
void drive_service(struct drive *d, uint32_t now_ms);

/* ---- 事实 ---- */
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
