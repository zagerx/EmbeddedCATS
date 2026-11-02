/*
 * state_preparing.c — 准备中
 *
 * 进入：清上线记录 + 广播 Reset Communication；
 * 每拍：所有节点 boot-up 到齐 → RUNNING；超 2000 ms → FAULT；
 * 退出：无动作。
 *
 * 超时用 me->entered_ms 手算（内核不再提供定时器）。
 */

#include "states.h"

#define PREPARE_TIMEOUT_MS 2000u /* 等不到 boot-up 就判准备失败 */

static int st_preparing_h(struct hsm *me, const struct hsm_event *e);
HSM_STATE_DEF(st_preparing, &st_operational, "PREPARING");

static int st_preparing_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);
	uint32_t now;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* 清空上线记录，避免启动判定沿用陈旧数据 */
		co_master_reset_seen(app->master);
		/* 0x82 = Reset Communication，站号 0 = 广播；只复位通信，
		 * 不改动对象字典里的配置 */
		(void)co_master_nmt(app->master, 0, 0x82);
		break;

	case HSM_SIG_EXIT:
		/* 无出口动作 */
		break;

	case CATS_SIG_TICK:
		now = now_ms(app);
		if (co_master_all_boot_seen(app->master)) {
			HSM_TRAN(&st_running);
		}
		if (now - me->entered_ms > PREPARE_TIMEOUT_MS) {
			app->fault_code = CATS_FAULT_BOOTUP_TIMEOUT;
			HSM_TRAN(&st_fault);
		}
		break;

	default:
		HSM_SUPER();
	}

	return HSM_RET_HANDLED;
}
