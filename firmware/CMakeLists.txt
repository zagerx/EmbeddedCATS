cmake_minimum_required(VERSION 3.16)
project(cats_firmware LANGUAGES C CXX)

# 导出编译库给 clangd：跳转/补全/格式化都靠它（生成 build/compile_commands.json）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON) # Q_OBJECT 需要 moc
set(CMAKE_AUTORCC ON) # qt.qrc 需要 rcc
add_compile_options(-Wall -Wextra)

# 跨线程同步（control 的指令槽/发布值用 pthread mutex）、控制线程（std::thread）
find_package(Threads REQUIRED)

# ---------------------------------------------------------------
# 一层一个 target。依赖方向在链接关系里也要立得住：
#   exe(qt) → control → master → canopen → bus
#   协议栈（canopen）不知道有几个从站；站号表与收发在 master
# ---------------------------------------------------------------

# 内核层：平台无关，只认识自己定义的 can_io.h / master.h
# （具体定义见下方 canopen_node / canopen_kernel）

# 边界层：一帧 CAN 的收发能力（契约 + 平台实现）。不认识任何协议
add_library(bus_socketcan STATIC controller/bus/socketcan/src/can_io_socketcan.c)
target_include_directories(bus_socketcan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/bus/socketcan/include)
target_include_directories(bus_socketcan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/bus/include)
target_compile_definitions(bus_socketcan PRIVATE _POSIX_C_SOURCE=200809L)

# 协议栈层：CANopen 客户端服务（DS 301 子集），不知道有几个从站
#   cob.c          COB-ID 算式与反判（纯函数）
#   nmt.c          NMT 命令 → 0x000 帧（纯函数）
#   hb.c           心跳状态（纯时间戳，超时时长由调用方给）
#   sdo_client.c   SDO 客户端：站号 + 事务槽 + 注入的 tx 回调
#   sdo_channel.c  一次 SDO 事务的纯状态机（零 I/O、零锁，可离线单测）
#   sdo_frame.c    SDO 报文编解码（纯函数，可单测）
add_library(canopen_stack STATIC
	controller/canopen/src/cob.c
	controller/canopen/src/nmt.c
	controller/canopen/src/hb.c
	controller/canopen/src/sdo_client.c
	controller/canopen/src/sdo_channel.c
	controller/canopen/src/sdo_frame.c)
target_include_directories(canopen_stack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/canopen/include)
target_include_directories(canopen_stack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/canopen/src)

# 纯逻辑单测：SDO 报文编解码（不需要 CAN，不碰硬件）
add_executable(canopen_selftest apps/tools/canopen_selftest/main.c)
target_include_directories(canopen_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/canopen/src)
target_link_libraries(canopen_selftest PRIVATE canopen_stack)

# 主站层：一条总线 + 登记的从站（站号表 / 收发分派 / 心跳超时）
add_library(cats_master STATIC controller/master/src/master.c)
target_include_directories(cats_master PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/master/include)
# master.h 要用 <bus/can_io.h>：bus 是它的下层，路径向上游都可见
target_include_directories(cats_master PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/bus/include)
target_link_libraries(cats_master PUBLIC canopen_stack bus_socketcan)

# DS 402 标准知识（纯函数，零依赖）：PDS 状态解析 / 序列表 / 对象号 / 位定义
# 谁都能链它；它自己不链任何东西，也没有循环与 I/O
add_library(ds402 STATIC controller/ds402/src/pds.c)
target_include_directories(ds402 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/ds402/include)

# 纯逻辑单测：状态字解析 + 起点快进（不需要 CAN）
add_executable(ds402_selftest apps/tools/ds402_selftest/main.c)
target_link_libraries(ds402_selftest PRIVATE ds402)

# 设备运行期：一个 402 从站（od_access 抽象 + 序列表推进 + PP）
#   drive.c      意图受理 / 服务执行 / 反馈刷新
#   drive_seq.c  402 流程：唯一一处"写控制字 + 等状态字"（序列表来自 ds402）
#   drive_pp.c   PP 运动的参数下发与到位判定
#   backends/    od_access 的 SDO 实现（阻塞式，condvar 让出 CPU）
add_library(drive STATIC
	controller/drive/src/drive.c
	controller/drive/src/drive_seq.c
	controller/drive/src/drive_pp.c
	controller/drive/src/backends/od_sdo.c)
