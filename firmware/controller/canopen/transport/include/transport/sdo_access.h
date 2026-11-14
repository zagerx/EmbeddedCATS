/*
 * sdo_access — 异步 SDO 对象访问契约。
 *
 * 调用方发起事务后立即返回，之后用 poll 逐拍推进并取结果。
 * 每条通道同时最多一笔事务；事务期限由通道自己掌握，调用方不记账。
 *
 * 期限口径统一定义在通道内部：从真正上总线那一刻起算，排队等待不计时；
 * 到期由通道自行 abort，并以 SDO_STATUS_TIMEOUT 交付结果。
 *
 * 调用方的全部义务只有一条：每次 start() 之后反复 poll() 直到返回 DONE。
 * start() 的返回值只用于诊断，不改变这条义务。
 *
 * PDO 等周期映射不属于本接口。
 */

#ifndef TRANSPORT_SDO_ACCESS_H
#define TRANSPORT_SDO_ACCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sdo_dir {
	SDO_DIR_READ = 0,
	SDO_DIR_WRITE = 1,
};

enum sdo_status {
	SDO_STATUS_OK = 0,      /* 对端应答正常 */
	SDO_STATUS_FAIL = -1,   /* 对端 abort、本地拒绝或参数错误 */
	SDO_STATUS_TIMEOUT = -2 /* 通道期限到：已 abort，可继续发起下一笔 */
};

struct sdo_result {
	int8_t status; /* enum sdo_status */
	uint32_t value;
	uint32_t abort_code;
};

enum sdo_poll {
	SDO_POLL_DONE = 0, /* 已交付一笔结果（含失败与超时） */
	SDO_POLL_WAITING,  /* 有在途事务，结果还没到 */
	SDO_POLL_IDLE      /* 没有在途事务 */
};

struct sdo_access {
	/* 0 = 已受理；-1 = 会话已被占用（上次结果未取走或有事务在途）。
	 * 拒绝时通道仍保证有结果可交付，调用方不会等不到事件。 */
	int (*start)(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		     uint8_t size, uint32_t timeout_ms);
	/* 推进并取结果：DONE 时填充 result；WAITING / IDLE 时 result 不变。 */
	enum sdo_poll (*poll)(void *ctx, struct sdo_result *result);
	/* 请求取消当前事务；结果仍通过 poll 交付。无在途事务时无副作用。 */
	void (*abort)(void *ctx);
	void *ctx;
};

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_ACCESS_H */
