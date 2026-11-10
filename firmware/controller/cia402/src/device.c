/*
 * device.c — 受理 CiA 402 设备请求、执行流程并刷新反馈
 *
 * 普通请求一次只执行一个，Halt 和 Quick Stop 可抢占在途流程。
 * 服务线程同步执行请求；反馈只在设备空闲时轮转读取，避免争用对象通道。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <cia402/od.h>
#include <cia402/pds.h>

#include <stddef.h>
#include <string.h>

/* 空闲时按间隔轮转读取状态字、位置、速度和错误码 */
static void monitor_step(struct cia402_device *d, uint32_t now_ms);

/* 提交普通请求；已有请求或流程时返回忙 */
static int submit(struct cia402_device *d, enum cia402_device_req req);

/* 中断在途事务并覆盖请求槽，仅供安全动作使用 */
static int submit_urgent(struct cia402_device *d, enum cia402_device_req req);

/* 写入请求结论；被取消的流程不留下结果 */
static void finish_request(struct cia402_device *d, int ok);

/**
 * cia402_device_init - 初始化一个 CiA 402 设备实例
 * @d: 设备对象
 * @access: 对象访问接口，由调用方持有
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
	d->result = CIA402_DEVICE_RESULT_NONE;
	d->controlword = CIA402_CW_DISABLE;
}

/**
 * cia402_device_request_config - 请求配置设备
 * @d: 设备对象
 *
 * Return: 0 已受理；1 已配置、已失败或设备忙；-1 参数或装配错误。
 */
int cia402_device_request_config(struct cia402_device *d)
{
	if (d != NULL) {
		pthread_mutex_lock(&d->lock);
		if (d->cfg_state != CIA402_DEVICE_CFG_PENDING) {
			pthread_mutex_unlock(&d->lock);
			return 1; /* 已经配过 / 已失败过，不重复发起 */
		}
		pthread_mutex_unlock(&d->lock);
	}
	return submit(d, CIA402_DEVICE_REQ_CONFIG);
}

/**
 * cia402_device_request_enable - 请求使能设备
 * @d: 设备对象
 *
 * Return: 0 已受理；1 已使能、已失败或设备忙；-1 参数或装配错误。
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
 * cia402_device_request_disable - 请求断使能
 * @d: 设备对象
 *
 * Return: 0 已受理；1 设备忙；-1 参数或装配错误。
 */
int cia402_device_request_disable(struct cia402_device *d)
{
	return submit(d, CIA402_DEVICE_REQ_DISABLE);
}

/**
 * cia402_device_pp_start - 请求一次 PP 绝对位置运动
 * @d: 设备对象
 * @target: 目标位置
 * @vel: 轮廓速度
 * @acc: 轮廓加速度
 * @dec: 轮廓减速度
 *
 * Return: 0 已受理；1 设备忙；-1 设备为空或未使能。
 */
