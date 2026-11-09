/*
 * od.h — DS 402 用到的对象号（**全工程唯一出处**）
 *
 * 任何地方看到 0x6040 / 0x6041 这类数字，都必须来自这里。
 * 本层不认识总线，所以这里只有编号与类型，没有访问方式。
 */

#ifndef CIA402_OD_H
#define CIA402_OD_H

#define CIA402_OD_CONTROLWORD 0x6040u      /* UNSIGNED16 读/写 */
#define CIA402_OD_STATUSWORD 0x6041u       /* UNSIGNED16 只读 */
#define CIA402_OD_MODE 0x6060u             /* INTEGER8 写 */
#define CIA402_OD_MODE_ACTUAL 0x6061u      /* INTEGER8 读回校验用 */
#define CIA402_OD_POSITION_ACTUAL 0x6064u  /* INTEGER32 实际位置（pulse） */
#define CIA402_OD_VELOCITY_ACTUAL 0x606Cu  /* INTEGER32 实际速度（pulse/s） */
#define CIA402_OD_TARGET_POSITION 0x607Au  /* INTEGER32 目标位置 */
#define CIA402_OD_PROFILE_VELOCITY 0x6081u /* UNSIGNED32 */
#define CIA402_OD_PROFILE_ACCEL 0x6083u    /* UNSIGNED32 */
#define CIA402_OD_PROFILE_DECEL 0x6084u    /* UNSIGNED32 */
#define CIA402_OD_ERROR_CODE 0x603Fu       /* UNSIGNED16 驱动器自己报的错误码 */

#endif /* CIA402_OD_H */
