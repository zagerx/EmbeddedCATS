/*
 * state_preparing.c — 发起通信复位并开始轴配置
 *
 * NMT 复位命令没有应答；后续对象访问的失败与超时由各配置流程报告。
 * 本状态不再把心跳作为启动或故障迁移条件。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_preparing - 复位通信后进入配置
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 首次节拍迁移 CONFIGURING；其余事件按层次上抛。
 */
sm_ret_t st_preparing(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller *ctl = container_of(me, struct controller, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 复位通信（广播所有节点）：只复位通信层，不改动对象字典里的配置 */
		(void)master_request_nmt(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_RESET_COMM);
		break;

	case SM_SIG_EXIT:
		break; /* 无出口动作 */

	case CONTROLLER_SIG_TICK:
		return SM_TRAN(&st_configuring);

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
