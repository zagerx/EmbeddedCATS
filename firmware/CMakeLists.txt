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

# ---------------------------------------------------------------
# 一层一个 target。依赖方向在链接关系里也要立得住：
#   exe(qt) → app → canopen ；backend 只实现 can_io.h，被 exe 注入
# ---------------------------------------------------------------

# 内核层：平台无关，只认识自己定义的 can_io.h / master.h / sdo_client.h
add_library(canopen_kernel STATIC src/canopen/src/sdo_client.c src/canopen/src/master.c)
target_include_directories(canopen_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/canopen/include)

# 边界层：can_io.h 的本机实现；只拿内核的“头”，不链它的实现
add_library(backend_socketcan STATIC src/backends/socketcan/src/can_io_socketcan.c)
target_include_directories(backend_socketcan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/socketcan/include)
target_include_directories(backend_socketcan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/canopen/include)
target_compile_definitions(backend_socketcan PRIVATE _POSIX_C_SOURCE=200809L)

# 应用层：QP 风格状态机
#   hsm/          机制层：状态即函数指针，dispatch 统一做 EXIT→ENTRY
#   states/       业务状态：一状态一文件，states.c 是唯一注册点
#   app.c         装配与对外接口：API → 事件
add_library(cats_app STATIC
	src/app/src/app.c
	src/app/src/hsm/hsm.c
	src/app/src/states/states.c
	src/app/src/states/state_root.c
	src/app/src/states/state_operational.c
	src/app/src/states/state_preparing.c
	src/app/src/states/state_running.c
	src/app/src/states/state_stopping.c
	src/app/src/states/state_fault.c
	src/app/src/states/state_estop.c)
target_include_directories(cats_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app/include)
target_link_libraries(cats_app PUBLIC canopen_kernel)

# 集成层：本机 Qt 壳
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# 纯界面：shell 装配 / ui 区块 / theme 调色板；不引用 app、不选后端
add_executable(cats_qt
	src/qt/main.cpp
	src/qt/bindings/app_binder.cpp
	src/qt/shell/mainwindow.cpp
	src/qt/ui/axis_card.cpp
	src/qt/ui/log_panel.cpp
	src/qt/ui/status_led.cpp
	src/qt/ui/state_bar.cpp
	src/qt/theme/theme.cpp
	src/qt/res/qt.qrc)
target_include_directories(cats_qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qt)
# cats_app / backend_socketcan 保留在链接里，逻辑接回时直接可用
target_link_libraries(cats_qt PRIVATE cats_app backend_socketcan Qt6::Widgets)

# 工具：无界面运行器。20 ms 推进 cats_app_poll()，用来在没有界面时验证逻辑
add_executable(master_probe tools/master_probe/main.c)
target_link_libraries(master_probe PRIVATE cats_app backend_socketcan)
target_compile_definitions(master_probe PRIVATE _POSIX_C_SOURCE=200809L)

# 工具：状态机骨架（一状态一文件，main.c 暂空）。
# 只编 hsm.c，不链 cats_app —— 顺带证明内核零依赖。
set(HSM_DEMO_SRCS
	tools/hsm_demo/states.c
	tools/hsm_demo/state_root.c
	tools/hsm_demo/state_operational.c
	tools/hsm_demo/state_idle.c
	tools/hsm_demo/state_running.c
	tools/hsm_demo/state_stopping.c
	tools/hsm_demo/state_fault.c
	tools/hsm_demo/state_estop.c)

add_executable(hsm_demo tools/hsm_demo/main.c ${HSM_DEMO_SRCS} src/app/src/hsm/hsm.c)
target_include_directories(hsm_demo PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src/app/src/hsm
	${CMAKE_CURRENT_SOURCE_DIR}/tools/hsm_demo)
target_compile_definitions(hsm_demo PRIVATE _POSIX_C_SOURCE=200809L)
