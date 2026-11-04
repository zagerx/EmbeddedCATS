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
#include <string.h>
#include <time.h>

#define POLL_INTERVAL_MS 20u /* 节拍：够判 750 ms 心跳超时（误差 2.7%） */

/* 可选：到点往 app 注入一条指令，方便在命令行复现运动/停止/急停。
 * 单位：位置 µm、速度 µm/s、加速度 µm/s²；--pulses/--lead 是**测试用**的
 * 换算参数（每转多少脉冲 / 每转多少微米），不是实测机械参数。 */
struct probe_cmd {
	long move_um;
	long move_at_ms;
	long stop_at_ms;
	long estop_at_ms;
	long vel_um_s;
	long acc_um_s2;
	int32_t pulses_per_rev;
	int32_t um_per_rev;
	int armed;
};

static struct probe_cmd probe_cmd_parse(int argc, char **argv)
{
	struct probe_cmd c = {-1, -1, -1, -1, 10000, 50000, 10000, 0, 0};
	int i;

	for (i = 3; i + 1 < argc; i += 2) {
		const char *k = argv[i];
		long v = strtol(argv[i + 1], NULL, 10);

		if (strcmp(k, "--move") == 0) {
			c.move_um = v;
			c.armed = 1;
		} else if (strcmp(k, "--move-at") == 0) {
			c.move_at_ms = v;
		} else if (strcmp(k, "--stop-at") == 0) {
			c.stop_at_ms = v;
			c.armed = 1;
		} else if (strcmp(k, "--estop-at") == 0) {
			c.estop_at_ms = v;
			c.armed = 1;
		} else if (strcmp(k, "--vel") == 0) {
			c.vel_um_s = v;
		} else if (strcmp(k, "--acc") == 0) {
			c.acc_um_s2 = v;
		} else if (strcmp(k, "--pulses") == 0) {
			c.pulses_per_rev = (int32_t)v;
		} else if (strcmp(k, "--lead") == 0) {
			c.um_per_rev = (int32_t)v;
		}
	}
	if (c.move_um >= 0 && c.move_at_ms < 0) {
		c.move_at_ms = 1500; /* 默认等 ENABLING 结束再发 */
	}
	return c;
}

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
	struct probe_cmd cmd = probe_cmd_parse(argc, argv);
	enum cats_run_state last = CATS_RUN_STATE_COUNT;
	uint32_t start_ms;
	uint32_t now;
	int moved = 0;
	int stopped = 0;
	int estopped = 0;

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

	if (cmd.armed) {
		if (cmd.um_per_rev <= 0 || cmd.pulses_per_rev <= 0) {
			fprintf(stderr, "注入指令需要 --lead <µm/rev> 与 --pulses <pulse/rev>\n");
			can_io_close(&io);
			return 1;
		}
		(void)cats_app_set_axis_scale(0, cmd.pulses_per_rev, cmd.um_per_rev);
		printf("测试换算：%d pulse/rev，%d µm/rev（测试值，不是实测机械参数）\n",
		       (int)cmd.pulses_per_rev, (int)cmd.um_per_rev);
		fflush(stdout);
	}

	while (!s_stop) {
		(void)cats_app_poll();

		now = can_io_now_ms(&io);
		if (cmd.armed) {
			long t = (long)(now - s_base_ms);

			if (cmd.move_um >= 0 && !moved && t >= cmd.move_at_ms) {
				moved = 1;
				printf("[%6ld] 指令：move_to %ld µm\n", t, cmd.move_um);
				if (cats_app_move_to(0, (int32_t)cmd.move_um, (int32_t)cmd.vel_um_s,
						     (int32_t)cmd.acc_um_s2) != 0) {
					printf("          —— 被拒（换算未配/轴号错）\n");
				}
				fflush(stdout);
			}
			if (cmd.stop_at_ms >= 0 && !stopped && t >= cmd.stop_at_ms) {
				stopped = 1;
				printf("[%6ld] 指令：stop\n", t);
				cats_app_stop();
				fflush(stdout);
			}
			if (cmd.estop_at_ms >= 0 && !estopped && t >= cmd.estop_at_ms) {
				estopped = 1;
				printf("[%6ld] 指令：estop\n", t);
				cats_app_estop();
				fflush(stdout);
			}
		}

		if (cats_app_state() != last) {
			last = cats_app_state();
			printf("[%6u] state = %s", (unsigned)(now - s_base_ms),
			       cats_app_state_name(last));
			if (last == CATS_RUN_FAULT) {
				uint8_t i;

				printf(" (%s", cats_app_fault_name(cats_app_fault_code()));
				for (i = 0; i < 2; i++) {
					uint32_t abort_code = cats_app_axis_abort(i);

					if (abort_code != 0U) {
						printf(", axis%u abort=0x%08X", (unsigned)i,
						       (unsigned)abort_code);
					}
				}
				printf(")");
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
