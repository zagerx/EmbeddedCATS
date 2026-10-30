cmake_minimum_required(VERSION 3.16)
project(cats_firmware LANGUAGES C CXX)

# 导出编译库给 clangd：跳转/补全/格式化都靠它（生成 build/compile_commands.json）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON) # Q_OBJECT 需要 moc
add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------
# 一层一个 target。依赖方向在链接关系里也要立得住：
#   exe(qt) → app → canopen ；backend 只实现 can_io.h，被 exe 注入
# ---------------------------------------------------------------

# 内核层：平台无关，只认识自己定义的 can_io.h
add_library(canopen_kernel STATIC src/canopen/src/sdo_client.c)
target_include_directories(canopen_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/canopen/include)

# 边界层：can_io.h 的本机实现；只拿内核的“头”，不链它的实现
add_library(backend_socketcan STATIC src/backends/socketcan/src/can_io_socketcan.c)
target_include_directories(backend_socketcan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/socketcan/include)
target_include_directories(backend_socketcan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/canopen/include)
target_compile_definitions(backend_socketcan PRIVATE _POSIX_C_SOURCE=200809L)

# 应用层：装配与时序
add_library(cats_app STATIC src/app/src/app.c)
target_include_directories(cats_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app/include)
target_link_libraries(cats_app PUBLIC canopen_kernel)

# 集成层：本机 Qt 壳
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(cats_qt src/qt/main.cpp src/qt/mainwindow.cpp)
target_include_directories(cats_qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/qt)
target_link_libraries(cats_qt PRIVATE cats_app backend_socketcan Qt6::Widgets)
