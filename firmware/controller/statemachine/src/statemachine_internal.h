/*
 * statemachine_internal.h — 业务状态机的内部对象（不对外）
 *
 * 状态逻辑在 states/state_*.c（一状态一文件），机制在 hsm/sm_hsm.c，
 * 事件队列在 evt_queue/。本文件把它们组成一个 struct controller_sm。
 * 完整定义还对组合根可见，供其内嵌存储；字段操作仍由状态机内部完成。
 *
 * 与 control 的边界：control 负责装配（总线/主站/驱动/轴）与发布；
 * app 只拿轴的事实、只往轴上发请求，不知道 canopen。
 */

#ifndef STATEMACHINE_INTERNAL_H
#define STATEMACHINE_INTERNAL_H

#include <statemachine/statemachine.h>

#include <axis/axis.h>

#include <stdint.h>

#include "evt_queue/evt_queue.h"
#include <hsm/sm_hsm.h>

/* 业务事件：内核只认 EMPTY/ENTRY/EXIT/INIT，业务信号从 SM_SIG_USER 起编号 */
enum cats_sig {
	CATS_SIG_TICK = SM_SIG_USER, /* 周期节拍，controller_poll() 每拍投递 */
	CATS_SIG_MOVE,               /* 位置指令（载荷见 cats_evt.u.move） */
	CATS_SIG_STOP,               /* 停止：减速停（6040 bit8） */
	CATS_SIG_ESTOP               /* 急停：quick stop → 断使能 */
};

/*
 * 业务状态机主体。
 * 全部字段只由控制线程读写（事件入队除外，evtq 自带锁）。
 */
struct controller_sm {
	/* ---- 装配期写入，之后只读 ---- */
	struct cats_axis *axes; /* 只借用：轴归组合根所有 */
	uint8_t axis_count;
	struct stm_ops ops; /* 组合根注入：NMT 广播 + 时钟 */

	/* ---- 控制线程私有 ---- */
	uint8_t hb_ok;             /* 本拍所有轴心跳都正常；状态只读不算 */
	struct cats_move_cmd move; /* 当前运动事件的参数副本 */
	uint8_t move_done;         /* 本批已到位的轴（bit = move.axis[] 下标） */
	struct sm_hsm hsm;         /* 纯状态机机制 */
	uint32_t state_since_ms;   /* 进入当前状态的时刻 */
	enum cats_fault fault_code;

	/* ---- 跨线程：事件队列（自带锁，多生产者 / 单消费者） ---- */
	struct cats_evtq evtq;
};

/* states.c 定义的状态表：下标 = enum cats_state，值 = 状态函数 */
extern const sm_state_t states[CATS_STATE_COUNT];

/* 取当前时刻（走组合根注入的时钟；未注入返回 0） */
uint32_t stm_now_ms(struct controller_sm *a);

#endif /* STATEMACHINE_INTERNAL_H */
