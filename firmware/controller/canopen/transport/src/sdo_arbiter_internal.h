/*
 * sdo_arbiter_internal.h — 仲裁器实例存储
 *
 * 由仲裁器实现与对象所有者包含；普通调用方只使用 sdo_arbiter.h。
 *
 * Author: zager
 */

#ifndef TRANSPORT_SDO_ARBITER_INTERNAL_H
#define TRANSPORT_SDO_ARBITER_INTERNAL_H

#include <transport/sdo_arbiter_instance.h>

void sdo_arbiter_init(struct sdo_arbiter *a, struct sdo_client *client);
void sdo_arbiter_step(struct sdo_arbiter *a);

#endif /* TRANSPORT_SDO_ARBITER_INTERNAL_H */
