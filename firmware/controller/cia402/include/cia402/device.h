/*
 * 一个 CiA 402 从站的运行控制：请求投递、流程推进、结果和反馈。
 * 调用者提交请求，服务线程通过 sdo_access 执行；本层不认识 CAN 帧和业务状态机。
 */

#ifndef CIA402_DEVICE_H
#define CIA402_DEVICE_H

#include <transport/sdo_access.h>

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum cia402_device_cfg_state {
	CIA402_DEVICE_CFG_PENDING = 0,
	CIA402_DEVICE_CFG_CONFIGURED,
	CIA402_DEVICE_CFG_FAILED
};

enum cia402_device_en_state {
	CIA402_DEVICE_EN_NOT_ENABLED = 0,
	CIA402_DEVICE_EN_ENABLED,
	CIA402_DEVICE_EN_FAILED
};

/* 结果被取走后回 NONE；被取消的流程不留下结果。 */
enum cia402_device_result {
	CIA402_DEVICE_RESULT_NONE = 0,
	CIA402_DEVICE_RESULT_DONE,
	CIA402_DEVICE_RESULT_FAILED
};

enum cia402_device_req {
	CIA402_DEVICE_REQ_NONE = 0,
	CIA402_DEVICE_REQ_CONFIG,
	CIA402_DEVICE_REQ_ENABLE,
	CIA402_DEVICE_REQ_DISABLE,
	CIA402_DEVICE_REQ_PP,
	CIA402_DEVICE_REQ_HALT,
	CIA402_DEVICE_REQ_QUICK_STOP
};

struct cia402_device {
	/* 装配后只读 */
	const struct sdo_access *access;
	uint8_t node_id;

	/* 请求槽：任意线程投递，服务线程取走（lock 保护） */
	pthread_mutex_t lock;
	enum cia402_device_req req;
	enum cia402_device_req running;
	uint8_t cancel;
	int32_t tgt, vel, acc, dec;

	/* 流程结论：服务线程写，其他线程查询 */
	enum cia402_device_cfg_state cfg_state;
	enum cia402_device_en_state en_state;
	uint8_t failed;
	enum cia402_device_result result;
	uint32_t abort_code;

	/* 反馈快照：monitor_step 写，axis 层读 */
	uint16_t statusword;
	int32_t position;
	int32_t velocity;
	uint16_t error_code;

	/* 仅服务线程使用，不加锁 */
	uint16_t controlword;
	uint8_t mon_slot;
	uint32_t mon_last_ms;
	/* PP 等待进度：触发后每拍推进一步，其它轴在同一拍里照常推进 */
	uint8_t pp_run;       /* 已触发，等 6041 bit10 */
	uint8_t pp_seen_low;  /* 见过 bit10 = 0：之后变 1 才算到位 */
	int32_t pp_tgt;       /* 本次 PP 的目标（不受调用方提前改 tgt 影响） */
	uint32_t pp_deadline; /* 到位超时时刻 */
};

void cia402_device_init(struct cia402_device *d, const struct sdo_access *access, uint8_t node_id);

/* 0 = 已受理；1 = 已有请求或已完成；-1 = 参数或装配错误。 */
int cia402_device_request_config(struct cia402_device *d);
int cia402_device_request_enable(struct cia402_device *d);
int cia402_device_request_disable(struct cia402_device *d);
int cia402_device_pp_start(struct cia402_device *d, int32_t target, int32_t vel, int32_t acc,
			   int32_t dec);
int cia402_device_halt(struct cia402_device *d);
int cia402_device_quick_stop(struct cia402_device *d);

/* 中断当前对象事务并取消在途流程，供停止和急停使用。 */
void cia402_device_cancel(struct cia402_device *d);

/* 仅服务线程调用；执行到下一次等待点就返回（PP 的到位等待分散到后续拍）。 */
void cia402_device_service(struct cia402_device *d, uint32_t now_ms);

int cia402_device_is_configured(const struct cia402_device *d);
int cia402_device_is_enabled(const struct cia402_device *d);
int cia402_device_failed(const struct cia402_device *d);
int cia402_device_busy(const struct cia402_device *d);
enum cia402_device_result cia402_device_take_result(struct cia402_device *d);
uint32_t cia402_device_abort_code(const struct cia402_device *d);

uint16_t cia402_device_feedback_statusword(const struct cia402_device *d);
int32_t cia402_device_feedback_position(const struct cia402_device *d);
int32_t cia402_device_feedback_velocity(const struct cia402_device *d);
uint16_t cia402_device_feedback_error_code(const struct cia402_device *d);

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_H */
