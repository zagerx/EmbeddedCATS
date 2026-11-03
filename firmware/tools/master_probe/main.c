/*
 * master_probe — 无界面运行器：开通道 → 建主站 → 按 20 ms 推进 app 状态机
 *
 * 为什么需要它：不用开界面就能看状态机日志、复现问题、跑长时间回归；
 * 界面层（apps/qt/bindings）虽然也能跑同样的东西，但它给不了可 grep 的终端输出。
 *
 * 用法：
 *     master_probe [CAN 接口] [运行秒数，0 = 一直跑]
 *     master_probe can0 30
 *
 * 退出：Ctrl-C 或到时。
 */

#include <app/app.h>
#include <socketcan/can_io_socketcan.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define POLL_INTERVAL_MS 20u /* 节拍：够判 750 ms 心跳超时（误差 2.7%） */

static volatile sig_atomic_t s_stop = 0;
static struct can_io *s_io; /* 只给时间戳用 */
static uint32_t s_base_ms;  /* 时间零点：开跑时刻 */

static void on_sigint(int sig)
{
	(void)sig;
	s_stop = 1;
}

static void sleep_ms(uint32_t ms)
{
	const struct timespec ts = {
		.tv_sec = (time_t)(ms / 1000u),
		.tv_nsec = (long)(ms % 1000u) * 1000000L,
	};

	(void)nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
	static struct socketcan_dev dev; /* 静态：全程不用 malloc */
	static struct co_master master;

	struct can_io io;
	const char *ifname = (argc > 1) ? argv[1] : "can0";
	long seconds = (argc > 2) ? strtol(argv[2], NULL, 10) : 0;
	enum cats_run_state last = CATS_RUN_STATE_COUNT;
	uint32_t start_ms;
	uint32_t now;

	if (socketcan_open(&io, &dev, ifname) != CAN_IO_OK) {
		fprintf(stderr, "打不开 %s（接口是否 up？）\n", ifname);
		return 1;
	}
	s_io = &io;

	signal(SIGINT, on_sigint);

	co_master_init(&master, &io);
	(void)co_master_add_node(&master, 1);
	(void)co_master_add_node(&master, 2);

	printf("接口 %s，节点 1/2，节拍 %u ms\n", ifname, POLL_INTERVAL_MS);
	fflush(stdout);

	if (cats_app_init(&master) != CATS_APP_OK) {
		fprintf(stderr, "cats_app_init 失败\n");
		can_io_close(&io);
		return 1;
	}

	start_ms = can_io_now_ms(&io);
	s_base_ms = start_ms;

	while (!s_stop) {
		(void)cats_app_poll();

		now = can_io_now_ms(&io);
		if (cats_app_state() != last) {
			last = cats_app_state();
			printf("[%6u] state = %s", (unsigned)(now - s_base_ms),
			       cats_app_state_name(last));
			if (last == CATS_RUN_FAULT) {
				printf(" (%s)", cats_app_fault_name(cats_app_fault_code()));
			}
			printf("\n");
			fflush(stdout);
		}

		if (seconds > 0 && (long)(now - start_ms) >= seconds * 1000L) {
			break;
		}
		sleep_ms(POLL_INTERVAL_MS);
	}

	can_io_close(&io);
	printf("退出\n");
	return 0;
}
