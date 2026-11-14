/*
 * axis_map.h — 整机拓扑：轴 ↔ CANopen 节点 ↔ OD 的 SDO 客户端通道
 *
 * 三者一一对应且固定，顺序不得调整。OD 里 0x1280/0x1281 的 COB-ID 与
 * node_ID 就是按这张表配的；装配时按表登记节点，并校验登记结果与表一致。
 *
 * 表在 config/axis_map.c 里定义，全程序只有一份；本文件只给 extern 声明，
 * 任意 .c 包含本文件都能读这张表。
 *
 * Author: zager
 */

#ifndef CATS_AXIS_MAP_H
#define CATS_AXIS_MAP_H

#include <control/state_types.h>

#include <stdint.h>

struct axis_map_entry {
	enum controller_axis axis; /* 轴索引，也是 axes[] 的下标 */
	uint8_t node_id;           /* CANopen 从站站号，OD 0x1280+n 的 node_ID */
	uint8_t sdo_channel;       /* OD 的 SDO 客户端通道：0x1280 + sdo_channel */
};

/* 表项数 = 轴个数；加轴时枚举、这个数字与 axis_map.c 里的表行一起改。
 * 写死字面量：（声明方不能用 sizeof 取长度）
 */
#define AXIS_MAP_COUNT 2u

/* 顺序即登记顺序：装配时逐项 master_add_node() */
extern const struct axis_map_entry AXIS_MAP[AXIS_MAP_COUNT];

typedef char axis_map_count_check[(AXIS_MAP_COUNT == CONTROLLER_AXIS_COUNT) ? 1 : -1];

#endif /* CATS_AXIS_MAP_H */
