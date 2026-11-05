/*
 * cia402.c — 对外接口：初始化、请求动作、每拍推进、事实查询
 *
 * 本文件只做两件事：
 *   1. 把"当前在推哪个协议过程"记在一个变量里（n->activity），每拍分派给子模块；
 *   2. 把上层的意图翻译成"能不能受理"，细节一概交给子模块：
 *        参数配置 → cia402_config.c
 *        使能序列 → cia402_enable.c
 *        PP 运动  → cia402_pp.c
 *        停车/急停 → cia402_stop.c
 *        单次 SDO → cia402_sdo.c
 *        状态字   → cia402_state.c
 *
 * 同一时刻只有一个过程在跑——一条 SDO 通道只能串行用，这是协议事实，
 * 不是本层的取舍。急停例外：它抢占任何过程（安全动作不能被排队挡住）。
 */

#include "cia402_internal.h"

#include <string.h>

/*
 * 活锁看门狗：同一个过程停留超过这个时长就兜底判失败。
 * 比各过程自己的超时都长（PP 等到位 5 s / Halt 3 s / 急停 5 s），
 * 所以它只在“相位机本身卡住”时才会先触发——而不是代替业务超时。
 */
#define CIA402_WATCHDOG_MS 10000u

void cia402_init(struct cia402_node *n, struct co_master *m, uint8_t node_id)
{
	memset(n, 0, sizeof(*n));
	n->master = m;
	n->node_id = node_id;
	n->cfg_state = CIA402_CFG_PENDING;
	n->en_state = CIA402_EN_NOT_ENABLED;
	n->state = CIA402_STATE_UNKNOWN;
	n->sdo = CIA402_SDO_IDLE;
	n->activity = CIA402_ACT_IDLE;
	n->prev_activity = CIA402_ACT_IDLE;
	n->controlword = CIA402_CW_DISABLE;
}

void cia402_finish(struct cia402_node *n, enum cia402_result r)
{
	n->activity = CIA402_ACT_IDLE;
	n->result = r;
	if (r == CIA402_RESULT_FAILED) {
		/* 钉住失败事实：结论槽被取走也不影响“这台轴出过错” */
		n->failed = 1;
	}
}

void cia402_abort_inflight(struct cia402_node *n)
{
	/* 抢占必须把在途事务丢掉；上游的 initiate 会重设客户端状态 */
	n->sdo = CIA402_SDO_IDLE;
	n->sdo_owner = CIA402_SDO_OWNER_PROCESS;
	n->monitor.await = 0; /* 被抢掉的监控读不算结果，下一圈重发 */
}

int cia402_request_config(struct cia402_node *n)
{
	if (n == NULL || n->master == NULL || n->node_id == 0U) {
		return -1;
	}
	if (n->cfg_state != CIA402_CFG_PENDING || n->activity != CIA402_ACT_IDLE) {
		return 1; /* 已配过 / 有别的过程在跑：不重复发起 */
	}
	n->abort_code = 0;
	n->failed = 0; /* 新过程开始：上一次的失败事实作废 */
	cia402_config_start(n);
	return 0;
}

int cia402_request_enable(struct cia402_node *n)
{
	if (n == NULL || n->master == NULL || n->node_id == 0U) {
		return -1;
	}
	if (n->en_state != CIA402_EN_NOT_ENABLED || n->activity != CIA402_ACT_IDLE) {
		return 1; /* 已经要求过 / 有别的过程在跑：不重复发起 */
	}
	n->abort_code = 0;
	n->failed = 0;
	cia402_enable_start(n);
	return 0;
}

int cia402_pp_start(struct cia402_node *n, int32_t target, int32_t vel, int32_t acc, int32_t dec)
{
	if (n == NULL) {
		return -1;
	}
	if (n->activity != CIA402_ACT_IDLE) {
		return 1; /* 有过程在跑：本轮不覆盖（应用层决定要不要先停） */
	}
	if (!cia402_is_enabled(n)) {
		return -1; /* 没使能就不接受运动指令 */
	}
	n->failed = 0; /* 新过程开始：上一次的失败事实作废 */
	n->activity = CIA402_ACT_MOVING;
	return cia402_pp_begin(n, target, vel, acc, dec);
}

