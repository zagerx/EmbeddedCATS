/*
 * statemachine.c — 管理业务事件入队、周期推进和事实查询
 *
 * 状态机只由控制线程推进，外部线程通过自带锁的事件队列提交命令。
 * 本层只读取轴事实并下发轴请求，不接触 CANopen 或传输协议。
 *
 * Author: zager
 */

#include "statemachine_internal.h"

#include "states/states.h"

#include <stddef.h>
#include <string.h>

static struct stm s_app;

/**
 * stm_instance - 获取进程内业务状态机单例
 *
 * Return: 静态状态机对象指针。
 */
struct stm *stm_instance(void)
{
	return &s_app;
}

/**
 * stm_now_ms - 读取注入给状态机的当前时刻
 * @a: 状态机对象；NULL 安全
 *
 * Return: 注入时钟的毫秒计数；对象或时钟回调为空时返回 0。
 */
uint32_t stm_now_ms(struct stm *a)
{
	return (a != NULL && a->ops.now_ms != NULL) ? a->ops.now_ms(a->ops.ctx) : 0u;
}

/**
 * stm_init - 装配并初始化业务状态机
 * @a: 状态机对象；NULL 时直接返回
 * @axes: 轴数组，由组合根持有
 * @axis_count: 轴数量
 * @ops: NMT 与时钟回调；NULL 时保留零初始化回调
 */
void stm_init(struct stm *a, struct cats_axis *axes, uint8_t axis_count, const struct stm_ops *ops)
{
	if (a == NULL) {
		return;
	}
	memset(a, 0, sizeof(*a));
	a->axes = axes;
	a->axis_count = axis_count;
	if (ops != NULL) {
		a->ops = *ops;
	}
	cats_evtq_init(&a->evtq);
	/* initial() 返回 SM_TRAN(&st_preparing)：内核把 top → root → preparing 的
	 * ENTRY 链走一遍，广播复位从 PREPARING 的 ENTRY 里发出 */
	sm_hsm_init(&a->hsm, &initial);
	a->state_since_ms = stm_now_ms(a);
}

/**
 * stm_poll - 消费外部事件并推进一次周期节拍
 * @a: 状态机对象；NULL 时直接返回
 * @hb_ok: 本拍所有被监控轴的心跳事实
 * @now_ms: 当前单调毫秒时刻
 *
 * 只能由控制线程调用；每拍消费事件数受 CATS_EVTQ_DRAIN_MAX 限制。
 */
void stm_poll(struct stm *a, uint8_t hb_ok, uint32_t now_ms)
{
	static const struct sm_event evt_tick = {CATS_SIG_TICK};
	struct cats_evt evt;
	sm_state_t before;
	unsigned n;

	if (a == NULL) {
		return;
	}
	a->hb_ok = hb_ok;

	/* 停止和急停插在队头，外部事件先于本拍节拍处理。 */
	for (n = 0u; n < CATS_EVTQ_DRAIN_MAX; n++) {
		if (!cats_evtq_pop(&a->evtq, &evt)) {
			break;
		}
		before = sm_hsm_state(&a->hsm);
		sm_hsm_dispatch(&a->hsm, &evt.super);
		if (sm_hsm_state(&a->hsm) != before) {
			a->state_since_ms = now_ms;
		}
	}

	/* 外部事件处理完后投递一次周期节拍。 */
	before = sm_hsm_state(&a->hsm);
	sm_hsm_dispatch(&a->hsm, &evt_tick);
	if (sm_hsm_state(&a->hsm) != before) {
		a->state_since_ms = now_ms;
	}
}

/**
 * stm_command - 将业务命令按值放入事件队列
 * @a: 状态机对象
 * @kind: 命令类别
 * @mv: 位置参数数组；MOVE 必填，其他命令传 NULL
 * @count: @mv 的条数（1..CATS_MAX_AXES）；其他命令传 0
 *
 * Return: CATS_OK 已入队；CATS_ERR_BUSY 队列满；CATS_ERR_PARAM 参数错误；
 * CATS_ERR_STATE 状态机为空。
 */
enum cats_result stm_command(struct stm *a, enum stm_cmd kind, const struct stm_move *mv,
			     uint8_t count)
{
	struct cats_evt e;
	uint8_t i;

	if (a == NULL) {
		return CATS_ERR_STATE;
	}
	memset(&e, 0, sizeof(e));

	switch (kind) {
	case STM_CMD_MOVE:
		if (mv == NULL || count == 0u || count > CATS_MAX_AXES) {
			return CATS_ERR_PARAM;
		}
		e.super.sig = CATS_SIG_MOVE;
		e.u.move.count = count;
		for (i = 0u; i < count; i++) {
			e.u.move.axis[i] = mv[i];
		}
		/* 排到队尾：多条指令按顺序保留；队列满则本条被丢弃 */
		return cats_evtq_push(&a->evtq, &e) ? CATS_OK : CATS_ERR_BUSY;

	case STM_CMD_STOP:
		e.super.sig = CATS_SIG_STOP;
		cats_evtq_push_front(&a->evtq, &e); /* 插队：优先于已排队的命令 */
		return CATS_OK;

	case STM_CMD_ESTOP:
		e.super.sig = CATS_SIG_ESTOP;
		cats_evtq_push_front(&a->evtq, &e);
		return CATS_OK;

	default:
		return CATS_ERR_PARAM;
	}
}

/**
 * stm_state - 将当前 HSM 叶子状态映射为业务状态
 * @a: 状态机对象；NULL 安全
 *
 * Return: 当前业务状态；对象为空或状态未注册时返回 PREPARING。
 */
enum cats_state stm_state(const struct stm *a)
{
	int i;

	if (a == NULL) {
		return CATS_STATE_PREPARING;
	}
	for (i = 0; i < CATS_STATE_COUNT; i++) {
		if (states[i] == sm_hsm_state(&a->hsm)) {
			return (enum cats_state)i;
		}
	}
	return CATS_STATE_PREPARING;
}

/**
 * stm_fault - 读取当前业务故障码
 * @a: 状态机对象；NULL 安全
 *
 * Return: 当前故障码；对象为空时返回 CATS_FAULT_NONE。
 */
enum cats_fault stm_fault(const struct stm *a)
{
	return (a == NULL) ? CATS_FAULT_NONE : a->fault_code;
}