target_include_directories(drive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/drive/include)
target_include_directories(drive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/drive/src)
target_link_libraries(drive PUBLIC ds402 canopen_stack Threads::Threads)
# pthread_cond_timedwait / clock_gettime / clock_nanosleep 属 POSIX 2008
target_compile_definitions(drive PRIVATE _POSIX_C_SOURCE=200809L)

# 轴层：轴集合（每节点一条 SDO 通道 ⇒ 各轴并行使能、互不阻塞）
add_library(cats_axis STATIC controller/axis/src/axis.c)
target_include_directories(cats_axis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/axis/include)
# 机械换算常量（cats_mech.h）属于整机配置，只给轴层用
target_include_directories(cats_axis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(cats_axis PUBLIC drive)

# 应用层：QP 风格层次状态机（HSM）
#   hsm/          机制层：sm_hsm 内核（层次 + LCA + INIT 下钻）
#   evt_queue/    多生产者事件队列（外部线程投递 → 控制线程消费）
#   states/       业务状态：一状态一文件，states.c 是唯一注册点
#   control.c     装配与对外接口：API → 事件
# 通用层次状态机内核：零业务依赖，谁都能链（可单独单测）
add_library(cats_hsm STATIC controller/hsm/src/sm_hsm.c)
target_include_directories(cats_hsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/hsm/include)

# 业务状态机：本机的状态图 + 事件队列（只拿轴的事实，不认识 canopen）
add_library(cats_statemachine STATIC
	controller/statemachine/src/statemachine.c
	controller/statemachine/src/evt_queue/evt_queue.c
	controller/statemachine/src/states/states.c
	controller/statemachine/src/states/state_root.c
	controller/statemachine/src/states/state_online.c
	controller/statemachine/src/states/state_preparing.c
	controller/statemachine/src/states/state_configuring.c
	controller/statemachine/src/states/state_enabling.c
	controller/statemachine/src/states/state_holding.c
	controller/statemachine/src/states/state_moving.c
	controller/statemachine/src/states/state_halting.c
	controller/statemachine/src/states/state_fault.c
	controller/statemachine/src/states/state_estop.c)
target_include_directories(cats_statemachine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/statemachine/include)
target_include_directories(cats_statemachine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/statemachine/src ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(cats_statemachine PUBLIC cats_hsm cats_axis)

# 组合根：装配 + 推进步骤 + 发布（唯一知道全部的地方；不创建线程）
add_library(cats_control STATIC controller/root.c)
target_include_directories(cats_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/include)
target_include_directories(cats_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
# Threads 仅用于同步原语（drive/backends/od_sdo 的 mutex/cond）；本库不创建线程
target_link_libraries(cats_control PUBLIC cats_statemachine cats_axis drive cats_master
	Threads::Threads)
target_compile_definitions(cats_control PRIVATE _POSIX_C_SOURCE=200809L)

# 集成层：本机 Qt 壳（在 apps/ 下，与 controller/ 的库层分开：这一层产出可执行程序）
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# 纯界面：shell 装配 / ui 区块 / theme 调色板；不引用 app、不选后端
add_executable(cats_qt
	apps/qt/main.cpp
	apps/qt/bindings/app_binder.cpp
	apps/qt/bindings/can_thread.cpp
	apps/qt/shell/mainwindow.cpp
	apps/qt/ui/axis_card.cpp
	apps/qt/ui/log_panel.cpp
	apps/qt/ui/status_led.cpp
	apps/qt/ui/state_bar.cpp
	apps/qt/theme/theme.cpp
	apps/qt/res/qt.qrc)
target_include_directories(cats_qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps/qt)
# cats_control / bus_socketcan 保留在链接里，逻辑接回时直接可用
target_link_libraries(cats_qt PRIVATE cats_control bus_socketcan Qt6::Widgets Threads::Threads)

# 工具：无界面运行器。按固定节拍推进 cats_control_poll()，用来在没有界面时验证逻辑
add_executable(master_probe apps/tools/master_probe/main.c)
target_include_directories(master_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(master_probe PRIVATE cats_control bus_socketcan)
target_compile_definitions(master_probe PRIVATE _POSIX_C_SOURCE=200809L)
