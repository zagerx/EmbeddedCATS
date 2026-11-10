/*
 * device.c — 按事件推进 CiA 402 设备状态机
 *
 * 每个实例独立保存请求、流程、传输和反馈状态。
 * 服务线程执行全部状态迁移，其他线程只能提交请求或读取加锁快照。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <stddef.h>
#include <string.h>

static int submit(struct cia402_device *d, enum cia402_device_req req);
static int submit_urgent(struct cia402_device *d, enum cia402_device_req req);
static enum cia402_device_req pop_request(struct cia402_device *d);
static void complete_transfer(struct cia402_device *d, const struct sdo_result *result);
static void monitor_start(struct cia402_device *d);
static void monitor_complete(struct cia402_device *d, const struct sdo_result *result);
static void monitor_skip(struct cia402_device *d);

/**
 * cia402_device_init - 初始化一个设备实例
 * @d: 设备对象
 * @access: 异步对象访问接口
 * @node_id: CANopen 节点号
 */
void cia402_device_init(struct cia402_device *d, const struct sdo_access *access, uint8_t node_id)
{
	memset(d, 0, sizeof(*d));
	pthread_mutex_init(&d->lock, NULL);
	d->access = access;
	d->node_id = node_id;
	d->cfg_state = CIA402_DEVICE_CFG_PENDING;
	d->en_state = CIA402_DEVICE_EN_NOT_ENABLED;
	d->state = CIA402_ST_IDLE;
}

/**
 * cia402_device_request_config - 投递配置请求
 * @d: 设备对象
 *
 * Return: 0 已受理；1 已配置、已失败或忙；-1 参数或装配错误。
 */
int cia402_device_request_config(struct cia402_device *d)
{
	if (d != NULL) {
		pthread_mutex_lock(&d->lock);
		if (d->cfg_state != CIA402_DEVICE_CFG_PENDING) {
			pthread_mutex_unlock(&d->lock);
			return 1;
		}
		pthread_mutex_unlock(&d->lock);
	}
	return submit(d, CIA402_DEVICE_REQ_CONFIG);
}

/**
 * cia402_device_request_enable - 投递使能请求
 * @d: 设备对象
 *
 * Return: 0 已受理；1 已使能、已失败或忙；-1 参数或装配错误。
 */
int cia402_device_request_enable(struct cia402_device *d)
{
	if (d != NULL) {
		pthread_mutex_lock(&d->lock);
		if (d->en_state != CIA402_DEVICE_EN_NOT_ENABLED) {
			pthread_mutex_unlock(&d->lock);
			return 1;
		}
		pthread_mutex_unlock(&d->lock);
	}
	return submit(d, CIA402_DEVICE_REQ_ENABLE);
}

/**
 * cia402_device_request_disable - 投递断使能请求
 * @d: 设备对象
 *
 * Return: 0 已受理；1 设备忙；-1 参数或装配错误。
 */
int cia402_device_request_disable(struct cia402_device *d)
{
	return submit(d, CIA402_DEVICE_REQ_DISABLE);
}

/**
 * cia402_device_pp_start - 投递 PP 绝对位置请求
 * @d: 设备对象
 * @target: 目标位置，单位 pulse
 * @vel: 轮廓速度
 * @acc: 轮廓加速度
 * @dec: 轮廓减速度
 *
 * Return: 0 已受理；1 设备忙；-1 设备为空或未使能。
 */
