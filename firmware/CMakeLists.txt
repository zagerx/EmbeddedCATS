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

# pthread 同步原语、宿主线程与 Qt 界面
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# 第三方入口集中管理 Linux 驱动、epoll 与裁剪后的 CANopenNode 服务。
add_subdirectory(controller/third_party)

# 主站持有接口、epoll 和协议栈资源，不创建线程。
add_library(cats_master STATIC controller/canopen/master/src/master.c)
target_include_directories(cats_master PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/canopen/master/include)
target_link_libraries(cats_master PUBLIC canopen_node)
target_compile_definitions(cats_master PRIVATE _DEFAULT_SOURCE)

# 对象访问契约与 CANopen SDO 后端。契约不认识具体协议，后端不实现 DS402 语义。
add_library(sdo_access INTERFACE)
target_include_directories(sdo_access INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/controller/canopen/transport/include)

add_library(canopen_sdo STATIC controller/canopen/transport/src/sdo_client.c)
target_include_directories(canopen_sdo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/canopen/transport/include)
target_link_libraries(canopen_sdo PUBLIC sdo_access canopen_node)
target_compile_definitions(canopen_sdo PRIVATE _POSIX_C_SOURCE=200809L)

# CiA 402：pds.c 为纯标准知识；device.c 为装配与推进；states/ 一状态一文件。
# 本模块只依赖 sdo_access 契约与 hsm 机制，不依赖 CANopenNode。
add_library(cia402 STATIC
        controller/cia402/src/pds.c
        controller/cia402/src/device.c
        controller/cia402/src/device_internal.c
        controller/cia402/src/states/states.c
        controller/cia402/src/states/state_idle.c
        controller/cia402/src/states/state_running.c
        controller/cia402/src/states/state_config.c
        controller/cia402/src/states/state_enable.c
        controller/cia402/src/states/state_disable.c
        controller/cia402/src/states/state_pp.c
        controller/cia402/src/states/state_halt.c
        controller/cia402/src/states/state_quick_stop.c)
target_include_directories(cia402 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/cia402/include)
target_include_directories(cia402 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/cia402/src)
target_link_libraries(cia402 PUBLIC sdo_access)
target_link_libraries(cia402 PUBLIC cats_hsm)

# 纯逻辑单测：状态字解析 + 起点快进（不需要 CAN）
add_executable(cia402_selftest
	apps/tools/cia402_selftest/main.c
	apps/tools/cia402_selftest/device_test.c)
target_link_libraries(cia402_selftest PRIVATE cia402)

# 轴层：单位换算与多轴请求转发；每节点使用独立对象访问通道
add_library(cats_axis STATIC controller/axis/src/axis.c)
target_include_directories(cats_axis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/axis/include)
target_include_directories(cats_axis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(cats_axis PUBLIC cia402 canopen_sdo)

# 通用层次状态机内核：零业务依赖，谁都能链（可单独单测）
add_library(cats_hsm STATIC controller/hsm/src/sm_hsm.c)
target_include_directories(cats_hsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/hsm/include)

# 控制层：组合根 + 输入事件队列 + 状态图，统一持有一个控制器上下文。
add_library(cats_control STATIC
	controller/controller.c
	controller/controller_actions.c
	controller/control_api.c
	controller/evt_queue.c
	controller/states/states.c
	controller/states/state_root.c
	controller/states/state_online.c
	controller/states/state_preparing.c
	controller/states/state_configuring.c
	controller/states/state_enabling.c
	controller/states/state_holding.c
	controller/states/state_moving.c
	controller/states/state_halting.c
	controller/states/state_fault.c
	controller/states/state_estop.c)
target_include_directories(cats_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/controller/include)
target_include_directories(cats_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller
	${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
# 本库持有控制对象并调用各层，不创建线程
target_link_libraries(cats_control PUBLIC cats_hsm cats_axis cia402 canopen_sdo cats_master
	Threads::Threads)
target_compile_definitions(cats_control PRIVATE _POSIX_C_SOURCE=200809L)

# 集成层：本机 Qt 壳（在 apps/ 下，与 controller/ 的库层分开：这一层产出可执行程序）

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
target_link_libraries(cats_qt PRIVATE cats_control Qt6::Widgets Threads::Threads)

# 工具：主线程直接运行 CANopen epoll 循环，用于无界面端到端验证
add_executable(master_probe apps/tools/master_probe/main.c)
target_include_directories(master_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(master_probe PRIVATE cats_control)
target_compile_definitions(master_probe PRIVATE _POSIX_C_SOURCE=200809L)