int cia402_device_pp_start(struct cia402_device *d, int32_t target, int32_t vel, int32_t acc,
			   int32_t dec)
{
	int rc;

	if (d == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->en_state != CIA402_DEVICE_EN_ENABLED) {
		pthread_mutex_unlock(&d->lock);
		return -1; /* 没使能就不接受运动指令 */
	}
	d->tgt = target;
	d->vel = vel;
	d->acc = acc;
	d->dec = dec;
	d->failed = 0u; /* 新过程开始：上次的失败事实作废 */
	pthread_mutex_unlock(&d->lock);

	rc = submit(d, CIA402_DEVICE_REQ_PP);
	return rc;
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
 * cia402_device_cancel - 取消当前对象事务和在途流程
 * @d: 设备对象；NULL 时直接返回
 */
void cia402_device_cancel(struct cia402_device *d)
{
	if (d == NULL) {
		return;
	}
	if (d->access != NULL && d->access->abort != NULL) {
		d->access->abort(d->access->ctx);
	}
	pthread_mutex_lock(&d->lock);
	d->cancel = 1u;
	pthread_mutex_unlock(&d->lock);
}

/**
 * cia402_device_service - 推进一步：受理请求、推进 PP 或刷新一次反馈
 * @d: 设备对象
 * @now_ms: 当前单调毫秒时刻
 *
 * 只能由服务线程调用。配置、使能、停止是同步流程（阻塞到出结论）；
 * PP 只在这里触发，到位判定分散到后续每拍一次读写，因此一个轴在等位时
 * 不会占住服务线程，多个轴能在同一拍里各自推进。
 */
void cia402_device_service(struct cia402_device *d, uint32_t now_ms)
{
	enum cia402_device_req req;
	int32_t t, v, a, dc;
	int used = 0; /* 本拍已用过对象通道 */

	if (d == NULL || d->access == NULL) {
		return;
	}

	pthread_mutex_lock(&d->lock);
	req = d->req;
	if (req != CIA402_DEVICE_REQ_NONE) {
		d->req = CIA402_DEVICE_REQ_NONE;
		d->running = req;
		d->cancel = 0u;
	}
	t = d->tgt;
	v = d->vel;
	a = d->acc;
	dc = d->dec;
	pthread_mutex_unlock(&d->lock);

	if (req != CIA402_DEVICE_REQ_NONE) {
		int ok = -1;

		/* 新流程接手：上一段 PP 的进度作废（含被 Halt/Quick Stop 抢占） */
		d->pp_run = 0u;
		used = 1;

		/* 下面这些调用会阻塞（等状态字/等应答），但都是睡眠不是空转 */
		switch (req) {
		case CIA402_DEVICE_REQ_CONFIG:
			ok = cia402_device_do_config(d);
			break;
		case CIA402_DEVICE_REQ_ENABLE:
			ok = cia402_device_do_enable(d);
			break;
		case CIA402_DEVICE_REQ_DISABLE:
			ok = cia402_device_do_disable(d);
			break;
		case CIA402_DEVICE_REQ_PP:
			/* 只触发就返回；到没到位由下面的 pp_step 逐拍判定 */
			ok = cia402_device_pp_begin(d, t, v, a, dc, now_ms);
			break;
		case CIA402_DEVICE_REQ_HALT:
			ok = cia402_device_do_halt(d);
			break;
		case CIA402_DEVICE_REQ_QUICK_STOP:
			ok = cia402_device_do_quick_stop(d);
			break;
		default:
			break;
		}
		if (req != CIA402_DEVICE_REQ_PP || ok != 0) {
			finish_request(d, ok); /* PP 成功触发时结论留给出位判定 */
		}
	} else if (d->pp_run != 0u) {
		int r = cia402_device_pp_step(d, now_ms);

		used = 1;
		if (r != 1) { /* 1 = 还在走，下一拍继续 */
			d->pp_run = 0u;
			finish_request(d, (r == 0) ? 0 : -1);
		}
	}

	/* 空闲才刷反馈：避免和业务事务抢同一对 COB-ID */
	if (used == 0) {
		monitor_step(d, now_ms);
	}
}

/**
 * cia402_device_is_configured - 查询设备是否已配置
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 已配置；0 未配置、配置失败或设备为空。
 */
int cia402_device_is_configured(const struct cia402_device *d)
{
	return d != NULL && d->cfg_state == CIA402_DEVICE_CFG_CONFIGURED;
}

/**
 * cia402_device_is_enabled - 查询设备是否已使能
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 已使能；0 未使能、使能失败或设备为空。
 */
int cia402_device_is_enabled(const struct cia402_device *d)
{
	return d != NULL && d->en_state == CIA402_DEVICE_EN_ENABLED;
}

/**
 * cia402_device_failed - 查询设备是否记录到失败
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 有失败记录；0 无失败记录或设备为空。
 */
int cia402_device_failed(const struct cia402_device *d)
{
	return d != NULL && d->failed != 0u;
}

/**
 * cia402_device_busy - 查询设备是否有请求待处理或在途
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 忙或设备为空；0 空闲。
 */
int cia402_device_busy(const struct cia402_device *d)
{
	if (d == NULL) {
		return 1;
	}
	return d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE;
}

/**
 * cia402_device_take_result - 取走设备请求结论
 * @d: 设备对象；NULL 安全
 *
 * Return: 当前结论并复位为 NONE；设备为空时返回 NONE。
 */
enum cia402_device_result cia402_device_take_result(struct cia402_device *d)
{
	enum cia402_device_result r;

	if (d == NULL) {
		return CIA402_DEVICE_RESULT_NONE;
	}
	pthread_mutex_lock(&d->lock);
	r = d->result;
	d->result = CIA402_DEVICE_RESULT_NONE;
	pthread_mutex_unlock(&d->lock);
	return r;
}

/**
 * cia402_device_abort_code - 读取最近一次失败对应的 abort 码
 * @d: 设备对象；NULL 安全
 *
 * Return: abort 码；无记录或设备为空时返回 0。
 */
uint32_t cia402_device_abort_code(const struct cia402_device *d)
{
	return (d == NULL) ? 0u : d->abort_code;
}

/**
 * cia402_device_feedback_statusword - 读取缓存的状态字
 * @d: 设备对象；NULL 安全
 *
 * Return: 状态字；设备为空时返回 0。
 */
uint16_t cia402_device_feedback_statusword(const struct cia402_device *d)
{
	return (d == NULL) ? 0u : d->statusword;
}

/**
 * cia402_device_feedback_position - 读取缓存的实际位置
 * @d: 设备对象；NULL 安全
 *
 * Return: 实际位置；设备为空时返回 0。
 */
int32_t cia402_device_feedback_position(const struct cia402_device *d)
{
	return (d == NULL) ? 0 : d->position;
}

/**
 * cia402_device_feedback_velocity - 读取缓存的实际速度
 * @d: 设备对象；NULL 安全
 *
 * Return: 实际速度；设备为空时返回 0。
 */
int32_t cia402_device_feedback_velocity(const struct cia402_device *d)
{
	return (d == NULL) ? 0 : d->velocity;
}

/**
 * cia402_device_feedback_error_code - 读取缓存的驱动器错误码
 * @d: 设备对象；NULL 安全
 *
 * Return: 错误码；设备为空时返回 0。
 */
uint16_t cia402_device_feedback_error_code(const struct cia402_device *d)
{
	return (d == NULL) ? 0u : d->error_code;
}

static int submit(struct cia402_device *d, enum cia402_device_req req)
{
	int rc;

	if (d == NULL || d->access == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE) {
		rc = 1; /* 有一件在处理：调用方自己决定何时重试 */
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
	if (d->access->abort != NULL) {
		d->access->abort(d->access->ctx); /* 先让阻塞的等待醒来 */
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != CIA402_DEVICE_REQ_NONE) {
		d->cancel = 1u; /* 当前流程尽快退出（结论作废） */
	}
	d->req = req;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

static void finish_request(struct cia402_device *d, int ok)
{
	int cancelled;

	pthread_mutex_lock(&d->lock);
	cancelled = d->cancel != 0u;
	d->cancel = 0u;
	if (cancelled != 0) {
		d->result = CIA402_DEVICE_RESULT_NONE;
		d->running = CIA402_DEVICE_REQ_NONE;
		pthread_mutex_unlock(&d->lock);
		return;
	}
	if (ok != 0) {
		d->abort_code = (d->access != NULL && d->access->last_error != NULL)
					? d->access->last_error(d->access->ctx)
					: 0u;
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
}

static void monitor_step(struct cia402_device *d, uint32_t now_ms)
{
	static const uint16_t kIndex[4] = {CIA402_OD_STATUSWORD, CIA402_OD_POSITION_ACTUAL,
					   CIA402_OD_VELOCITY_ACTUAL, CIA402_OD_ERROR_CODE};
	uint32_t v = 0u;
	uint8_t len = 0u;
	uint8_t slot = d->mon_slot;

	/* mon_slot 回到 0 说明四个对象都轮过一遍：按间隔刷新 */
	if (d->mon_slot == 0u &&
	    (uint32_t)(now_ms - d->mon_last_ms) < CIA402_DEVICE_MON_INTERVAL_MS) {
		return;
	}
	d->mon_last_ms = now_ms;
	if (d->access->xfer(d->access->ctx, SDO_DIR_READ, kIndex[slot], 0u, 0u, 0u, &v, &len,
			    CIA402_DEVICE_MON_TIMEOUT_MS) != 0) {
		d->mon_slot = (uint8_t)((slot + 1u) % 4u); /* 读失败保留旧值 */
		return;
	}

	pthread_mutex_lock(&d->lock);
	switch (slot) {
	case 0:
		d->statusword = (uint16_t)(v & 0xFFFFu);
		break;
	case 1:
		d->position = (int32_t)v;
		break;
	case 2:
		d->velocity = (int32_t)v;
		break;
	default:
		d->error_code = (uint16_t)(v & 0xFFFFu);
		break;
	}
	d->mon_slot = (uint8_t)((slot + 1u) % 4u);
	pthread_mutex_unlock(&d->lock);
}
