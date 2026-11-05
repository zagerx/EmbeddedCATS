/*
 * evt_queue.c — 定长环形事件队列的实现
 *
 * 环形规则：head 指向第一个元素，tail 指向"下一个写入位置"，
 *           count 记录有效元素数 ⇒ 空（count==0）与满（count==LEN）可区分。
 */

#include "evt_queue.h"

void cats_evtq_init(struct cats_evtq *q)
{
	if (q == NULL) {
		return;
	}
	q->head = 0u;
	q->tail = 0u;
	q->count = 0u;
	pthread_mutex_init(&q->lock, NULL);
}

bool cats_evtq_push(struct cats_evtq *q, const struct cats_evt *e)
{
	bool ok = true;

	if (q == NULL || e == NULL) {
		return false;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count == CATS_EVTQ_LEN) {
		ok = false; /* 满：丢弃新的这一条，绝不覆盖已有的 */
	} else {
		q->buf[q->tail] = *e;
		q->tail = (q->tail + 1u) % CATS_EVTQ_LEN;
		q->count++;
	}
	pthread_mutex_unlock(&q->lock);
	return ok;
}

void cats_evtq_push_front(struct cats_evtq *q, const struct cats_evt *e)
{
	if (q == NULL || e == NULL) {
		return;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count < CATS_EVTQ_LEN) {
		q->head = (q->head + CATS_EVTQ_LEN - 1u) % CATS_EVTQ_LEN;
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

bool cats_evtq_pop(struct cats_evtq *q, struct cats_evt *out)
{
	bool ok = false;

	if (q == NULL || out == NULL) {
		return false;
	}

	pthread_mutex_lock(&q->lock);
	if (q->count > 0u) {
		*out = q->buf[q->head];
		q->head = (q->head + 1u) % CATS_EVTQ_LEN;
		q->count--;
		ok = true;
	}
	pthread_mutex_unlock(&q->lock);
	return ok;
}

void cats_evtq_clear(struct cats_evtq *q)
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
