/*
 * axis_map.c — 整机拓扑表的唯一实体
 *
 * 表定义在这里，全程序只有一份；其它 .c 通过 axis_map.h 的 extern 声明读它。
 * 行数必须等于 AXIS_MAP_COUNT：多写一行编译不过，少写一行会被零填充，
 * 靠 controller_init 里 axis 字段与下标的一致性校验拦住。
 *
 * Author: zager
 */

#include <axis_map.h>

const struct axis_map_entry AXIS_MAP[AXIS_MAP_COUNT] = {
	{CONTROLLER_AXIS_1, 1u, 0u}, /* 0x1280：0x601 / 0x581，站号 1 */
	{CONTROLLER_AXIS_2, 2u, 1u}, /* 0x1281：0x602 / 0x582，站号 2 */
};
