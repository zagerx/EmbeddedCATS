/*
 * mode.h — 402 操作模式的插件接口
 *
 * 一种模式 = 一段"下发设定值 + 判定到位"的知识（PP / CSP / CSV / HM…）。
 * drive 只负责：什么时候允许下发、事实与失败怎么记；
 * 模式只负责：写哪些对象、什么算"这次运动结束"。
 *
 * 加一种模式 = 新增一个 mode_*.c + 在 drive_init 里按 6060 的值取表，
 * drive.c 不需要改。
 */

#ifndef DRIVE_MODE_H
#define DRIVE_MODE_H

#include <ds402/types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct drive;

struct mode_ops {
	uint8_t id; /* 6060 的值（DS402_MODE_*） */

	/* 下发一次新目标（阻塞，服务线程）。0 = 已下发；-1 = 失败 */
	int (*begin)(struct drive *d, int32_t a, int32_t b, int32_t c, int32_t e, uint32_t now_ms);

	/* 运动是否结束：1 = 到位；0 = 在途；-1 = 失败（超时/故障） */
	int (*poll)(struct drive *d, uint32_t now_ms);

	/* 是否还有在途运动（转发到各自的状态位） */
	int (*busy)(const struct drive *d);
};

/* PP（Profile Position），本工程目前唯一实现的模式 */
const struct mode_ops *mode_pp_ops(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRIVE_MODE_H */
