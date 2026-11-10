/*
 * 一个 CiA 402 从站的实例化运行控制。
 * 所有流程由服务线程按事件推进，对象访问不阻塞，设备之间不共享可写状态。
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
	const struct sdo_access *access;
	uint8_t node_id;

	pthread_mutex_t lock;
	enum cia402_device_req req;
	enum cia402_device_req running;
	uint8_t cancel;

	enum cia402_device_cfg_state cfg_state;
	enum cia402_device_en_state en_state;
	uint8_t failed;
	enum cia402_device_result result;
	uint32_t abort_code;

	uint16_t statusword;
	int32_t position;
	int32_t velocity;
	uint16_t error_code;

	uint16_t state;
	uint8_t transfer_pending;
	uint32_t now_ms;
	uint32_t deadline_ms;
	uint32_t wait_deadline;
	uint16_t controlword;
	uint16_t pending_controlword;
	uint8_t controlword_pending;

	uint8_t mon_slot;
	uint32_t mon_last_ms;

	uint8_t pp_seen_low;
	int32_t pp_tgt;
	int32_t tgt, vel, acc, dec;
	uint32_t pp_deadline;
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
void cia402_device_cancel(struct cia402_device *d);

/* 仅服务线程调用；每次最多处理一个传输结果或启动一个新流程。 */
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
