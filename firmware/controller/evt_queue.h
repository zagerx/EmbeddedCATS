/*
 * evt_queue.h — 控制层的输入事件队列（多生产者 / 单消费者）
 *
 * 为什么需要：Qt 线程等外部线程不能直接碰状态机。它们把"命令"封装成事件投进来，
 * 控制线程在 controller_poll() 里逐个 sm_hsm_dispatch() 消费。
 *
 * 入队规则（**队列满时**）：
 *   · controller_evtq_push()       普通命令，满了丢弃**新的一条**，返回 false ⇒ 调用方回 BUSY；
 *   · controller_evtq_push_front() 停止/急停，插到队头；满了就替换队头那条最旧的，**永不失败**。
 *
 * 事件按**值拷贝**：sm_hsm_dispatch() 是同步的（run-to-completion），
 * 内核从不保存事件指针，所以队列里不能存指针。
 */

#ifndef CONTROL_EVT_QUEUE_H
#define CONTROL_EVT_QUEUE_H

#include <pthread.h>
#include <stdbool.h>

#include <hsm/sm_hsm.h>
#include <control/control.h>

#define CONTROLLER_EVTQ_LEN 8u       /* 队列容量 */
#define CONTROLLER_EVTQ_DRAIN_MAX 4u /* 每轮最多消费几条，限制单次事件处理时长 */

/*
 * 位置指令载荷（对应 controller_move 的参数；业务单位 mm / mm·s⁻¹ / mm·s⁻²）。
 * 一批可以是 1..CONTROLLER_MAX_AXES 条：它们一起启动，全部到位才算完成。
 */
struct controller_move_cmd {
	uint8_t count;
	struct controller_move_req axis[CONTROLLER_MAX_AXES];
};

/* 队列元素：取最大的一种事件。以后加别的带参事件就往 union 里加成员 */
struct controller_evt {
	struct sm_event super; /* 必须是第一个成员（SM_EVT_CAST 依赖这条） */
	union {
		struct controller_move_cmd move;
	} u;
};

struct controller_evtq {
	struct controller_evt buf[CONTROLLER_EVTQ_LEN];
	unsigned head;  /* 第一个元素的位置 */
	unsigned tail;  /* 最后一个元素的下一格 */
	unsigned count; /* 用 count 区分"满"和"空"，不牺牲一格 */
	pthread_mutex_t lock;
};

/* 清空队列；互斥锁随静态控制器初始化，可重复复位 */
void controller_evtq_init(struct controller_evtq *q);

/* 尾部入队（普通命令）。false = 队列满，本事件被丢弃 */
bool controller_evtq_push(struct controller_evtq *q, const struct controller_evt *e);

/* 头部入队（停止/急停）：插到队头；满时替换队头那条最旧的。永不失败 */
void controller_evtq_push_front(struct controller_evtq *q, const struct controller_evt *e);

/* 取一个事件。false = 空 */
bool controller_evtq_pop(struct controller_evtq *q, struct controller_evt *out);

/* 清空队列（进入 FAULT / ESTOP 时用：别让恢复后跑旧命令） */
void controller_evtq_clear(struct controller_evtq *q);

#endif /* CONTROL_EVT_QUEUE_H */
