/*
 * controller_log.c — 请求未执行与取消的统一日志
 *
 * Author: zager
 */

#include "controller_log.h"

#include <stdio.h>
#include <time.h>

static const char *request_name(enum controller_sig sig);
static const char *reason_name(enum controller_not_executed reason);
static const char *state_name(enum controller_state state);

/**
 * controller_log_not_executed - 记录没有产生动作的请求
 * @ctl: 控制上下文；NULL 时无法取得当前状态
 * @sig: 请求类型
 * @axis: 轴号；NO_AXIS 表示整机或未定位
 * @reason: 未执行原因，不作为故障上报
 */
void controller_log_not_executed(struct controller *ctl, enum controller_sig sig, uint8_t axis,
				 enum controller_not_executed reason)
{
	enum controller_state state = CONTROLLER_STATE_COUNT;
	struct timespec ts;

	if (ctl != NULL) {
		pthread_mutex_lock(&ctl->info_lock);
		state = ctl->info.state;
		pthread_mutex_unlock(&ctl->info_lock);
	}
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr,
		"[controller][%lld.%03ld] 请求未执行: request=%s, axis=%s, state=%s, reason=%s\n",
		(long long)ts.tv_sec, ts.tv_nsec / 1000000L, request_name(sig),
		(axis < CONTROLLER_AXIS_COUNT) ? (axis == 0u ? "0" : "1") : "ALL",
		state_name(state), reason_name(reason));
}

/**
 * controller_log_queue_cleared - 记录终态清掉的未处理命令
 * @ctl: 控制上下文；NULL 安全
 *
 * 只在控制线程清空队列时调用。
 */
void controller_log_queue_cleared(struct controller *ctl)
{
	struct controller_evt evt;

	if (ctl == NULL) {
		return;
	}
	while (controller_evtq_pop(&ctl->evtq, &evt)) {
		controller_log_not_executed(
			ctl, (enum controller_sig)evt.super.sig,
			controller_log_event_axis(&evt, CONTROLLER_FAULT_NO_AXIS),
			CONTROLLER_NOT_EXECUTED_TERMINAL);
	}
}

/**
 * controller_log_event_axis - 从请求中取出日志用轴号
 * @evt: 已入队的命令；NULL 安全
 * @fallback: 整机命令或无载荷时的轴号
 *
 * Return: 轴号；多轴命令使用 fallback。
 */
uint8_t controller_log_event_axis(const struct controller_evt *evt, uint8_t fallback)
{
	if (evt == NULL) {
		return fallback;
	}
	if (evt->super.sig == CONTROLLER_SIG_MOVE && evt->u.move.count == 1u) {
		return evt->u.move.axis[0].axis;
	}
	if (evt->super.sig == CONTROLLER_SIG_HOME) {
		return evt->u.home.axis;
	}
	return fallback;
}

/**
 * controller_log_fault - 记录真实的执行失败
 * @stage: 失败时的业务状态
 * @reason: 已归类的失败原因
 * @axis: 失败轴；NO_AXIS 表示未定位
 * @abort_code: SDO abort 码；无证据时传 0
 */
void controller_log_fault(enum controller_state stage, enum controller_fault_reason reason,
			  uint8_t axis, uint32_t abort_code)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr,
		"[controller][%lld.%03ld] 执行失败: state=%s, axis=%u, reason=%d, abort=0x%08X\n",
		(long long)ts.tv_sec, ts.tv_nsec / 1000000L, state_name(stage), (unsigned)axis,
		(int)reason, (unsigned)abort_code);
}

static const char *request_name(enum controller_sig sig)
{
	switch (sig) {
	case CONTROLLER_SIG_TICK:
		return "CONFIG";
	case CONTROLLER_SIG_MOVE:
		return "MOVE";
	case CONTROLLER_SIG_HALT:
		return "HALT";
	case CONTROLLER_SIG_ESTOP:
		return "ESTOP";
	case CONTROLLER_SIG_ESTOP_CLEAR:
		return "ESTOP_CLEAR";
	case CONTROLLER_SIG_ENABLE:
		return "ENABLE";
	case CONTROLLER_SIG_HOME:
		return "HOME";
	default:
		return "UNKNOWN";
	}
}

static const char *state_name(enum controller_state state)
{
	switch (state) {
	case CONTROLLER_STATE_PREPARING:
		return "PREPARING";
	case CONTROLLER_STATE_CONFIGURING:
		return "CONFIGURING";
	case CONTROLLER_STATE_IDLE:
		return "IDLE";
	case CONTROLLER_STATE_ENABLING:
		return "ENABLING";
	case CONTROLLER_STATE_HOLDING:
		return "HOLDING";
	case CONTROLLER_STATE_MOVING:
		return "MOVING";
	case CONTROLLER_STATE_HALTING:
		return "HALTING";
	case CONTROLLER_STATE_FAULT:
		return "FAULT";
	case CONTROLLER_STATE_ESTOP:
		return "ESTOP";
	case CONTROLLER_STATE_HOMING:
		return "HOMING";
	default:
		return "NOT_INITIALIZED";
	}
}

static const char *reason_name(enum controller_not_executed reason)
{
	switch (reason) {
	case CONTROLLER_NOT_EXECUTED_QUEUE_FULL:
		return "QUEUE_FULL";
	case CONTROLLER_NOT_EXECUTED_INVALID_ARGUMENT:
		return "INVALID_ARGUMENT";
	case CONTROLLER_NOT_EXECUTED_INVALID_STATE:
		return "INVALID_STATE";
	case CONTROLLER_NOT_EXECUTED_DEVICE_BUSY:
		return "DEVICE_BUSY";
	case CONTROLLER_NOT_EXECUTED_NOT_INITIALIZED:
		return "NOT_INITIALIZED";
	case CONTROLLER_NOT_EXECUTED_TERMINAL:
		return "TERMINAL_STATE";
	case CONTROLLER_NOT_EXECUTED_ALREADY_HOLDING:
		return "ALREADY_HOLDING";
	default:
		return "UNKNOWN";
	}
}
