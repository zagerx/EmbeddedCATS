# controller/ — 自包含的控制栈
#
# 任何宿主工程（本仓库的 apps/、外部的 build_all 等）只要：
#     add_subdirectory(<path>/firmware/controller)
#     target_link_libraries(自己的目标 PRIVATE cats_control)
# 就能编进整条控制栈，不需要知道下面分了哪几层。
#
# 依赖只向下：cats_control → cats_axis → cia402 → sdo_access ← canopen_sdo ← canopen_node。
# 对外只暴露 include/（control/control.h）；其余各层的 include 都是本目录内部的事。
# 本文件不假设父工程设过语言标准或警告选项，全部按目标自己声明。

find_package(Threads REQUIRED)

# 本目录所有 C 目标统一：C99 + 全警告（与父工程的设置无关）
function(cats_c_target name)
	target_compile_features(${name} PRIVATE c_std_99)
	target_compile_options(${name} PRIVATE -Wall -Wextra)
endfunction()

# 第三方：Linux 驱动、epoll 与裁剪后的 CANopenNode 服务
add_subdirectory(third_party)
cats_c_target(canopen_node)

# 通用层次状态机内核：零业务依赖，谁都能链
add_library(cats_hsm STATIC hsm/src/sm_hsm.c)
target_include_directories(cats_hsm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/hsm/include)
cats_c_target(cats_hsm)

# 主站持有接口、epoll 和协议栈资源，不创建线程
add_library(cats_master STATIC canopen/master/src/master.c)
target_include_directories(cats_master PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/canopen/master/include)
target_link_libraries(cats_master PUBLIC canopen_node)
target_compile_definitions(cats_master PRIVATE _DEFAULT_SOURCE)
cats_c_target(cats_master)

# 对象访问契约：只有类型，不认识具体协议
add_library(sdo_access INTERFACE)
target_include_directories(sdo_access INTERFACE
	${CMAKE_CURRENT_SOURCE_DIR}/canopen/transport/include)

# SDO 后端：事务推进，不实现 DS402 语义。通道由 402 独占，不再仲裁。
add_library(canopen_sdo STATIC canopen/transport/src/sdo_client.c)
target_include_directories(canopen_sdo PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/canopen/transport/include)
target_link_libraries(canopen_sdo PUBLIC sdo_access canopen_node)
target_compile_definitions(canopen_sdo PRIVATE _POSIX_C_SOURCE=200809L)
cats_c_target(canopen_sdo)

# 周期反馈：在协议栈的阳置接收槽上接住从站 TPDO 并解包。
# 布局与 apps/tools/drive_setup 写入的一致；本层不认识 402 业务。
add_library(canopen_pdo STATIC canopen/pdo/src/tpdo.c)
target_include_directories(canopen_pdo PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/canopen/pdo/include)
target_link_libraries(canopen_pdo PUBLIC cats_master)
cats_c_target(canopen_pdo)

# CiA 402：pds.c 为纯标准知识；device.c 为装配与推进；states/ 一状态一文件。
# 本模块只依赖 sdo_access 契约与 hsm 机制，不依赖 CANopenNode。
add_library(cia402 STATIC
	cia402/src/pds.c
	cia402/src/device.c
	cia402/src/device_internal.c
	cia402/src/states/states.c
	cia402/src/states/idle.c
	cia402/src/states/operation_active.c
	cia402/src/operations/config.c
	cia402/src/operations/enable.c
	cia402/src/operations/disable.c
	cia402/src/operations/move.c
	cia402/src/operations/halt.c
	cia402/src/operations/homing.c
	cia402/src/operations/quick_stop.c)
target_include_directories(cia402 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cia402/include)
target_include_directories(cia402 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cia402/src)
target_link_libraries(cia402 PUBLIC sdo_access cats_hsm)
cats_c_target(cia402)

# 轴模块：单轴单位换算与请求，整机轴集合动作与结果汇总
add_library(cats_axis STATIC
	controller_axis/axis/axis.c
	controller_axis/controller_axis.c)
target_include_directories(cats_axis PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/controller_axis/include)
target_include_directories(cats_axis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/config)
target_link_libraries(cats_axis PUBLIC cia402 canopen_sdo canopen_pdo)
cats_c_target(cats_axis)

# 控制层：组合根 + 输入事件队列 + 状态图。
# 对外的头文件只有 include/（control/control.h、control/state_types.h），
# 因此下面这些依赖全部 PRIVATE：宿主链 cats_control 之后看不到任何内部头。
add_library(cats_control STATIC
	config/axis_map.c
	controller.c
	controller_actions.c
	controller_feedback.c
	control_api.c
	controller_evtqueue/evt_queue.c
	controller_states/states.c
	controller_states/state_root.c
	controller_states/state_preparing.c
	controller_states/state_configuring.c
	controller_states/state_idle.c
	controller_states/state_enabling.c
	controller_states/state_holding.c
	controller_states/state_moving.c
	controller_states/state_halting.c
	controller_states/state_homing.c
	controller_states/state_fault.c
	controller_states/state_estop.c)
target_include_directories(cats_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(cats_control PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/config
	${CMAKE_CURRENT_SOURCE_DIR}/controller_evtqueue)
# 本库持有控制对象并调用各层，不创建线程
target_link_libraries(cats_control PRIVATE
	cats_hsm cats_axis cia402 canopen_sdo canopen_pdo cats_master Threads::Threads)
target_compile_definitions(cats_control PRIVATE _POSIX_C_SOURCE=200809L)
cats_c_target(cats_control)
