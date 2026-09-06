# =============================================================================
# 农田土壤参数移动采集系统 V1.0
# Farmland Soil Parameter Mobile Collection System
#
# 目标平台: 嵌入式 Linux (ARM Cortex-A53, Linux Kernel 5.10)
# 开发平台: Windows / Linux (使用模拟硬件抽象层)
# 编译器:   GCC 9.3+ / Clang 10+ / MSVC 2019+ (C++17)
# =============================================================================
cmake_minimum_required(VERSION 3.12)
project(SoilCollectionSystem VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# 主程序：系统全部功能模块
add_executable(soil_collection_system
    src/main.cpp
    src/logger.cpp
    src/hardware_abstraction.cpp
    src/sensing_module.cpp
    src/fusion_module.cpp
    src/storage_module.cpp
    src/comm_module.cpp
    src/autonomy_module.cpp
    src/system_orchestrator.cpp
    src/web_server.cpp
)

target_include_directories(soil_collection_system PRIVATE include)

if(WIN32)
    target_link_libraries(soil_collection_system PRIVATE ws2_32)
else()
    target_link_libraries(soil_collection_system PRIVATE pthread)
    # 嵌入式目标下链接实时库（PREEMPT_RT 内核）
    find_library(RT_LIB rt)
    if(RT_LIB)
        target_link_libraries(soil_collection_system PRIVATE ${RT_LIB})
    endif()
endif()
