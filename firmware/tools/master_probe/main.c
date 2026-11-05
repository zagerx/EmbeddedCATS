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

#include <control/control.h>
#include <socketcan/can_io_socketcan.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POLL_INTERVAL_MS 20u /* 节拍：够判 750 ms 心跳超时（误差 2.7%） */

/* 可选：到点往 app 注入一条指令，方便在命令行复现运动/停止/急停。
 * 单位：位置 mm、速度 mm/s、加速度 mm/s²；--pulses/--lead 是**测试用**的
 * 换算参数（每转多少脉冲 / 每转多少毫米），不是实测机械参数。 */
struct probe_cmd {
	double move_mm;
	double move2_mm;
	double vel_mm_s;
	double acc_mm_s2;
	double mm_per_rev;
	long move_at_ms;
	long move2_at_ms;
	long stop_at_ms;
	long estop_at_ms;
	long spam;
	long spam_at_ms;
	int32_t pulses_per_rev;
	int has_move;
	int has_move2;
	int has_stop;
	int has_estop;
	int has_spam;
	int armed;
};

static struct probe_cmd probe_cmd_parse(int argc, char **argv)
{
	struct probe_cmd c = {0};
	int i;

	c.vel_mm_s = 10.0; /* 默认 10 mm/s */
	c.acc_mm_s2 = 50.0;
	c.pulses_per_rev = 10000;
	c.move_at_ms = -1;
	c.move2_at_ms = -1;
	c.stop_at_ms = -1;
	c.estop_at_ms = -1;
	c.spam = -1;
	c.spam_at_ms = -1;

	for (i = 3; i + 1 < argc; i += 2) {
		const char *k = argv[i];
		double v = strtod(argv[i + 1], NULL);

		if (strcmp(k, "--move") == 0) {
			c.move_mm = v;
			c.has_move = 1;
		} else if (strcmp(k, "--move-at") == 0) {
			c.move_at_ms = (long)v;
		} else if (strcmp(k, "--move2") == 0) {
			c.move2_mm = v;
			c.has_move2 = 1;
		} else if (strcmp(k, "--move2-at") == 0) {
			c.move2_at_ms = (long)v;
		} else if (strcmp(k, "--stop-at") == 0) {
			c.stop_at_ms = (long)v;
			c.has_stop = 1;
		} else if (strcmp(k, "--estop-at") == 0) {
			c.estop_at_ms = (long)v;
			c.has_estop = 1;
		} else if (strcmp(k, "--spam") == 0) {
			c.spam = (long)v;
			c.has_spam = c.spam > 0;
		} else if (strcmp(k, "--spam-at") == 0) {
			c.spam_at_ms = (long)v;
		} else if (strcmp(k, "--vel") == 0) {
			c.vel_mm_s = v;
		} else if (strcmp(k, "--acc") == 0) {
			c.acc_mm_s2 = v;
		} else if (strcmp(k, "--pulses") == 0) {
			c.pulses_per_rev = (int32_t)v;
		} else if (strcmp(k, "--lead") == 0) {
			c.mm_per_rev = v;
		}
	}

	c.armed = c.has_move || c.has_move2 || c.has_stop || c.has_estop || c.has_spam;
	if (c.has_move && c.move_at_ms < 0) {
		c.move_at_ms = 1500; /* 默认等 ENABLING 结束再发 */
	}
	if (c.has_move2 && c.move2_at_ms < 0) {
		c.move2_at_ms = (c.has_move ? c.move_at_ms : 1500) + 500;
	}
	if (c.has_spam && c.spam_at_ms < 0) {
		c.spam_at_ms = (c.has_move ? c.move_at_ms : 1500) + 200;
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
	enum cats_state last = CATS_STATE_COUNT;
	uint32_t start_ms;
	uint32_t now;
	int moved = 0;
	int moved2 = 0;
	int spammed = 0;
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

	if (cats_control_init(&master) != CATS_OK) {
		fprintf(stderr, "cats_control_init 失败\n");
		can_io_close(&io);
		return 1;
	}

	start_ms = can_io_now_ms(&io);
	s_base_ms = start_ms;

	if (cmd.armed) {
		if (cmd.mm_per_rev <= 0.0 || cmd.pulses_per_rev <= 0) {
			fprintf(stderr, "注入指令需要 --lead <mm/rev> 与 --pulses <pulse/rev>\n");
			can_io_close(&io);
			return 1;
		}
		(void)cats_control_set_axis_scale(0, cmd.pulses_per_rev, (float)cmd.mm_per_rev);
		printf("测试换算：%d pulse/rev，%.3f mm/rev（测试值，不是实测机械参数）\n",
		       (int)cmd.pulses_per_rev, cmd.mm_per_rev);
		fflush(stdout);
	}

	while (!s_stop) {
		(void)cats_control_poll();

		now = can_io_now_ms(&io);
		if (cmd.armed) {
			long t = (long)(now - s_base_ms);

			if (cmd.has_move && !moved && t >= cmd.move_at_ms) {
				moved = 1;
				printf("[%6ld] 指令：move_to %.3f mm\n", t, cmd.move_mm);
				if (cats_control_move_to(0, (float)cmd.move_mm, (float)cmd.vel_mm_s,
							 (float)cmd.acc_mm_s2) != CATS_OK) {
					printf("          —— 入队失败（参数错或队列满）\n");
				}
				fflush(stdout);
			}
			if (cmd.has_move2 && !moved2 && t >= cmd.move2_at_ms) {
				moved2 = 1;
				printf("[%6ld] 指令：move_to %.3f mm（第二条）\n", t, cmd.move2_mm);
				if (cats_control_move_to(0, (float)cmd.move2_mm,
							 (float)cmd.vel_mm_s,
							 (float)cmd.acc_mm_s2) != CATS_OK) {
					printf("          —— 入队失败（参数错或队列满）\n");
				}
				fflush(stdout);
			}
			if (cmd.has_spam && !spammed && t >= cmd.spam_at_ms) {
				long i;
				long ok = 0;
				long busy = 0;

				spammed = 1;
				for (i = 0; i < cmd.spam; i++) {
					if (cats_control_move_to(0, (float)(i + 1),
								 (float)cmd.vel_mm_s,
								 (float)cmd.acc_mm_s2) == CATS_OK) {
						ok++;
					} else {
						busy++;
					}
				}
				printf("[%6ld] 连发 %ld 条 move：入队 %ld，BUSY %ld\n", t, cmd.spam,
				       ok, busy);
				fflush(stdout);
			}
			if (cmd.has_stop && !stopped && t >= cmd.stop_at_ms) {
				stopped = 1;
				printf("[%6ld] 指令：stop\n", t);
				cats_control_stop();
				fflush(stdout);
			}
			if (cmd.has_estop && !estopped && t >= cmd.estop_at_ms) {
				estopped = 1;
				printf("[%6ld] 指令：estop\n", t);
				cats_control_estop();
				fflush(stdout);
			}
		}

		if (cats_control_state() != last) {
			last = cats_control_state();
			printf("[%6u] state = %s", (unsigned)(now - s_base_ms),
			       cats_control_state_name(last));
			if (last == CATS_STATE_FAULT) {
				uint8_t i;

				printf(" (%s", cats_control_fault_name(cats_control_fault_code()));
				for (i = 0; i < 2; i++) {
					uint32_t abort_code = cats_control_axis_abort(i);

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
