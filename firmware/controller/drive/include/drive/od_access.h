/*
 * od_access.h — 对象访问抽象（drive 唯一依赖的"传输"接口）
 *
 * drive 只说"读/写哪个对象、等多久"，不知道背后是 SDO 还是 PDO。
 * 实现放在 drive/src/backends/：od_sdo.c（现在）、od_pdo.c（以后）。
 *
 * 三个函数都是**阻塞**语义，只能在服务线程里调用。
 */

#ifndef DRIVE_OD_ACCESS_H
#define DRIVE_OD_ACCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_access {
	/* 0 = 成功；-1 = 失败（超时 / abort / 总线错） */
	int (*read)(void *ctx, uint16_t index, uint8_t sub, uint32_t *value, uint8_t *len,
		    uint32_t timeout_ms);
	int (*write)(void *ctx, uint16_t index, uint8_t sub, uint32_t value, uint8_t size,
		     uint32_t timeout_ms);
	/*
	 * 打断在途事务：正在等结果的调用立刻返回 -1，并作废这一笔。
	 * 安全动作用（急停/halt 抢在途运动），不要拿来重试。
	 */
	void (*abort)(void *ctx);
	/* 单调毫秒：drive 自己不碰平台时钟 */
	uint32_t (*now_ms)(void *ctx);
	/* 最近一次失败的原因（SDO abort code；0 = 超时/取消/总线错） */
	uint32_t (*last_error)(void *ctx);
	void *ctx;
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRIVE_OD_ACCESS_H */
