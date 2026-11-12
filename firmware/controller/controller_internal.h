/*
 * controller_internal.h — 控制层私有上下文（不对外发布）
 *
 * controller.c 持有对象并提供推进入口，controller_ops.c 适配状态机能力，
 * control_api.c 翻译对外 API 并发布事实。三者共用这一份结构。
 *
 * Author: zager
 */

#ifndef CONTROL_INTERNAL_H
#define CONTROL_INTERNAL_H

#include <control/control.h>

#include <master/master.h>
#include <cia402/device.h>
#include <transport/sdo_client.h>

#include "statemachine/src/statemachine_internal.h"

#include <pthread.h>

#include <statemachine/state_types.h>

/* 字段按所有权划分：控制层持有的实体、控制线程状态、跨线程发布值。 */
struct controller {
	/* 主站、轴、SDO 通道与业务状态机的实体都归本对象持有 */
	struct master master;
	struct cats_axis axes[CATS_MAX_AXES];
	struct sdo_client sdo[CATS_MAX_AXES];
	uint8_t axis_count;
	struct controller_sm stm;

	/* 跨线程共享：事件队列自带锁，发布值由 lock 保护 */
	pthread_mutex_t lock;
	int published_state;
	int published_fault;
	struct cats_axis_sample published_axes[CATS_MAX_AXES];
};

/* 取当前时刻；未接管主站时返回 0（I/O/服务线程用） */
uint32_t now_ms(struct controller *app);

/* 装配业务状态机并注入主站与时钟能力（仅初始化阶段调用） */
void controller_stm_init(struct controller *app);

/* 控制层单例（由 controller.c 创建；同层另一文件通过它拿上下文） */
struct controller *control_ctx(void);

/* 把其他线程可以看的值发布出去（只在控制线程里调） */
void controller_publish(void);

#endif /* CONTROL_INTERNAL_H */
