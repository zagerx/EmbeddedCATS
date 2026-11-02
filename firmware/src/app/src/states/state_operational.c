/*
 * state_operational.c — 运行期父状态
 *
 * PREPARING / STARTING / RUNNING / STOPPING 的共同祖先，**目前不处理任何事件**：
 * 本轮只把层次搭起来，公共逻辑（心跳监控、公共下使能等）等行为定下来再往这里收。
 *
 * 注意：不要在这里写"进子状态就顺带做的事"——子状态之间迁移时 LCA 就是本状态，
 * 它的 ENTRY 不会被重复触发；但一旦从 st_fault 回到运行期，本状态会重新 ENTRY。
 */

#include "states.h"

static int st_operational_h(struct hsm *me, const struct hsm_event *e);
HSM_STATE_DEF(st_operational, &st_root, "OPERATIONAL");

static int st_operational_h(struct hsm *me, const struct hsm_event *e)
{
	(void)me;
	(void)e;
	HSM_SUPER();
}
