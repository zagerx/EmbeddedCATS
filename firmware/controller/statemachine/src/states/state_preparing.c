/*
 * 准备中：广播 Reset Communication，等待心跳后转 CONFIGURING。
 * 本状态不在 ONLINE 下，启动期没有心跳不算运行期掉线。
 *
 * 100 ms 静默期小于 750 ms 心跳超时，残留的 ACTIVE 可能让状态提前迁移；
 * 随后收到延迟的 boot-up 时，心跳槽会重新变为 UNKNOWN。
 */

#include "states.h"

#define PREPARE_SETTLE_MS 100u /* 发完 0x82 先静默这么久再判心跳 */
#define PREPARE_TIMEOUT_MS 3000u /* 从进入本状态算起，这么久还等不到心跳就判失败 */

sm_ret_t st_preparing(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct stm *ctl = container_of(me, struct stm, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 复位通信（广播所有节点）：只复位通信层，不改动对象字典里的配置 */
		ctl->ops.nmt_reset_comm(ctl->ops.ctx);
		break;

	case SM_SIG_EXIT:
		break; /* 无出口动作 */

	case CATS_SIG_TICK: {
		uint32_t elapsed = stm_now_ms(ctl) - ctl->state_since_ms;

		if (elapsed < PREPARE_SETTLE_MS) {
			return SM_SUPER(&st_root); /* 静默期：心跳还是复位前的旧数据 */
		}
		if (ctl->hb_ok) {
			return SM_TRAN(&st_configuring); /* 先配参数（模式等），再使能 */
		}
		if (elapsed > PREPARE_TIMEOUT_MS) {
			ctl->fault_code = CATS_FAULT_BOOTUP_TIMEOUT; /* 由 ROOT 迁 FAULT */
		}
		return SM_SUPER(&st_root);
	}

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
