cmake_minimum_required(VERSION 3.24)
project(ARZ VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ Runtime (/MT) -> 100% Zero external VC++ Redistributable dependency on client machines!
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PREMIERE_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/premiere_sdk/Premiere Pro 27.0 C++ SDK"
    CACHE PATH "Path to official Adobe Premiere Pro 27.0 C++ SDK")

set(FFMPEG_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/ffmpeg-master-latest-win64-lgpl-shared"
    CACHE PATH "Path to FFmpeg 9.x win64 LGPL shared dev package")

set(ARZ_INSTALL_DIR
    "C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore/ARZ"
    CACHE PATH "Shared Adobe MediaCore ARZ plugin folder (Premiere Pro, Media Encoder & After Effects)")

if(NOT EXISTS "${PREMIERE_SDK_ROOT}/Examples/Headers/PrSDKImport.h")
    message(FATAL_ERROR "Premiere Pro 27.0 SDK headers not found at: ${PREMIERE_SDK_ROOT}")
endif()

if(NOT EXISTS "${FFMPEG_ROOT}/include/libavformat/avformat.h")
    message(FATAL_ERROR "FFmpeg headers not found at: ${FFMPEG_ROOT}")
endif()

set(PREMIERE_INCLUDE_DIRS
    "${PREMIERE_SDK_ROOT}/Examples/Headers"
    "${PREMIERE_SDK_ROOT}/Examples/Utils"
)

set(FFMPEG_LIBS
    "${FFMPEG_ROOT}/lib/avformat.lib"
    "${FFMPEG_ROOT}/lib/avcodec.lib"
    "${FFMPEG_ROOT}/lib/avutil.lib"
    "${FFMPEG_ROOT}/lib/swscale.lib"
    "${FFMPEG_ROOT}/lib/swresample.lib"
)

add_compile_options(/W3 /EHsc /utf-8 /MP /wd4251)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# 1. Core Shared Engine (ARZCore.dll)
add_library(ARZCore SHARED
    src/core/AERConfig.cpp
    src/core/AERTelemetry.cpp
    src/core/AERSession.cpp
    src/cache/GOPSeekIndex.cpp
    src/cache/FrameCache.cpp
    src/cache/ThumbnailCache.cpp
    src/media/DemuxEngine.cpp
    src/media/GpuDeviceManager.cpp
    src/media/VideoDecoder.cpp
    src/media/AudioDecoder.cpp
    src/export/AERExportEngine.cpp
    src/adobe/AERImporter.cpp
    src/adobe/AERExporter.cpp
)
target_compile_definitions(ARZCore PRIVATE AER_CORE_EXPORTS=1 PRWIN_ENV=1 UNICODE _UNICODE)
target_include_directories(ARZCore
    PUBLIC src
    PRIVATE "${FFMPEG_ROOT}/include" ${PREMIERE_INCLUDE_DIRS}
)
target_link_libraries(ARZCore PRIVATE ${FFMPEG_LIBS} d3d11 dxgi)

# 2. Premiere Pro 27 Importer Shims (ARZ.prm & ARZ_HP.prm)
foreach(SUFFIX IN ITEMS "" "_HP")
    set(PRM_TARGET "ARZ${SUFFIX}")
    add_library(${PRM_TARGET} SHARED
        src/adobe/AEREntry.cpp
        src/adobe/AERPiPL.rc
    )
    target_include_directories(${PRM_TARGET} PRIVATE src ${PREMIERE_INCLUDE_DIRS})
    target_compile_definitions(${PRM_TARGET} PRIVATE PRWIN_ENV=1 UNICODE _UNICODE)
    if(SUFFIX STREQUAL "_HP")
        target_compile_definitions(${PRM_TARGET} PRIVATE AER_HIGH_PRIORITY=1)
    else()
        target_compile_definitions(${PRM_TARGET} PRIVATE AER_HIGH_PRIORITY=0)
    endif()
    set_target_properties(${PRM_TARGET} PROPERTIES
        SUFFIX ".prm"
        PREFIX ""
    )
endforeach()

# 3. Premiere Pro 27 Native Exporter Shim (ARZ_Exporter.prm)
add_library(ARZ_Exporter SHARED
    src/adobe/AERExporterEntry.cpp
    src/adobe/AERExporterPiPL.rc
)
target_include_directories(ARZ_Exporter PRIVATE src ${PREMIERE_INCLUDE_DIRS})
target_compile_definitions(ARZ_Exporter PRIVATE PRWIN_ENV=1 UNICODE _UNICODE)
set_target_properties(ARZ_Exporter PROPERTIES
    SUFFIX ".prm"
    PREFIX ""
)

# 4. Bilingual Dark-Mode ARZ Desktop Control Center (ARZDesktop.exe)
add_executable(ARZDesktop WIN32
    src/desktop/ARZDesktop.cpp
    src/desktop/ARZDesktop.rc
)
target_compile_definitions(ARZDesktop PRIVATE UNICODE _UNICODE)
target_link_libraries(ARZDesktop PRIVATE dwmapi shell32 ole32 advapi32 user32 gdi32)

# 5. Automated QA & Hardware Verification Suite (arz_qa_test.exe)
add_executable(arz_qa_test
    tests/aer_cli_test.cpp
)
target_include_directories(arz_qa_test PRIVATE src ${PREMIERE_INCLUDE_DIRS})
target_compile_definitions(arz_qa_test PRIVATE PRWIN_ENV=1 UNICODE _UNICODE)
target_link_libraries(arz_qa_test PRIVATE ARZCore)

file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")
add_custom_command(TARGET ARZCore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_RUNTIME_DLLS}
        "$<TARGET_FILE_DIR:ARZCore>"
    COMMENT "Staging FFmpeg 9.1+ runtime DLLs next to ARZCore.dll"
)
