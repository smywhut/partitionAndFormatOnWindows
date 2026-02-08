cmake_minimum_required(VERSION 3.15)

# 项目名称
project(DiskPartitionTool LANGUAGES CXX)

# 设置 C++ 标准 (推荐 C++17 或更高)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源文件列表
set(SOURCE_FILES src/main.cpp)

# 创建可执行文件
add_executable(${PROJECT_NAME} ${SOURCE_FILES})

# --------------------------------------------------------
# Windows 平台特定配置
# --------------------------------------------------------
if(WIN32)
    # 1. 定义 Unicode 宏
    # Windows API 编程的最佳实践，确保 TCHAR 映射为 WCHAR
    target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE)

    # 2. 链接库
    # MI (Management Infrastructure) API 需要 mi.lib
    # 注意：mi.lib 包含在 Windows SDK 中，MSVC 通常能自动找到
    target_link_libraries(${PROJECT_NAME} PRIVATE mi)

    # 3. 配置 UAC (用户账户控制)
    # 磁盘操作必须需要管理员权限。
    # 添加此链接器标志后，生成的 .exe 图标上会有盾牌，
    # 且双击运行时会自动弹出 UAC 提权提示，防止因权限不足导致 "Access Denied"。
    if(MSVC)
        set_target_properties(${PROJECT_NAME} PROPERTIES 
            LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
        )
    endif()
    
    # 4. 编译选项优化 (可选)
    # /W4: 高警告级别
    # /EHsc: 标准 C++ 异常处理
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /W4 /EHsc)
    endif()
endif()