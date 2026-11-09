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

# pthread 同步原语与宿主线程
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

# 主站层：一条总线 + 登记的从站（站号表 / 收发分派 / 心跳超时）
add_library(cats_master STATIC controller/master/src/master.c)
target_include_directories(cats_master PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/master/include)
# master.h 要用 <bus/can_io.h>：bus 是它的下层，路径向上游都可见
target_include_directories(cats_master PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/bus/include)
target_link_libraries(cats_master PUBLIC canopen_node bus_socketcan)

# ---------------------------------------------------------------
# CANopenNode v4（第三方协议栈，原样参与构建）
#   third_party/CANopenNode/  官方源码（只读，不改）
#   canopen_node/             本机移植层：CO_driver 用我们的 bus 实现
#   config/canopen_node/      对象字典：心跳消费 0x1016 / SDO 客户端 0x1280
# 只开本工程用到的服务：NMT master、心跳消费（含查询）、SDO 客户端。
# ---------------------------------------------------------------
set(CANOPENNODE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/controller/third_party/CANopenNode)
add_library(canopen_node STATIC
	${CANOPENNODE_DIR}/CANopen.c
	${CANOPENNODE_DIR}/301/CO_Emergency.c
	${CANOPENNODE_DIR}/301/CO_fifo.c
	${CANOPENNODE_DIR}/301/CO_HBconsumer.c
	${CANOPENNODE_DIR}/301/CO_NMT_Heartbeat.c
	${CANOPENNODE_DIR}/301/CO_Node_Guarding.c
	${CANOPENNODE_DIR}/301/CO_ODinterface.c
	${CANOPENNODE_DIR}/301/CO_PDO.c
	${CANOPENNODE_DIR}/301/CO_SDOclient.c
	${CANOPENNODE_DIR}/301/CO_SDOserver.c
	${CANOPENNODE_DIR}/301/CO_SYNC.c
	${CANOPENNODE_DIR}/301/CO_TIME.c
	${CANOPENNODE_DIR}/301/crc16-ccitt.c
	controller/canopen_node/co_driver_cats.c
	controller/config/canopen_node/OD.c)
target_include_directories(canopen_node PUBLIC
	${CANOPENNODE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/controller/canopen_node
	${CMAKE_CURRENT_SOURCE_DIR}/controller/config/canopen_node
	${CMAKE_CURRENT_SOURCE_DIR}/controller/bus/include)
# 这些宏决定 CANopenNode 头里暴露哪些类型/服务，所有包含 CANopen.h 的
# 编译单元必须一致 → PUBLIC
target_compile_definitions(canopen_node PUBLIC
	CO_USE_GLOBALS=1
	CO_CONFIG_NMT=0x02
	CO_CONFIG_HB_CONS=0x09
	CO_CONFIG_FIFO=0x01
	CO_CONFIG_SDO_CLI=0x01
	CO_CONFIG_LSS=0
	CO_CONFIG_LEDS=0
	CO_CONFIG_GTW=0
	CO_CONFIG_SRDO=0)
# 第三方代码：按配置裁剪后会留下"未使用参数"，只对本 target 关掉这条告警
target_compile_options(canopen_node PRIVATE -Wno-unused-parameter)

# 对象访问契约与 CANopen SDO 后端。契约不认识具体协议，后端不实现 DS402 语义。
add_library(object_access INTERFACE)
target_include_directories(object_access INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/controller/transport/include)

add_library(canopen_sdo STATIC controller/transport/src/sdo_client.c)
target_include_directories(canopen_sdo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/transport/include)
target_link_libraries(canopen_sdo PUBLIC object_access canopen_node Threads::Threads)
target_compile_definitions(canopen_sdo PRIVATE _POSIX_C_SOURCE=200809L)

# CiA 402：pds.c 为纯标准知识；device*.c 为请求、同步流程和反馈。
# 本模块只依赖 object_access 契约，不依赖 CANopenNode。
add_library(cia402 STATIC
	controller/cia402/src/pds.c
	controller/cia402/src/device.c
	controller/cia402/src/device_seq.c
	controller/cia402/src/device_pp.c)
target_include_directories(cia402 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/cia402/include)
target_include_directories(cia402 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/cia402/src)
target_link_libraries(cia402 PUBLIC object_access Threads::Threads)

# 纯逻辑单测：状态字解析 + 起点快进（不需要 CAN）
add_executable(cia402_selftest apps/tools/cia402_selftest/main.c)
target_link_libraries(cia402_selftest PRIVATE cia402)

# 轴层：单位换算与多轴请求转发；每节点使用独立对象访问通道
add_library(cats_axis STATIC controller/axis/src/axis.c)
target_include_directories(cats_axis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/axis/include)
target_include_directories(cats_axis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(cats_axis PUBLIC cia402)

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
add_library(cats_control STATIC
	controller/controller.c
	controller/control_api.c)
target_include_directories(cats_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/include)
target_include_directories(cats_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
# 本库只装配并调用各层，不创建线程
target_link_libraries(cats_control PUBLIC cats_statemachine cats_axis cia402 canopen_sdo cats_master
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
# Qt 宿主链接完整控制栈与 SocketCAN 后端
target_link_libraries(cats_qt PRIVATE cats_control bus_socketcan Qt6::Widgets Threads::Threads)

# 工具：无界面运行器。按固定节拍推进 cats_control_poll()，用来在没有界面时验证逻辑
add_executable(master_probe apps/tools/master_probe/main.c)
target_include_directories(master_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(master_probe PRIVATE cats_control bus_socketcan)
target_compile_definitions(master_probe PRIVATE _POSIX_C_SOURCE=200809L)
