/*
 * evt_queue.c — 实现定长多生产者、单消费者环形事件队列
 *
 * head 指向首个元素，tail 指向下一个写入位置，count 用于区分空与满。
 * 事件按值保存，队列锁只保护索引、计数和元素拷贝。
 *
 * Author: zager
 */

#include "evt_queue.h"

/**
 * controller_evtq_init - 清空事件队列
 * @q: 队列对象；NULL 时直接返回
 *
 * 互斥锁随静态控制器初始化；重复装配只复位队列内容。
 */
void controller_evtq_init(struct controller_evtq *q)
{
	if (q == NULL) {
		return;
	}
	pthread_mutex_lock(&q->lock);
	q->head = 0u;
	q->tail = 0u;
	q->count = 0u;
	pthread_mutex_unlock(&q->lock);
}

/**
 * controller_evtq_push - 将普通事件放入队尾
 * @q: 队列对象
 * @e: 待拷贝事件
 *
 * Return: true 入队成功；false 队列满或参数为 NULL。
 */
bool controller_evtq_push(struct controller_evtq *q, const struct controller_evt *e)
{
	bool ok = true;

	if (q == NULL || e == NULL) {
		return false;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count == CONTROLLER_EVTQ_LEN) {
		ok = false; /* 满：丢弃新的这一条，绝不覆盖已有的 */
	} else {
		q->buf[q->tail] = *e;
		q->tail = (q->tail + 1u) % CONTROLLER_EVTQ_LEN;
		q->count++;
	}
	pthread_mutex_unlock(&q->lock);
	return ok;
}

/**
 * controller_evtq_push_front - 将停止或急停事件插入队头
 * @q: 队列对象；NULL 时直接返回
 * @e: 待拷贝事件；NULL 时直接返回
 *
 * 队列满时替换当前队头，保证新事件优先被取出。
 */
void controller_evtq_push_front(struct controller_evtq *q, const struct controller_evt *e)
{
	if (q == NULL || e == NULL) {
		return;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count < CONTROLLER_EVTQ_LEN) {
		q->head = (q->head + CONTROLLER_EVTQ_LEN - 1u) % CONTROLLER_EVTQ_LEN;
		q->buf[q->head] = *e;
		q->count++;
	} else {
		/*
		 * 满：head 正好是队头（最旧的那条）。直接替换它 ⇒
		 * 本事件成为第一个被弹出的，只牺牲了一条最旧的命令。
		 */
		q->buf[q->head] = *e;
	}
	pthread_mutex_unlock(&q->lock);
}

/**
 * controller_evtq_pop - 从队头取出并移除一个事件
 * @q: 队列对象
 * @out: 输出事件副本
 *
 * Return: true 取出成功；false 队列为空或参数为 NULL。
 */
bool controller_evtq_pop(struct controller_evtq *q, struct controller_evt *out)
{
	bool ok = false;

	if (q == NULL || out == NULL) {
		return false;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count > 0u) {
		*out = q->buf[q->head];
		q->head = (q->head + 1u) % CONTROLLER_EVTQ_LEN;
		q->count--;
		ok = true;
	}
	pthread_mutex_unlock(&q->lock);
	return ok;
}

/**
 * controller_evtq_clear - 清空队列中的全部待处理事件
 * @q: 队列对象；NULL 时直接返回
 */
void controller_evtq_clear(struct controller_evtq *q)
{
	if (q == NULL) {
		return;
	}

	pthread_mutex_lock(&q->lock);
	q->head = 0u;
	q->tail = 0u;
	q->count = 0u;
	pthread_mutex_unlock(&q->lock);
}
