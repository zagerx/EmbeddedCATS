/*
 * master_probe.c — 使用 CANopen 事件循环的无界面验证器
 *
 * 主线程就是唯一的 CANopen 工作线程；定时命令在事件处理后注入。
 * 用法：master_probe [CAN 接口] [运行秒数，0 = 一直跑] [指令参数...]
 *
 * Author: zager
 */

#include <control/control.h>

#include <cats_mech.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct probe_cmd {
	double move_mm;
	double move2_mm;
	double vel_mm_s;
	double acc_mm_s2;
	long move_at_ms;
	long move2_at_ms;
	long stop_at_ms;
	long estop_at_ms;
	long spam;
	long spam_at_ms;
	int has_move;
	int has_move2;
	int has_stop;
	int has_estop;
	int has_spam;
	int armed;
	int no_enable; /* 置位后不自动发使能，用来验证停在 IDLE */
};

static volatile sig_atomic_t s_stop;
static uint32_t s_base_ms;

/* 解析可选的运动、停止与急停命令 */
static struct probe_cmd probe_cmd_parse(int argc, char **argv);

/* 记录 SIGINT，让事件循环自行退出 */
static void on_sigint(int sig);

/* 读取与 CANopenLinux 同源的单调毫秒时钟 */
static uint32_t probe_now_ms(void);

