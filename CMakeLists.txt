# CMakeList.txt: shp-merge 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.10)

# 生成 compile_commands.json 供 clangd 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project ("ffmpeg-merge")

# 如果支持，请为 MSVC 编译器启用热重载（仅在 MSVC 编译器时设置）
if(MSVC)
  if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
  endif()
endif()


set(FFMPEG_ROOT "D:/Devpkg/ffmpeg/ffmpeg-n4.4.5-gpl-amd64-shared")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")


# 将源代码添加到此项目的可执行文件。
add_executable (ffmpeg-merge 
    "win-thread.cpp"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET ffmpeg-merge PROPERTY CXX_STANDARD 20)
endif()

# 使用 target_include_directories 替代全局 include_directories
target_include_directories(ffmpeg-merge PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_directories(ffmpeg-merge PRIVATE ${FFMPEG_LIB_DIR})
# target_link_libraries(ffmpeg-merge PRIVATE     
#     avcodec
#     avdevice
#     avfilter
#     avformat
#     avutil
#     postproc
#     swresample
#     swscale)

# 设置运行时 DLL 路径（Windows）
if(WIN32)
    set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
    
    # 检查 bin 目录是否存在
    if(EXISTS "${FFMPEG_BIN_DIR}")
        # 设置 Visual Studio 调试器环境变量（使用正斜杠，CMake 会自动转换）
        set_target_properties(ffmpeg-merge PROPERTIES
            VS_DEBUGGER_ENVIRONMENT "PATH=${FFMPEG_BIN_DIR};$ENV{PATH}"
        )
        
        # 获取所有需要复制的 DLL 文件列表
        file(GLOB FFMPEG_DLLS "${FFMPEG_BIN_DIR}/*.dll")
        
        # 复制所有 DLL 到输出目录
        if(FFMPEG_DLLS)
            foreach(DLL ${FFMPEG_DLLS})
                get_filename_component(DLL_NAME ${DLL} NAME)
                add_custom_command(TARGET ffmpeg-merge POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}"
                    $<TARGET_FILE_DIR:ffmpeg-merge>
                    COMMENT "Copying ${DLL_NAME} to output directory"
                )
            endforeach()
        else()
            message(WARNING "未找到 FFmpeg DLL 文件在: ${FFMPEG_BIN_DIR}")
        endif()
    else()
        message(WARNING "FFmpeg bin directory not found: ${FFMPEG_BIN_DIR}")
    endif()
    
endif()

# 安装规则
install(TARGETS ffmpeg-merge
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)

# 安装配置文件（如果存在）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.ini")
    install(FILES config.ini
        DESTINATION bin
        COMPONENT Runtime
    )
endif()

# 安装所有 DLL 文件（Windows）
if(WIN32)
    if(FFMPEG_DLLS)
        install(FILES ${FFMPEG_DLLS}
            DESTINATION bin
            COMPONENT Runtime
        )
    endif()
endif()
