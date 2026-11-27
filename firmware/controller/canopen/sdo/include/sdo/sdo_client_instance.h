/*
 * sdo_client_instance.h — CANopen SDO 客户端实例存储
 *
 * 仅组合根、实例所有者与真实实现包含本头。
 * 402 等普通调用方只包含 sdo_client.h，不接触 CANopenNode 类型。
 *
 * Author: zager
 */

#ifndef SDO_SDO_CLIENT_INSTANCE_H
#define SDO_SDO_CLIENT_INSTANCE_H

#include <sdo/sdo_client.h>

#include "CANopen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 取消指令的发令方：调用方登记，或通道期限到后自己发出 */
enum sdo_abort_cause {
	SDO_ABORT_NONE = 0,
	SDO_ABORT_CALLER,
	SDO_ABORT_TIMEOUT
};

/*
 * 一笔在途事务的全部状态与结果。
 * 一条通道同时最多一笔事务，因此事务就嵌在通道里，不单独分配。
 */
struct sdo_transaction {
	uint8_t dir;               /* enum sdo_dir */
	uint8_t state;             /* enum sdo_client_state */
	uint8_t abort_cause;       /* enum sdo_abort_cause；非 NONE 即已进入取消收尾 */
	uint32_t timeout_us;       /* 事务总期限 */
	uint32_t elapsed_us;       /* 事务已用时间 */
	uint32_t abort_elapsed_us; /* 取消收尾已用时间 */
	struct sdo_result result;
};

struct sdo_client {
	CO_SDOclient_t *co;         /* 通道：CANopenNode 客户端，装配后不变 */
	struct sdo_transaction txn; /* 当前事务 */
};

void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client);

#ifdef __cplusplus
}
#endif

#endif /* SDO_SDO_CLIENT_INSTANCE_H */
