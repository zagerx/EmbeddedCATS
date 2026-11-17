/*
 * state_fault.c — 保持业务故障终态并丢弃运行命令
 *
 * 故障快照由触发位置写入 controller_info.fault；本状态没有退出事件，只能重启程序。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_fault - 处理故障态入口、命令和节拍
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: MOVE、HALT 和 TICK 被消费；其他事件上抛 ROOT。
 */
sm_ret_t st_fault(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 进终态：丢掉还没处理的命令，别让它们留到重启之后 */
		controller_evtq_clear(&ctl->evtq);
		break;

	case SM_SIG_EXIT:
		/* 无出口动作 */
		break;

	case CONTROLLER_SIG_MOVE:
	case CONTROLLER_SIG_HALT:
		return SM_HANDLED(); /* 终态：命令直接丢弃 */

	case CONTROLLER_SIG_TICK:
		/* 终态吃掉节拍：不往上抛，免得 ROOT 再判一次故障（自迁移）*/
		return SM_HANDLED();

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
