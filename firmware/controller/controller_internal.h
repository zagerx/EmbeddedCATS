/*
 * controller_internal.h — 控制层私有上下文与状态图契约
 *
 * controller.c 持有对象并提供推进入口，controller_ops.c 适配状态机能力，
 * control_api.c 翻译对外 API 并发布事实，states/ 实现状态图。
 *
 * Author: zager
 */

#ifndef CONTROL_INTERNAL_H
#define CONTROL_INTERNAL_H

#include <control/control.h>

#include <master/master.h>
#include <axis/axis.h>
#include <cia402/device.h>
#include <transport/sdo_client.h>

#include "evt_queue.h"

#include <pthread.h>

/* 状态机向组合根请求的能力 */
struct stm_ops {
	void (*nmt_start)(void *ctx);
	void (*nmt_reset_comm)(void *ctx);
	uint32_t (*now_ms)(void *ctx);
	void *ctx;
};

/* 控制层与状态图之间的事件 */
enum cats_sig {
	CATS_SIG_TICK = SM_SIG_USER,
	CATS_SIG_MOVE,
	CATS_SIG_STOP,
	CATS_SIG_ESTOP
};

/* 控制器的对象实体、状态图上下文和跨线程发布值集中在这里 */
struct controller {
	struct master master;
	struct axis axes[CATS_MAX_AXES];
	uint8_t axis_count;

	struct stm_ops ops;
	uint8_t hb_ok;
	struct cats_move_cmd move;
	uint8_t move_done;
	struct sm_hsm hsm;
	uint32_t state_since_ms;
	enum cats_fault fault_code;
	struct cats_evtq evtq;

	pthread_mutex_t lock;
	int published_state;
	int published_fault;
	struct cats_axis_sample published_axes[CATS_MAX_AXES];
};

/* 状态图注册表与名称映射，由 states/states.c 定义 */
extern const sm_state_t states[CATS_STATE_COUNT];
sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e);
const char *stm_state_name(enum cats_state s);
const char *stm_fault_name(enum cats_fault f);

/* 取当前时刻；未接管主站时返回 0（I/O/服务线程用） */
uint32_t now_ms(struct controller *app);

/* 装配状态图并注入主站与时钟能力（仅初始化阶段调用） */
void controller_stm_init(struct controller *app);

/* 控制层单例（由 controller.c 创建；同层另一文件通过它拿上下文） */
struct controller *control_ctx(void);

/* 把其他线程可以看的值发布出去（只在控制线程里调） */
void controller_publish(void);

#endif /* CONTROL_INTERNAL_H */
