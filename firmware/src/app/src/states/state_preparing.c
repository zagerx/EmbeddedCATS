/*
 * state_preparing.c — 准备中
 *
 * ENTRY：清掉上一轮的上线记录，广播 Reset Communication 让两个节点一起复位；
 *        并 arm 一个 2000 ms 定时器（超时由内核投 CATS_SIG_BOOT_TIMEOUT）；
 * TICK ：所有节点 boot-up 到齐就进 RUNNING；
 * EXIT ：撤掉定时器，免得在别的状态里冒出超时事件。
 *
 * 本状态只判断 boot-up 是否到齐。
 */

#include "states.h"

#define PREPARE_TIMEOUT_MS 2000u /* 等不到 boot-up 就判准备失败 */

static int st_preparing_h(struct hsm *me, const struct hsm_event *e);
HSM_STATE_DEF(st_preparing, &st_operational, "PREPARING");

static int st_preparing_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* 清空上线记录，避免启动判定沿用陈旧数据 */
		co_master_reset_seen(app->master);
		/* 0x82 = Reset Communication，站号 0 = 广播；只复位通信，
		 * 不改动对象字典里的配置 */
		(void)co_master_nmt(app->master, 0, 0x82);
		hsm_arm(me, CATS_SIG_BOOT_TIMEOUT, 0, PREPARE_TIMEOUT_MS, now_ms(app));
		HSM_HANDLED();

	case CATS_SIG_TICK:
		if (co_master_all_boot_seen(app->master)) {
			HSM_TRAN(&st_running);
		}
		HSM_HANDLED();

	case CATS_SIG_BOOT_TIMEOUT:
		app->fault_code = CATS_FAULT_BOOTUP_TIMEOUT;
		HSM_TRAN(&st_fault);

	case HSM_SIG_EXIT:
		hsm_disarm(me);
		HSM_HANDLED();

	default:
		HSM_SUPER();
	}
}
