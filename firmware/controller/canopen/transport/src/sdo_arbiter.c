/*
 * sdo_arbiter.c — 仲裁一条异步 SDO 通道的两个会话
 *
 * 控制会话最多保留一笔请求，必要时等待或取消反馈；反馈不排队。
 * 本文件只在底层通道完成后取结果，避免跨会话误取。402 等上层只看到
 * 与自己接口一致的事务语义，不需要知道另一个会话是否存在。
 *
 * Author: zager
 */

#include "sdo_arbiter_internal.h"

#include <string.h>

/* 尝试启动已登记的控制请求；底层忙时保留下一轮重试 */
static void arbiter_start_control(struct sdo_arbiter *a);

/* 把当前底层结果路由到其所属会话，并释放通道 */
static void arbiter_route_result(struct sdo_arbiter *a);

static int arbiter_control_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub,
				 uint32_t value, uint8_t size, uint32_t timeout_ms);
static int arbiter_control_take(void *ctx, struct sdo_result *result);
static void arbiter_control_abort(void *ctx);
static int arbiter_feedback_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub,
				  uint32_t value, uint8_t size, uint32_t timeout_ms);
static int arbiter_feedback_take(void *ctx, struct sdo_result *result);
static void arbiter_feedback_abort(void *ctx);

/**
 * sdo_arbiter_init - 在一条底层通道上装配两个会话
 * @a: 仲裁器对象，生命周期内地址不变
 * @client: 已完成初始化的底层客户端
 */
void sdo_arbiter_init(struct sdo_arbiter *a, struct sdo_client *client)
{
	memset(a, 0, sizeof(*a));
	a->client = client;
	a->control.start = arbiter_control_start;
	a->control.take_result = arbiter_control_take;
	a->control.abort = arbiter_control_abort;
	a->control.ctx = a;
	a->feedback.start = arbiter_feedback_start;
	a->feedback.take_result = arbiter_feedback_take;
	a->feedback.abort = arbiter_feedback_abort;
	a->feedback.ctx = a;
	a->owner = SDO_ARBITER_NONE;
}

/**
 * sdo_arbiter_control - 获取控制会话接口
 * @a: 仲裁器对象；NULL 安全
 *
 * Return: 与仲裁器同生命周期的接口；NULL 入参返回 NULL。
 */
const struct sdo_access *sdo_arbiter_control(struct sdo_arbiter *a)
{
	return (a == NULL) ? NULL : &a->control;
}

/**
 * sdo_arbiter_feedback - 获取反馈会话接口
 * @a: 仲裁器对象；NULL 安全
 *
 * Return: 与仲裁器同生命周期的接口；NULL 入参返回 NULL。
 */
const struct sdo_access *sdo_arbiter_feedback(struct sdo_arbiter *a)
{
	return (a == NULL) ? NULL : &a->feedback;
}

/**
 * sdo_arbiter_step - 路由结果并启动等待中的控制请求
 * @a: 仲裁器对象；NULL 安全
 *
 * 必须在底层 sdo_client_step() 之后调用。
 */
void sdo_arbiter_step(struct sdo_arbiter *a)
{
	if (a == NULL) {
		return;
	}
	arbiter_route_result(a);
	arbiter_start_control(a);
}

static void arbiter_route_result(struct sdo_arbiter *a)
{
	struct sdo_result result;

	if (a->owner == SDO_ARBITER_NONE || a->client->state != SDO_CLIENT_DONE) {
		return;
	}
	if (a->client->access.take_result(a->client->access.ctx, &result) != 0) {
		return;
	}
	if (a->owner == SDO_ARBITER_CONTROL) {
		a->control_result = result;
		a->control_ready = 1u;
	} else {
		a->feedback_result = result;
		a->feedback_ready = 1u;
	}
	a->owner = SDO_ARBITER_NONE;
}

static void arbiter_start_control(struct sdo_arbiter *a)
{
	const struct sdo_request *req = &a->control_request;

	if (a->control_pending == 0u || a->owner != SDO_ARBITER_NONE ||
	    a->client->state != SDO_CLIENT_IDLE) {
		return;
	}
	if (a->client->access.start(a->client->access.ctx, req->dir, req->index, req->sub,
				    req->value, req->size, req->timeout_ms) != 0) {
		a->control_pending = 0u;
		a->control_result = (struct sdo_result){.status = -1};
		a->control_ready = 1u;
		return;
	}
	a->control_pending = 0u;
	a->owner = SDO_ARBITER_CONTROL;
}

static int arbiter_control_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub,
				 uint32_t value, uint8_t size, uint32_t timeout_ms)
{
	struct sdo_arbiter *a = ctx;

	if (a == NULL || a->control_pending != 0u || a->owner == SDO_ARBITER_CONTROL ||
	    a->control_ready != 0u) {
		return -1;
	}
	a->control_request = (struct sdo_request){dir, index, sub, value, size, timeout_ms};
	a->control_pending = 1u;
	if (a->owner == SDO_ARBITER_FEEDBACK) {
		a->client->access.abort(a->client->access.ctx);
	}
	arbiter_start_control(a);
	return 0;
}

static int arbiter_control_take(void *ctx, struct sdo_result *result)
{
	struct sdo_arbiter *a = ctx;

	if (a == NULL || result == NULL) {
		return -1;
	}
	if (a->control_ready == 0u) {
		return 1;
	}
	*result = a->control_result;
	a->control_ready = 0u;
	return 0;
}

static void arbiter_control_abort(void *ctx)
{
	struct sdo_arbiter *a = ctx;

	if (a == NULL) {
		return;
	}
	a->control_pending = 0u;
	if (a->owner == SDO_ARBITER_CONTROL) {
		a->client->access.abort(a->client->access.ctx);
	}
}

static int arbiter_feedback_start(void *ctx, uint8_t dir, uint16_t index, uint8_t sub,
				  uint32_t value, uint8_t size, uint32_t timeout_ms)
{
	struct sdo_arbiter *a = ctx;

	if (a == NULL || a->owner != SDO_ARBITER_NONE || a->control_pending != 0u ||
	    a->control_ready != 0u || a->feedback_ready != 0u ||
	    a->client->state != SDO_CLIENT_IDLE) {
		return -1;
	}
	if (a->client->access.start(a->client->access.ctx, dir, index, sub, value, size,
				    timeout_ms) != 0) {
		return -1;
	}
	a->owner = SDO_ARBITER_FEEDBACK;
	return 0;
}

static int arbiter_feedback_take(void *ctx, struct sdo_result *result)
{
	struct sdo_arbiter *a = ctx;

	if (a == NULL || result == NULL) {
		return -1;
	}
	if (a->feedback_ready == 0u) {
		return 1;
	}
	*result = a->feedback_result;
	a->feedback_ready = 0u;
	return 0;
}

static void arbiter_feedback_abort(void *ctx)
{
	struct sdo_arbiter *a = ctx;

	if (a != NULL && a->owner == SDO_ARBITER_FEEDBACK) {
		a->client->access.abort(a->client->access.ctx);
	}
}