int main(int argc, char **argv)
{
	const char *ifname = (argc > 1) ? argv[1] : "can0";
	long seconds = (argc > 2) ? strtol(argv[2], NULL, 10) : 0;
	struct probe_cmd cmd = probe_cmd_parse(argc, argv);
	enum controller_state last = CONTROLLER_STATE_COUNT;
	uint32_t start_ms;
	uint32_t now;
	int moved = 0;
	int moved2 = 0;
	int batch = 0;
	int spammed = 0;
	int stopped = 0;
	int estopped = 0;

	signal(SIGINT, on_sigint);
	printf("接口 %s，节点 1/2，CANopenLinux epoll（基础定时 1 ms）\n", ifname);
	fflush(stdout);
	if (controller_init(ifname) != CONTROLLER_OK) {
		fprintf(stderr, "controller_init 失败（接口 %s 是否存在且已 up？）\n", ifname);
		return 1;
	}
	start_ms = probe_now_ms();
	s_base_ms = start_ms;

	if (cmd.armed) {
		printf("换算参数由 controller/config/cats_mech.h 决定（%d pulse/rev，%.3f "
		       "mm/rev）\n",
		       (int)CATS_AXIS_PULSES_PER_REV, (double)CATS_AXIS_MM_PER_REV);
		fflush(stdout);
	}

	while (!s_stop) {
		if (controller_event_poll() != CONTROLLER_OK) {
			fprintf(stderr, "CANopen 事件循环退出\n");
			break;
		}
		now = probe_now_ms();
		if (cmd.armed) {
			long t = (long)(now - s_base_ms);

			if (cmd.has_move && cmd.has_move2 && !batch &&
			    cmd.move2_at_ms == cmd.move_at_ms && t >= cmd.move_at_ms) {
				struct controller_move_req reqs[2];

				batch = 1;
				moved = 1;
				moved2 = 1;
				reqs[0].axis = 0u;
				reqs[0].mm = (float)cmd.move_mm;
				reqs[0].mm_s = (float)cmd.vel_mm_s;
				reqs[0].mm_s2 = (float)cmd.acc_mm_s2;
				reqs[1].axis = 1u;
				reqs[1].mm = (float)cmd.move2_mm;
				reqs[1].mm_s = (float)cmd.vel_mm_s;
				reqs[1].mm_s2 = (float)cmd.acc_mm_s2;
				printf("[%6ld] 指令：move 双轴 %.3f / %.3f mm\n", t, cmd.move_mm,
				       cmd.move2_mm);
				if (controller_move(reqs, 2u) != CONTROLLER_OK) {
					printf("          —— 入队失败（参数错或队列满）\n");
				}
				fflush(stdout);
			}
			if (cmd.has_move && !moved && t >= cmd.move_at_ms) {
				struct controller_move_req req = {
					.axis = 0,
					.mm = (float)cmd.move_mm,
					.mm_s = (float)cmd.vel_mm_s,
					.mm_s2 = (float)cmd.acc_mm_s2,
				};

				moved = 1;
				printf("[%6ld] 指令：move_to %.3f mm\n", t, cmd.move_mm);
				if (controller_move(&req, 1u) != CONTROLLER_OK) {
					printf("          —— 入队失败（参数错或队列满）\n");
				}
				fflush(stdout);
			}
			if (cmd.has_move2 && !moved2 && t >= cmd.move2_at_ms) {
				struct controller_move_req req = {
					.axis = 1,
					.mm = (float)cmd.move2_mm,
					.mm_s = (float)cmd.vel_mm_s,
					.mm_s2 = (float)cmd.acc_mm_s2,
				};

				moved2 = 1;
				printf("[%6ld] 指令：move_to %.3f mm（第二条）\n", t, cmd.move2_mm);
				if (controller_move(&req, 1u) != CONTROLLER_OK) {
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
					struct controller_move_req req = {
						.axis = 0,
						.mm = (float)(i + 1),
						.mm_s = (float)cmd.vel_mm_s,
						.mm_s2 = (float)cmd.acc_mm_s2,
					};

					if (controller_move(&req, 1u) == CONTROLLER_OK) {
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
				controller_stop();
				fflush(stdout);
			}
			if (cmd.has_estop && !estopped && t >= cmd.estop_at_ms) {
				estopped = 1;
				printf("[%6ld] 指令：estop\n", t);
				controller_estop();
				fflush(stdout);
			}
		}

		if (controller_state() != last) {
			last = controller_state();
			printf("[%6u] state = %s", (unsigned)(now - s_base_ms),
			       controller_state_name(last));
			if (last == CONTROLLER_STATE_FAULT) {
				uint8_t i;

				printf(" (%s", controller_fault_name(controller_fault_code()));
				for (i = 0; i < 2; i++) {
					uint32_t abort_code = controller_axis_abort(i);

					if (abort_code != 0U) {
						printf(", axis%u abort=0x%08X", (unsigned)i,
						       (unsigned)abort_code);
					}
				}
				printf(")");
			}
			/* 配置完成、停在空闲态时自动请求使能；--no-enable 可关掉以验证停在 IDLE */
			if (last == CONTROLLER_STATE_IDLE && !cmd.no_enable) {
				printf(" —— 自动请求使能");
				(void)controller_enable();
			}
			printf("\n");
			fflush(stdout);
		}
		if (seconds > 0 && (long)(now - start_ms) >= seconds * 1000L) {
			break;
		}
	}

	(void)controller_deinit();
	printf("退出\n");
	return 0;
}

static struct probe_cmd probe_cmd_parse(int argc, char **argv)
{
	struct probe_cmd c = {0};
	int i;

	c.vel_mm_s = 10.0;
	c.acc_mm_s2 = 50.0;
	c.move_at_ms = -1;
	c.move2_at_ms = -1;
	c.stop_at_ms = -1;
	c.estop_at_ms = -1;
	c.spam = -1;
	c.spam_at_ms = -1;
	for (i = 3; i < argc; i++) {
		const char *k = argv[i];
		double v;

		if (strcmp(k, "--no-enable") == 0) {
			c.no_enable = 1;
			continue;
		}
		if (i + 1 >= argc) {
			break;
		}
		v = strtod(argv[i + 1], NULL);
		i++; /* 消费参数值 */

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
		}
	}
	c.armed = c.has_move || c.has_move2 || c.has_stop || c.has_estop || c.has_spam;
	if (c.has_move && c.move_at_ms < 0) {
		c.move_at_ms = 1500;
	}
	if (c.has_move2 && c.move2_at_ms < 0) {
		c.move2_at_ms = (c.has_move ? c.move_at_ms : 1500) + 500;
	}
	if (c.has_spam && c.spam_at_ms < 0) {
		c.spam_at_ms = (c.has_move ? c.move_at_ms : 1500) + 200;
	}
	return c;
}

static void on_sigint(int sig)
{
	(void)sig;
	s_stop = 1;
}

static uint32_t probe_now_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u);
}