int cia402_device_pp_start(struct cia402_device *d, int32_t target, int32_t vel, int32_t acc,
			   int32_t dec)
{
	if (d == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->en_state != CIA402_DEVICE_EN_ENABLED) {
		pthread_mutex_unlock(&d->lock);
		return -1;
	}
	if (d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE) {
		pthread_mutex_unlock(&d->lock);
		return 1;
	}
	d->tgt = target;
	d->vel = vel;
	d->acc = acc;
	d->dec = dec;
	d->failed = 0u;
	d->req = CIA402_DEVICE_REQ_PP;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

/**
 * cia402_device_halt - 抢占当前流程并请求减速停车
 * @d: 设备对象
 *
 * Return: 0 已受理；-1 参数或装配错误。
 */
int cia402_device_halt(struct cia402_device *d)
{
	return submit_urgent(d, CIA402_DEVICE_REQ_HALT);
}

/**
 * cia402_device_quick_stop - 抢占当前流程并请求快速停止
 * @d: 设备对象
 *
 * Return: 0 已受理；-1 参数或装配错误。
 */
int cia402_device_quick_stop(struct cia402_device *d)
{
	return submit_urgent(d, CIA402_DEVICE_REQ_QUICK_STOP);
}

/**
 * cia402_device_cancel - 取消当前流程且不启动后续请求
 * @d: 设备对象；NULL 安全
 */
void cia402_device_cancel(struct cia402_device *d)
{
	if (d == NULL) {
		return;
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != CIA402_DEVICE_REQ_NONE || d->transfer_pending != 0u) {
		d->cancel = CIA402_CANCEL_MANUAL;
	}
	pthread_mutex_unlock(&d->lock);
}

/**
 * cia402_device_service - 推进一次设备状态机
 * @d: 设备对象
 * @now_ms: 当前单调毫秒时刻
 *
 * 仅服务线程调用；每次最多消费一个传输结果或启动一个请求，不阻塞。
 */
void cia402_device_service(struct cia402_device *d, uint32_t now_ms)
{
	struct sdo_result result;
	enum cia402_device_req req;

	if (d == NULL || d->access == NULL) {
		return;
	}
	d->now_ms = now_ms;

	pthread_mutex_lock(&d->lock);
	{
		uint8_t cancel = d->cancel;

		pthread_mutex_unlock(&d->lock);
		if (cancel != CIA402_CANCEL_NONE && d->transfer_pending != 0u) {
			d->access->abort(d->access->ctx);
		}
	}

	if (d->transfer_pending != 0u) {
		int rc = d->access->take_result(d->access->ctx, &result);

		if (rc != 0) {
			if ((int32_t)(now_ms - d->deadline_ms) >= 0) {
				pthread_mutex_lock(&d->lock);
				if (d->cancel == CIA402_CANCEL_NONE) {
					d->cancel = CIA402_CANCEL_TIMEOUT;
					pthread_mutex_unlock(&d->lock);
					d->access->abort(d->access->ctx);
				} else {
					pthread_mutex_unlock(&d->lock);
				}
			}
			return;
		}
		d->transfer_pending = 0u;
		complete_transfer(d, &result);
		return;
	}

	pthread_mutex_lock(&d->lock);
	if (d->cancel != CIA402_CANCEL_NONE) {
		d->cancel = CIA402_CANCEL_NONE;
		d->running = CIA402_DEVICE_REQ_NONE;
		d->state = CIA402_ST_IDLE;
	}
	pthread_mutex_unlock(&d->lock);

	req = pop_request(d);
	if (req != CIA402_DEVICE_REQ_NONE) {
		pthread_mutex_lock(&d->lock);
		d->running = req;
		pthread_mutex_unlock(&d->lock);
		d->pp_seen_low = 0u;
		if (req == CIA402_DEVICE_REQ_PP) {
			cia402_device_pp_begin(d);
		} else {
			cia402_device_seq_begin(d, req);
		}
		return;
	}

	if (d->running == CIA402_DEVICE_REQ_NONE) {
		monitor_start(d);
	}
}

/**
 * cia402_device_is_configured - 查询配置事实
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 已配置；0 其他情况。
 */
int cia402_device_is_configured(const struct cia402_device *d)
{
	int ok;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	ok = d->cfg_state == CIA402_DEVICE_CFG_CONFIGURED;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return ok;
}

/**
 * cia402_device_is_enabled - 查询使能事实
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 已使能；0 其他情况。
 */
int cia402_device_is_enabled(const struct cia402_device *d)
{
	int ok;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	ok = d->en_state == CIA402_DEVICE_EN_ENABLED;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return ok;
}

/**
 * cia402_device_failed - 查询流程失败事实
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 有失败记录；0 其他情况。
 */
int cia402_device_failed(const struct cia402_device *d)
{
	int failed;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	failed = d->failed != 0u;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return failed;
}

/**
 * cia402_device_busy - 查询设备是否有请求或流程在途
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 忙或设备为空；0 空闲。
 */
int cia402_device_busy(const struct cia402_device *d)
{
	int busy;

	if (d == NULL) {
		return 1;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	busy = d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return busy;
}

/**
 * cia402_device_take_result - 取走最近一次流程结论
 * @d: 设备对象；NULL 安全
 *
 * Return: 当前结论，取走后恢复 NONE。
 */
enum cia402_device_result cia402_device_take_result(struct cia402_device *d)
{
	enum cia402_device_result result;

	if (d == NULL) {
		return CIA402_DEVICE_RESULT_NONE;
	}
	pthread_mutex_lock(&d->lock);
	result = d->result;
	d->result = CIA402_DEVICE_RESULT_NONE;
	pthread_mutex_unlock(&d->lock);
	return result;
}

/**
 * cia402_device_abort_code - 读取最近一次失败的 SDO abort 码
 * @d: 设备对象；NULL 安全
 *
 * Return: abort 码；无记录或设备为空时返回 0。
 */
uint32_t cia402_device_abort_code(const struct cia402_device *d)
{
	uint32_t code;

	if (d == NULL) {
		return 0u;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	code = d->abort_code;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return code;
}

/**
 * cia402_device_feedback_statusword - 读取缓存的状态字
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次状态字；设备为空时返回 0。
 */
uint16_t cia402_device_feedback_statusword(const struct cia402_device *d)
{
	uint16_t value;

	if (d == NULL) {
		return 0u;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->statusword;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

/**
 * cia402_device_feedback_position - 读取缓存的实际位置
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次位置；设备为空时返回 0。
 */
int32_t cia402_device_feedback_position(const struct cia402_device *d)
{
	int32_t value;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->position;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

/**
 * cia402_device_feedback_velocity - 读取缓存的实际速度
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次速度；设备为空时返回 0。
 */
int32_t cia402_device_feedback_velocity(const struct cia402_device *d)
{
	int32_t value;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->velocity;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

/**
 * cia402_device_feedback_error_code - 读取缓存的驱动器错误码
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次错误码；设备为空时返回 0。
 */
uint16_t cia402_device_feedback_error_code(const struct cia402_device *d)
{
	uint16_t value;

	if (d == NULL) {
		return 0u;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->error_code;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

void cia402_device_start_read(struct cia402_device *d, enum cia402_state state, uint16_t index,
			      uint32_t timeout_ms)
{
	int rc;

	d->state = state;
	d->deadline_ms = d->now_ms + timeout_ms;
	d->controlword_pending = 0u;
	rc = d->access->start(d->access->ctx, SDO_DIR_READ, index, 0u, 0u, 0u, timeout_ms);
	if (rc != 0) {
		if (d->running == CIA402_DEVICE_REQ_NONE) {
			monitor_skip(d);
		} else {
			cia402_device_finish(d, -1, 0u);
		}
		return;
	}
	d->transfer_pending = 1u;
}

void cia402_device_start_write(struct cia402_device *d, enum cia402_state state, uint16_t index,
			       uint32_t value, uint8_t size, uint32_t timeout_ms)
{
	int rc;

	d->state = state;
	d->deadline_ms = d->now_ms + timeout_ms;
	if (index == CIA402_OD_CONTROLWORD) {
		d->pending_controlword = (uint16_t)value;
		d->controlword_pending = 1u;
	} else {
		d->controlword_pending = 0u;
	}
	rc = d->access->start(d->access->ctx, SDO_DIR_WRITE, index, 0u, value, size, timeout_ms);
	if (rc != 0) {
		d->controlword_pending = 0u;
		cia402_device_finish(d, -1, 0u);
		return;
	}
	d->transfer_pending = 1u;
}

void cia402_device_finish(struct cia402_device *d, int ok, uint32_t abort_code)
{
	pthread_mutex_lock(&d->lock);
	if (ok != 0) {
		d->abort_code = abort_code;
		d->failed = 1u;
	}
	switch (d->running) {
	case CIA402_DEVICE_REQ_CONFIG:
		d->cfg_state = (ok == 0) ? CIA402_DEVICE_CFG_CONFIGURED : CIA402_DEVICE_CFG_FAILED;
		break;
	case CIA402_DEVICE_REQ_ENABLE:
		d->en_state = (ok == 0) ? CIA402_DEVICE_EN_ENABLED : CIA402_DEVICE_EN_FAILED;
		break;
	case CIA402_DEVICE_REQ_DISABLE:
		d->en_state = CIA402_DEVICE_EN_NOT_ENABLED;
		break;
	case CIA402_DEVICE_REQ_QUICK_STOP:
		d->en_state = (ok == 0) ? CIA402_DEVICE_EN_NOT_ENABLED : CIA402_DEVICE_EN_FAILED;
		break;
	default:
		break;
	}
	d->result = (ok == 0) ? CIA402_DEVICE_RESULT_DONE : CIA402_DEVICE_RESULT_FAILED;
	d->running = CIA402_DEVICE_REQ_NONE;
	pthread_mutex_unlock(&d->lock);
	d->state = CIA402_ST_IDLE;
	d->controlword_pending = 0u;
}

static int submit(struct cia402_device *d, enum cia402_device_req req)
{
	int rc;

	if (d == NULL || d->access == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE) {
		rc = 1;
	} else {
		d->req = req;
		rc = 0;
	}
	pthread_mutex_unlock(&d->lock);
	return rc;
}

static int submit_urgent(struct cia402_device *d, enum cia402_device_req req)
{
	if (d == NULL || d->access == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	d->req = req;
	d->cancel = CIA402_CANCEL_REQUEST;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

static enum cia402_device_req pop_request(struct cia402_device *d)
{
	enum cia402_device_req req;

	pthread_mutex_lock(&d->lock);
	req = d->req;
	d->req = CIA402_DEVICE_REQ_NONE;
	pthread_mutex_unlock(&d->lock);
	return req;
}

static void complete_transfer(struct cia402_device *d, const struct sdo_result *result)
{
	enum cia402_cancel_reason cancel;

	pthread_mutex_lock(&d->lock);
	cancel = d->cancel;
	d->cancel = CIA402_CANCEL_NONE;
	if (cancel != CIA402_CANCEL_NONE) {
		d->controlword_pending = 0u;
		if (cancel == CIA402_CANCEL_REQUEST || cancel == CIA402_CANCEL_MANUAL) {
			d->running = CIA402_DEVICE_REQ_NONE;
			d->state = CIA402_ST_IDLE;
		}
	}
	pthread_mutex_unlock(&d->lock);

	if (cancel == CIA402_CANCEL_REQUEST || cancel == CIA402_CANCEL_MANUAL) {
		return;
	}
	if (cancel == CIA402_CANCEL_TIMEOUT) {
		if (d->running == CIA402_DEVICE_REQ_NONE) {
			monitor_skip(d);
		} else if (d->running != CIA402_DEVICE_REQ_QUICK_STOP ||
			   cia402_device_seq_on_failure(d) == 0) {
			cia402_device_finish(d, -1, result->abort_code);
		}
		return;
	}

	if (d->controlword_pending != 0u) {
		if (result->status == 0) {
			d->controlword = d->pending_controlword;
		}
		d->controlword_pending = 0u;
	}

	if (result->status != 0) {
		if (d->running == CIA402_DEVICE_REQ_NONE) {
			monitor_skip(d);
			return;
		}
		if (d->running != CIA402_DEVICE_REQ_PP && cia402_device_seq_on_failure(d) != 0) {
			return;
		}
		cia402_device_finish(d, -1, result->abort_code);
		return;
	}

	switch (d->running) {
	case CIA402_DEVICE_REQ_NONE:
		monitor_complete(d, result);
		break;
	case CIA402_DEVICE_REQ_PP:
		cia402_device_pp_complete(d, result);
		break;
	default:
		cia402_device_seq_complete(d, result);
		break;
	}
}

static void monitor_start(struct cia402_device *d)
{
	static const uint16_t kIndex[4] = {CIA402_OD_STATUSWORD, CIA402_OD_POSITION_ACTUAL,
					   CIA402_OD_VELOCITY_ACTUAL, CIA402_OD_ERROR_CODE};

	if (d->mon_slot == 0u &&
	    (uint32_t)(d->now_ms - d->mon_last_ms) < CIA402_DEVICE_MON_INTERVAL_MS) {
		return;
	}
	d->mon_last_ms = d->now_ms;
	cia402_device_start_read(d, CIA402_ST_MONITOR_READ, kIndex[d->mon_slot],
				 CIA402_DEVICE_MON_TIMEOUT_MS);
}

static void monitor_complete(struct cia402_device *d, const struct sdo_result *result)
{
	pthread_mutex_lock(&d->lock);
	switch (d->mon_slot) {
	case 0:
		d->statusword = (uint16_t)(result->value & 0xFFFFu);
		break;
	case 1:
		d->position = (int32_t)result->value;
		break;
	case 2:
		d->velocity = (int32_t)result->value;
		break;
	default:
		d->error_code = (uint16_t)(result->value & 0xFFFFu);
		break;
	}
	pthread_mutex_unlock(&d->lock);
	monitor_skip(d);
}

static void monitor_skip(struct cia402_device *d)
{
	d->mon_slot = (uint8_t)((d->mon_slot + 1u) % 4u);
	d->state = CIA402_ST_IDLE;
}
