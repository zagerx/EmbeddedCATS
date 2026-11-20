/*
 * state_preparing.c — 复位节点并等待驱动器重启
 *
 * NMT 复位命令没有应答；后续对象访问的失败与超时由各配置流程报告。
 * 本状态不再把心跳作为启动或故障迁移条件。
 *
 * 复位节点会让驱动器整机重启：实测（2026-11-20，5 次样本）从收到命令到
 * 发出 boot-up 帧需 138~168 ms。窗口内发出的 SDO 不会被拒绝，而是排队后
 * 延迟应答，因此不能靠"发早了自然失败"兜底，必须等够时间再配置。
 *
 * Author: zager
 */

#include "states.h"

#define PREPARE_RESET_DWELL_MS 250u /* 驱动器重启实测 138~168 ms，留余量 */

/**
 * st_preparing - 复位节点后等驱动器重启，再进入配置
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 驻留结束后迁移 CONFIGURING；其余事件按层次上抛。
 */
sm_ret_t st_preparing(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 复位节点（广播所有节点）：驱动器按存储值重载应用参数，
		 * 位置不受影响（6064 复位前后一致，实测 2026-11-20）。
		 * 驱动器重启期间不在线：此时发出的 SDO 会被排队后乱序应答，
		 * 把后续配置搅成 abort，所以整段复位期间不发起任何对象访问。 */
		ctl->node_ready = 0u;
		(void)master_request_nmt(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_RESET_NODE);
		break;

	case SM_SIG_EXIT:
		ctl->node_ready = 1u; /* 驱动器已重启完毕，允许发起对象访问 */
		break;

	case CONTROLLER_SIG_TICK:
		if (controller_now_ms(ctl) - ctl->state_since_ms < PREPARE_RESET_DWELL_MS) {
			return SM_HANDLED(); /* 驱动器还在重启，不发起任何对象访问 */
		}
		return SM_TRAN(&st_configuring);

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
