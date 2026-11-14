/*
 * sdo_arbiter_instance.h — 仲裁器实例存储
 *
 * 仅供对象所有者和仲裁器实现包含；普通调用方只使用 sdo_arbiter.h。
 *
 * Author: zager
 */

#ifndef TRANSPORT_SDO_ARBITER_INSTANCE_H
#define TRANSPORT_SDO_ARBITER_INSTANCE_H

#include <transport/sdo_arbiter.h>
#include <transport/sdo_client.h>

enum sdo_arbiter_owner {
	SDO_ARBITER_NONE = 0,
	SDO_ARBITER_CONTROL,
	SDO_ARBITER_FEEDBACK
};

struct sdo_request {
	uint8_t dir;
	uint16_t index;
	uint8_t sub;
	uint32_t value;
	uint8_t size;
	uint32_t timeout_ms;
};

struct sdo_arbiter {
	struct sdo_client *client;
	struct sdo_access control;
	struct sdo_access feedback;
	enum sdo_arbiter_owner owner;
	struct sdo_request control_request;
	uint8_t control_pending;
	uint8_t control_ready;
	uint8_t feedback_ready;
	struct sdo_result control_result;
	struct sdo_result feedback_result;
};

#endif /* TRANSPORT_SDO_ARBITER_INSTANCE_H */
