/*
 * state_preparing.c — 准备中
 *
 * ENTRY：清掉上一轮的上线记录，广播 Reset Communication 让两个节点一起复位；
 * TICK ：等所有节点 boot-up（2000 ms 不到齐就报 FAULT）。
 *
 * 本状态只判断 boot-up 是否到齐。
 */

#include "states.h"

#define PREPARE_TIMEOUT_MS 2000u /* 等不到 boot-up 就判准备失败 */

int st_preparing(struct hsm *me, const struct hsm_event *e)
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
		HSM_HANDLED();

	case HSM_SIG_TICK:
		now = now_ms(app);
		if (co_master_all_boot_seen(app->master)) {
			HSM_TRAN(st_running);
		}
		if (now - me->entered_ms > PREPARE_TIMEOUT_MS) {
			app->fault_code = CATS_FAULT_BOOTUP_TIMEOUT;
			HSM_TRAN(st_fault);
		}
		HSM_HANDLED();

	default:
		HSM_IGNORED();
	}
}
