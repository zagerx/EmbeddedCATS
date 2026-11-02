/*
 * main.c — 演示程序入口
 *
 * 1 ms 推一次状态机：
 *   hsm_init() 从根一路 ENTRY 到 IDLE（所以先看到 enter ROOT/OPERATIONAL/IDLE）；
 *   之后每 1 ms 调一次 hsm_dispatch(DEMO_SIG_TICK)。
 *
 * 状态自己会打印 enter/exit，所以**只有发生迁移时才有输出**；
 * 另外每 1000 拍打一行心跳，用来确认它还在跑。
 *
 * 用绝对时刻 sleep（CLOCK_MONOTONIC + TIMER_ABSTIME），长期跑不会累积漂移。
 */

#include <stdio.h>
#include <time.h>

#include "hsm.h"
#include "states.h"

#define TICK_MS 1u

/* 演示工程只有一个状态机实例 */
static struct hsm g_hsm;

static uint32_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* 睡到绝对时刻（毫秒），不是"睡多久"，所以不会越睡越慢 */
static void sleep_until_ms(uint32_t deadline)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(deadline / 1000u);
	ts.tv_nsec = (long)(deadline % 1000u) * 1000000L;
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

int main(void)
{
	const uint32_t start = now_ms();
	uint32_t next = start;
	unsigned ticks = 0u;

	hsm_init(&g_hsm, &st_idle, start);

	for (;;) {
		next += TICK_MS;
		sleep_until_ms(next);

		hsm_dispatch(&g_hsm, DEMO_SIG_TICK, 0, next);

		ticks++;
		if (ticks % 1000u == 0u) {
			printf("[%6u ms] %u 拍，state=%s\n", next - start, ticks,
			       hsm_current(&g_hsm)->name);
		}
	}

	return 0;
}