int cia402_halt(struct cia402_node *n)
{
	if (n == NULL) {
		return -1;
	}
	if (n->activity != CIA402_ACT_IDLE && n->activity != CIA402_ACT_MOVING) {
		return 1; /* 已在停车/急停/使能里：不重复发起 */
	}
	/* 抢占运动：丢掉在途的读，立刻置 bit8（停车不能排在运动后面） */
	cia402_abort_inflight(n);
	n->failed = 0;
	n->activity = CIA402_ACT_HALTING;
	return cia402_halt_begin(n);
}

int cia402_quick_stop(struct cia402_node *n)
{
	if (n == NULL) {
		return -1;
	}
	if (n->activity == CIA402_ACT_QUICK_STOP) {
		return 1; /* 已经在急停流程里 */
	}
	/* 抢占：把在途的 SDO 事务丢掉，立刻发 0x02 */
	cia402_abort_inflight(n);
	n->failed = 0;
	n->activity = CIA402_ACT_QUICK_STOP;
	return cia402_quick_stop_begin(n);
}

void cia402_poll(struct cia402_node *n, uint32_t now_ms)
{
	uint32_t diff_us = 0;

	if (n == NULL) {
		return;
	}
	if (n->started) {
		diff_us = (uint32_t)(now_ms - n->last_ms) * 1000u; /* 无符号相减处理回绕 */
	}
	n->last_ms = now_ms;
	if (!n->started) {
		n->started = 1;
		n->activity_since_ms = now_ms;
	}

	/* 换过程就重新计时；同一个过程待太久说明相位机卡住了，兜底判失败 */
	if (n->activity != n->prev_activity) {
		n->prev_activity = n->activity;
		n->activity_since_ms = now_ms;
	}
	if (n->activity != CIA402_ACT_IDLE &&
	    (uint32_t)(now_ms - n->activity_since_ms) > CIA402_WATCHDOG_MS) {
		cia402_finish(n, CIA402_RESULT_FAILED);
		return;
	}

	cia402_sdo_step(n, diff_us); /* 先把在途 SDO 推一步 */

	switch (n->activity) {
	case CIA402_ACT_CONFIGURING:
		cia402_config_step(n);
		break;
	case CIA402_ACT_ENABLING:
		cia402_enable_step(n);
		break;
	case CIA402_ACT_MOVING:
		cia402_pp_step(n);
		break;
	case CIA402_ACT_HALTING:
		cia402_halt_step(n);
		break;
	case CIA402_ACT_QUICK_STOP:
		cia402_quick_stop_step(n);
		break;
	default:
		break; /* IDLE：没有过程在跑 */
	}

	/* 过程推完之后再排监控读：有在途事务就自己跳过，绝不插队 */
	cia402_monitor_poll(n, now_ms);
}

int cia402_busy(const struct cia402_node *n)
{
	return n != NULL && n->activity != CIA402_ACT_IDLE;
}

enum cia402_result cia402_take_result(struct cia402_node *n)
{
	enum cia402_result r;

	if (n == NULL) {
		return CIA402_RESULT_NONE;
	}
	r = n->result;
	n->result = CIA402_RESULT_NONE;
	return r;
}

int cia402_is_configured(const struct cia402_node *n)
{
	return n != NULL && n->cfg_state == CIA402_CFG_CONFIGURED;
}

int cia402_is_enabled(const struct cia402_node *n)
{
	return n != NULL && n->en_state == CIA402_EN_ENABLED && n->state == CIA402_OP_ENABLED;
}

int cia402_failed(const struct cia402_node *n)
{
	/* 任何一步失败都算“这台轴出过错”；是哪一步由上层故障码区分。
	 * 失败事实一直留着，直到下一个过程被受理时清除。 */
	return n != NULL && n->failed != 0;
}

uint32_t cia402_abort_code(const struct cia402_node *n)
{
	return n != NULL ? n->abort_code : 0U;
}
