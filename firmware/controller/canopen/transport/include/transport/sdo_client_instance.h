/*
 * sdo_client_instance.h — CANopen SDO 客户端实例存储
 *
 * 仅组合根、实例所有者与真实实现包含本头。
 * 402 等普通调用方只包含 sdo_client.h，不接触 CANopenNode 类型。
 *
 * Author: zager
 */

#ifndef TRANSPORT_SDO_CLIENT_INSTANCE_H
#define TRANSPORT_SDO_CLIENT_INSTANCE_H

#include <transport/sdo_client.h>

#include "CANopen.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sdo_client_state {
	SDO_CLIENT_IDLE = 0,
	SDO_CLIENT_RUNNING,
	SDO_CLIENT_DONE
};

struct sdo_client {
	CO_SDOclient_t *co;
	uint8_t dir;
	uint8_t state;
	uint8_t cancel;
	uint8_t timed_out;
	uint8_t abort_timed_out;
	uint32_t timeout_us;
	uint32_t elapsed_us;
	uint32_t abort_elapsed_us;
	struct sdo_result result;
};

void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_CLIENT_INSTANCE_H */
