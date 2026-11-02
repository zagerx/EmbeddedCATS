/*
 * state_starting.c — 启动中
 *
 * ENTRY：清掉上一轮的上线记录，广播 Reset Communication 让两个节点一起复位；
 * TICK ：等所有节点 boot-up（2000 ms 不到齐就报 FAULT）；
 * STOP ：随时可取消。
 *
 * 本状态只看 boot-up，不判心跳：节点没起来和没心跳是同一件事，分开报没意义。
 */

#include "states.h"

#define STARTUP_TIMEOUT_MS 2000u /* 等不到 boot-up 就判启动失败 */

int st_starting(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);
	uint32_t now;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* 旧记录若不清理，上一轮的“已上线”会让启动一步跨过 */
		co_master_reset_seen(app->master);
		/* 0x82 = Reset Communication，站号 0 = 广播。
		 * 用 0x82 而不是 0x81：只复位通信，不冲掉对象字典里的配置。 */
		(void)co_master_nmt(app->master, 0, 0x82);
		HSM_HANDLED();

	case HSM_SIG_TICK:
		now = now_ms(app);
		if (co_master_all_boot_seen(app->master)) {
			HSM_TRAN(st_running);
		}
		if (now - me->entered_ms > STARTUP_TIMEOUT_MS) {
			app->fault_code = CATS_FAULT_BOOTUP_TIMEOUT;
			HSM_TRAN(st_fault);
		}
		HSM_HANDLED();

	case HSM_SIG_STOP:
		HSM_TRAN(st_stopping);

	default:
		HSM_IGNORED();
	}
}
