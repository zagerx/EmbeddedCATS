/*
 * states.c — 设备状态机的入口与层次约定
 *
 * 层次只有两层：sm_top → st_operation_active → 具体操作。
 * 每个操作只占一个状态，内部按 operation.stage 顺序推进（见 operations/）。
 * st_idle 直接挂在 sm_top 下：它不属于任何操作，
 * 因此不继承 st_operation_active 的「被抢占」语义。
 *
 * Author: zager
 */

#include "states.h"

/**
 * device_initial - 顶层初始转移
 * @me: 状态机内核对象
 * @e: 信号为 SM_SIG_INIT 的保留事件
 *
 * Return: 转移目标 st_idle。
 */
sm_ret_t device_initial(struct sm_hsm *const me, const struct sm_event *const e)
{
	(void)e;

	return SM_TRAN(&st_idle);
}
