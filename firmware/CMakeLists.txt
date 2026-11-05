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
#   exe(qt) → control → canopen ；backend 只实现 can_io.h，被 exe 注入
# ---------------------------------------------------------------

# 内核层：平台无关，只认识自己定义的 can_io.h / master.h
# （具体定义见下方 canopen_node / canopen_kernel）

# 边界层：can_io.h 的本机实现；只拿内核的“头”，不链它的实现
add_library(backend_socketcan STATIC src/backends/socketcan/src/can_io_socketcan.c)
target_include_directories(backend_socketcan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/socketcan/include)
target_include_directories(backend_socketcan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/canopen/include)
target_compile_definitions(backend_socketcan PRIVATE _POSIX_C_SOURCE=200809L)

# 第三方：CANopenNode 301 协议栈（子模块，只读）
#   - 头文件用 "301/CO_xxx.h" 风格，所以把仓库根放进 include 路径
#   - CO_driver_target.h 由本工程提供：config/canopen_node/
add_library(canopen_node STATIC
	src/third_party/CANopenNode/CANopen.c
	src/third_party/CANopenNode/301/CO_Emergency.c
	src/third_party/CANopenNode/301/CO_HBconsumer.c
	src/third_party/CANopenNode/301/CO_NMT_Heartbeat.c
	src/third_party/CANopenNode/301/CO_ODinterface.c
	src/third_party/CANopenNode/301/CO_PDO.c
	src/third_party/CANopenNode/301/CO_SDOclient.c
	src/third_party/CANopenNode/301/CO_SDOserver.c
	src/third_party/CANopenNode/301/CO_SYNC.c
	src/third_party/CANopenNode/301/CO_TIME.c
	src/third_party/CANopenNode/301/CO_fifo.c
	src/third_party/CANopenNode/301/crc16-ccitt.c
	# 照抄自上游 example/ 的对象字典（生成于 DS301_profile.xpd）
	${CMAKE_CURRENT_SOURCE_DIR}/config/canopen_node/OD.c)
target_include_directories(canopen_node PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/CANopenNode
	${CMAKE_CURRENT_SOURCE_DIR}/config/canopen_node)
# 这些 CO_CONFIG_* 在 301/CO_config.h 里只有文档、真实值由编译参数给；
# 用 PUBLIC：链接本库的源文件也必须看到，否则对应头文件整段被预处理掉。
# 注意：CMake 不接受 "(MACRO)" 这种函数式写法，必须给字面量。
target_compile_definitions(canopen_node PUBLIC
	CO_CONFIG_SDO_CLI=1
	CO_CONFIG_FIFO=1  # SDO client 依赖 FIFO 缓冲
	# NMT：CO_CONFIG_NMT_MASTER(0x02) —— 主站要能发 NMT 命令
	CO_CONFIG_NMT=2
	# 心跳消费：ENABLE(0x01) | QUERY_FUNCT(0x08) —— QUERY_FUNCT 才有
	#   getState()，本层只靠它回答"某个节点还活着吗"，不需要按节点的回调
	CO_CONFIG_HB_CONS=9
	# 静态分配：CO_new() 返回指向 static 对象的指针，全程不用 malloc
	CO_USE_GLOBALS
	# 用不到的功能关掉，省代码也省链接（站号是写死的，不做 LSS；没有 LED）
	CO_CONFIG_LSS=0
	CO_CONFIG_LEDS=0)
# 上游在"未启用的可选功能"下会报 unused parameter，属上游代码，不修；
# 只在本目标内静音，避免刷我们自己的构建日志。
target_compile_options(canopen_node PRIVATE -Wno-unused-parameter)

# 内核层：主站对外接口（master.h）+ 用 can_io 实现的 CO_CANmodule
#   master.c        CANopenNode 的薄封装（主站语义）
#   node/           驱动适配层：收帧分派、发送、rx/tx 槽管理
add_library(canopen_kernel STATIC
	src/canopen/src/master.c
	src/canopen/src/node/co_driver_cats.c)
target_include_directories(canopen_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/canopen/include)
target_include_directories(canopen_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/canopen/src/node)
target_link_libraries(canopen_kernel PUBLIC canopen_node)

# 设备层：CiA 402（驱动器语义）。控制字/状态字/使能序列只在这一层
#   用上游的 SDO 客户端原语驱动（canopen/sdo_client.h 只交出对象，不封装动作）
add_library(cia402 STATIC
	src/cia402/src/cia402.c
	src/cia402/src/cia402_config.c
	src/cia402/src/cia402_enable.c
	src/cia402/src/cia402_monitor.c
	src/cia402/src/cia402_pp.c
	src/cia402/src/cia402_sdo.c
	src/cia402/src/cia402_state.c
	src/cia402/src/cia402_stop.c)
target_include_directories(cia402 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/cia402/include)
target_link_libraries(cia402 PUBLIC canopen_kernel)

# 轴层：轴集合（每节点一条 SDO 通道 ⇒ 各轴并行使能、互不阻塞）
add_library(cats_axis STATIC src/axis/src/axis.c)
target_include_directories(cats_axis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/axis/include)
target_link_libraries(cats_axis PUBLIC cia402)

# 应用层：QP 风格层次状态机（HSM）
#   hsm/          机制层：sm_hsm 内核（层次 + LCA + INIT 下钻）
#   evt_queue/    多生产者事件队列（外部线程投递 → 控制线程消费）
#   states/       业务状态：一状态一文件，states.c 是唯一注册点
#   control.c     装配与对外接口：API → 事件
add_library(cats_control STATIC
	src/control/src/control.c
	src/control/src/evt_queue/evt_queue.c
	src/control/src/hsm/sm_hsm.c
	src/control/src/states/states.c
	src/control/src/states/state_root.c
	src/control/src/states/state_online.c
	src/control/src/states/state_preparing.c
	src/control/src/states/state_configuring.c
	src/control/src/states/state_enabling.c
	src/control/src/states/state_holding.c
	src/control/src/states/state_moving.c
	src/control/src/states/state_halting.c
	src/control/src/states/state_fault.c
	src/control/src/states/state_estop.c)
target_include_directories(cats_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/control/include)
target_link_libraries(cats_control PUBLIC cats_axis Threads::Threads)

# 集成层：本机 Qt 壳（在 apps/ 下，与 src/ 的库层分开：这一层产出可执行程序）
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
# cats_control / backend_socketcan 保留在链接里，逻辑接回时直接可用
target_link_libraries(cats_qt PRIVATE cats_control backend_socketcan Qt6::Widgets Threads::Threads)

# 工具：无界面运行器。20 ms 推进 cats_control_poll()，用来在没有界面时验证逻辑
add_executable(master_probe tools/master_probe/main.c)
target_link_libraries(master_probe PRIVATE cats_control backend_socketcan)
target_compile_definitions(master_probe PRIVATE _POSIX_C_SOURCE=200809L)
