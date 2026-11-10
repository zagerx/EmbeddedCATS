/*
 * sdo_access — 异步 SDO 对象访问契约。
 *
 * 调用方发起事务后立即返回，之后通过 take_result 取完成事件。
 * 每条通道同时最多一笔事务；PDO 等周期映射不属于本接口。
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

struct sdo_result {
	int8_t status; /* 0 成功；-1 失败、取消或超时 */
	uint8_t len;
	uint32_t value;
	uint32_t abort_code;
};

struct sdo_access {
	/* 0 = 已排队；-1 = 参数错误、通道忙或实现不可用。 */
	int (*start)(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t value,
		     uint8_t size, uint32_t timeout_ms);
	/* 0 = 取走一笔结果；1 = 尚无结果。 */
	int (*take_result)(void *ctx, struct sdo_result *result);
	/* 请求取消当前事务；结果仍通过 take_result 返回。 */
	void (*abort)(void *ctx);
	void *ctx;
};

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_ACCESS_H */
