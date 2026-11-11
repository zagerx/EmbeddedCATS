/*
 * 无界面验证器：20 ms 推进业务状态机，独立线程负责 I/O 与阻塞服务。
 * 用法：master_probe [CAN 接口] [运行秒数，0 = 一直跑] [指令参数...]
 */

#include <control/control.h>
#include <socketcan/can_io_socketcan.h>

#include <cats_mech.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POLL_INTERVAL_MS 1u /* 节拍：与 I/O、服务线程一致，心跳超时误差 0.13% */

/* 可选：到点往 app 注入一条指令，方便在命令行复现运动/停止/急停。
 * 单位：位置 mm、速度 mm/s、加速度 mm/s²（换算参数是编译期常量，见 controller/config/cats_mech.h）。 */
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
};

static struct probe_cmd probe_cmd_parse(int argc, char **argv)
{
	struct probe_cmd c = {0};
	int i;

	c.vel_mm_s = 10.0; /* 默认 10 mm/s */
	c.acc_mm_s2 = 50.0;
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

/* ---- 宿主起的两条工作线程（controller 只提供步骤，不创建线程） ---- */

static volatile int s_run_threads = 1;

static void *io_thread_main(void *arg)
{
	(void)arg;
	while (s_run_threads != 0) {
		controller_io_poll();               /* 独占总线、非阻塞 */
		struct timespec ts = {0, 1000000L}; /* 1 ms */
		(void)nanosleep(&ts, NULL);
	}
	return NULL;
}

static void *svc_thread_main(void *arg)
{
	(void)arg;
	while (s_run_threads != 0) {
		controller_svc_poll(); /* 非阻塞推进设备状态机 */
		struct timespec ts = {0, 1000000L};
		(void)nanosleep(&ts, NULL);
	}
	return NULL;
}

static int start_worker_threads(void)
{
	pthread_t t;

	if (pthread_create(&t, NULL, io_thread_main, NULL) != 0) {
		return -1;
	}
	pthread_detach(t);
	if (pthread_create(&t, NULL, svc_thread_main, NULL) != 0) {
		return -1;
	}
	pthread_detach(t);
	return 0;
}

static void on_sigint(int sig)
{
	(void)sig;
	s_stop = 1;
}

/* 睡到绝对时刻 deadline_ms：早于该时刻返回才算正常，不累积漂移。
 * 时间基准与 can_io_now_ms 同源（CLOCK_MONOTONIC 毫秒）。 */
static void sleep_until_ms(uint32_t deadline_ms)
{
	const struct timespec ts = {
		.tv_sec = (time_t)(deadline_ms / 1000u),
		.tv_nsec = (long)(deadline_ms % 1000u) * 1000000L,
	};

	(void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

int main(int argc, char **argv)
{
	static struct socketcan_dev dev; /* 静态：全程不用 malloc */
	static struct master master;

	struct can_io io;
	const char *ifname = (argc > 1) ? argv[1] : "can0";
	long seconds = (argc > 2) ? strtol(argv[2], NULL, 10) : 0;
	struct probe_cmd cmd = probe_cmd_parse(argc, argv);
	enum cats_state last = CATS_STATE_COUNT;
	uint32_t start_ms;
	uint32_t now;
	uint32_t next;
	int moved = 0;
	int moved2 = 0;
	int batch = 0;
	int spammed = 0;
	int stopped = 0;
	int estopped = 0;

	if (socketcan_open(&io, &dev, ifname) != CAN_IO_OK) {
		fprintf(stderr, "打不开 %s（接口是否 up？）\n", ifname);
		return 1;
	}
	s_io = &io;

	signal(SIGINT, on_sigint);

	master_init(&master, &io);
	(void)master_add_node(&master, 1);
	(void)master_add_node(&master, 2);

	printf("接口 %s，节点 1/2，节拍 %u ms\n", ifname, POLL_INTERVAL_MS);
	fflush(stdout);

	if (controller_init(&master) != CATS_OK) {
		fprintf(stderr, "controller_init 失败\n");
		can_io_close(&io);
		return 1;
	}

	/* 宿主职责：起 I/O 与服务两条线程（controller 自己不创建线程） */
	if (start_worker_threads() != 0) {
		fprintf(stderr, "起线程失败\n");
		can_io_close(&io);
		return 1;
	}

	start_ms = can_io_now_ms(&io);
	s_base_ms = start_ms;
	next = start_ms;

	if (cmd.armed) {
		printf("换算参数由 controller/config/cats_mech.h 决定（%d pulse/rev，%.3f "
		       "mm/rev）\n",
		       (int)CATS_AXIS_PULSES_PER_REV, (double)CATS_AXIS_MM_PER_REV);
		fflush(stdout);
	}

	while (!s_stop) {
		(void)controller_poll(); /* ② 业务状态机走一拍 */

		now = can_io_now_ms(&io);
		if (cmd.armed) {
			long t = (long)(now - s_base_ms);

			/* 两轴同一时刻下发：走批量接口，一起启动（这才是"同时动"） */
			if (cmd.has_move && cmd.has_move2 && !batch &&
			    cmd.move2_at_ms == cmd.move_at_ms && t >= cmd.move_at_ms) {
				struct stm_move reqs[2];

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
				if (controller_move(reqs, 2u) != CATS_OK) {
					printf("          —— 入队失败（参数错或队列满）\n");
				}
				fflush(stdout);
			}
			if (cmd.has_move && !moved && t >= cmd.move_at_ms) {
				moved = 1;
				printf("[%6ld] 指令：move_to %.3f mm\n", t, cmd.move_mm);
				if (controller_move_to(0, (float)cmd.move_mm, (float)cmd.vel_mm_s,
						       (float)cmd.acc_mm_s2) != CATS_OK) {
					printf("          —— 入队失败（参数错或队列满）\n");
				}
				fflush(stdout);
			}
			if (cmd.has_move2 && !moved2 && t >= cmd.move2_at_ms) {
				moved2 = 1;
				printf("[%6ld] 指令：move_to %.3f mm（第二条）\n", t, cmd.move2_mm);
				if (controller_move_to(1, (float)cmd.move2_mm, (float)cmd.vel_mm_s,
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
					if (controller_move_to(0, (float)(i + 1),
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
			if (last == CATS_STATE_FAULT) {
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
			printf("\n");
			fflush(stdout);
		}

		if (seconds > 0 && (long)(now - start_ms) >= seconds * 1000L) {
			break;
		}
		/* 固定基准 + 固定步长：晚一点点会自己追回来，不累积漂移；
		 * 但落后超过一拍（例如被 printf 阻塞）就重新对齐，不做无谓追赶。 */
		next += POLL_INTERVAL_MS;
		if ((int32_t)(now - next) >= 0) {
			next = now;
		}
		sleep_until_ms(next);
	}

	can_io_close(&io);
	printf("退出\n");
	return 0;
}
