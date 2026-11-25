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

# 宿主侧需要的运行时：Qt 界面与线程原语（控制栈自己也会 find_package(Threads)）
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# 控制栈是自包含的子工程：外壳只链接 cats_control，不需要知道下面分了哪几层。
# 同一行写法可以直接搬到外部工程（例如 build_all）。
add_subdirectory(controller)

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
# Qt 宿主链接完整控制栈（内核、协议栈、轴与业务状态图都在 cats_control 里）
target_link_libraries(cats_qt PRIVATE cats_control Qt6::Widgets Threads::Threads)
target_compile_options(cats_qt PRIVATE -Wall -Wextra)

# 单层验证件：只链被测的那一层，不牵动整条控制栈
add_executable(cia402_selftest
	apps/tools/cia402_selftest/main.c
	apps/tools/cia402_selftest/device_test.c)
target_link_libraries(cia402_selftest PRIVATE cia402)
target_compile_options(cia402_selftest PRIVATE -Wall -Wextra)

# 真实 SDO 客户端 + 测试专用 CANopenNode 协议桩：不创建 CAN 接口
add_executable(sdo_client_selftest
	apps/tools/sdo_client_selftest/main.c
	controller/canopen/sdo/src/sdo_client.c)
target_include_directories(sdo_client_selftest PRIVATE
	apps/tools/sdo_client_selftest/include)
target_link_libraries(sdo_client_selftest PRIVATE sdo_iface)
target_compile_options(sdo_client_selftest PRIVATE -Wall -Wextra)

# 控制层纯逻辑回归：不创建 CAN 接口，也不驱动任何电机
add_executable(controller_fault_selftest
	apps/tools/controller_fault_selftest/main.c
	apps/tools/controller_fault_selftest/sdo_client_fake.c
	controller/controller_axis/axis/axis.c
	controller/controller_axis/controller_axis.c
	controller/controller_log.c
	controller/controller_fault.c
	controller/controller_evtqueue/evt_queue.c)
target_include_directories(controller_fault_selftest PRIVATE
	controller
	controller/include
	controller/config
	controller/controller_evtqueue
	controller/controller_axis/include
	controller/cia402/include
	controller/canopen/master/include
	controller/canopen/pdo/include
	controller/canopen/sdo/include
	controller/hsm/include)
target_link_libraries(controller_fault_selftest PRIVATE cia402 cats_hsm canopen_node Threads::Threads)
target_compile_options(controller_fault_selftest PRIVATE -Wall -Wextra)

# 工具：主线程直接运行 CANopen epoll 循环，用于无界面端到端验证
add_executable(master_probe apps/tools/master_probe/main.c)
target_include_directories(master_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/controller/config)
target_link_libraries(master_probe PRIVATE cats_control)
target_compile_definitions(master_probe PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(master_probe PRIVATE -Wall -Wextra)
