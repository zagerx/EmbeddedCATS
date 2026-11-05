/*
 * state_preparing.c — 准备中
 *
 * 进入：广播 Reset Communication（0x82，只复位通信，不动配置）。
 * 每拍：静默期过后心跳正常 → CONFIGURING；超时 → 置 BOOTUP_TIMEOUT（由 ROOT 迁 FAULT）。
 * 退出：无动作。
 *
 * 静默期：发完 0x82 后等这么久再判心跳，让驱动器有机会先处理复位并回 boot-up
 * （boot-up 会把消费者槽位置回 UNKNOWN，之后必须收到新心跳才算 ACTIVE）。
 *
 * 注意：本值（100 ms）小于 OD 0x1016 的超时值（750 ms），所以它并不保证
 * "复位前那一帧残留的 ACTIVE 已经过期"。若驱动器回 boot-up 晚于本值，
 * 判定会用上残留的 ACTIVE 而提前进 ENABLING，随后槽位变 UNKNOWN，
 * 可能在 750 ms 内被判成心跳丢失。驱动器响应足够快时不会走到这条路。
 *
 * 心跳的事实（hb_ok）由 control.c 每拍算好，本状态只读；掉线 → FAULT 的判定在
 * ONLINE（父状态）统一做。**本状态不在 ONLINE 下**，所以自己不做心跳判定。
 *
 * 时间：内核不提供定时器，用 ctl->state_since_ms（每拍由 cats_control_poll 刷新）。
 */

#include "states.h"

#define PREPARE_SETTLE_MS 100u /* 发完 0x82 先静默这么久再判心跳 */
#define PREPARE_TIMEOUT_MS 3000u /* 从进入本状态算起，这么久还等不到心跳就判失败 */

sm_ret_t st_preparing(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 0x82 = Reset Communication，站号 0 = 广播；只复位通信，
		 * 不改动对象字典里的配置 */
		(void)co_master_nmt(ctl->master, 0, 0x82);
		break;

	case SM_SIG_EXIT:
		break; /* 无出口动作 */

	case CATS_SIG_TICK: {
		uint32_t elapsed = now_ms(ctl) - ctl->state_since_ms;

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
