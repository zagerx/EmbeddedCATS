/*
 * operation_active.c — 操作进行中的公共父状态
 *
 * 这一层是全部流程状态共同继承的父状态，只做四件事：
 *   1. 收到取消信号时迁回 st_idle，结论由 st_idle 的 ENTRY 给出；
 *   2. 节拍里检查流程总期限：任何一步静默卡住都在这里兜底成超时；
 *   3. 其余 TICK 吞掉：流程推进只依赖 sdo 完成事件；
 *   4. 兜底漏派发的完成事件：报失败并迁回 st_idle。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_operation_active - 操作进行中的公共规则
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 具体操作都挂在本状态下面；Quick Stop 自行覆盖取消规则。
 *
 * Return: 取消或流程超期迁回 st_idle；其余上抛。
 */
sm_ret_t st_operation_active(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case CIA402_SIG_CANCEL:
		/* 打断在途事务由 device.c 负责，这里只退出当前流程 */
		return SM_TRAN(&st_idle);

	case CIA402_SIG_TICK:
		/*
		 * 已受理事务的结果未按约交付时，各状态的期限可能检查不到，
		 * 这里兜住整条流程的业务期限。
		 */
		if (d->operation.flow_deadline != 0u &&
		    (int32_t)(d->now_ms - d->operation.flow_deadline) >= 0) {
			dev_finish(d, CIA402_FAIL_TIMEOUT);
			return SM_TRAN(&st_idle);
		}
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		/* 子状态都应自行处理完成事件，走到这里说明漏了分支 */
		dev_fail(d, DEV_SDO_RESULT(e));
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&sm_top);
	}
}
