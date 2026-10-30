cmake_minimum_required(VERSION 3.28)
project(dds-viewer VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "dds-viewer builds a Windows WIC codec and needs Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into Explorer, so it must not need a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# All binaries land in one folder next to each other.
foreach(kind RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${kind}_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG)
        set(CMAKE_${kind}_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
    endforeach()
endforeach()

# DirectXTex (Microsoft, MIT): the same DDS decoding as texconv. Pinned to the may2026 release.
include(FetchContent)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_DX11 OFF CACHE BOOL "" FORCE)
set(BUILD_DX12 OFF CACHE BOOL "" FORCE)
set(BC_USE_OPENMP OFF CACHE BOOL "" FORCE)  # no vcomp140.dll dependency inside Explorer
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(DirectXTex
    GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
    GIT_TAG 4feb3e11a020f35b796fc769a74216a555d4f5ef  # may2026
    GIT_SHALLOW FALSE
    EXCLUDE_FROM_ALL)  # keeps its headers and .lib out of the release package
FetchContent_MakeAvailable(DirectXTex)

# Windows Imaging Component codec: Explorer thumbnails, preview pane, photo viewers.
add_library(DdsWic SHARED src/wic/ddswic.cpp src/wic/ddswic.def)
target_link_libraries(DdsWic PRIVATE DirectXTex windowscodecs ole32 advapi32 shell32)
target_compile_definitions(DdsWic PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(DdsWic PRIVATE /W4 /utf-8)
endif()

# Test tool that decodes through WIC like Explorer does.
add_executable(wictest tools/wictest.cpp)
target_link_libraries(wictest PRIVATE windowscodecs ole32)
target_compile_definitions(wictest PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(wictest PRIVATE /W4 /utf-8)
endif()

# Release package (cmake --install build --prefix dist).
install(TARGETS DdsWic wictest RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES scripts/install.bat scripts/uninstall.bat scripts/clear-thumbnails.bat README.md LICENSE
        DESTINATION .)
# DirectXTex is linked statically: its MIT notice ships with the binaries.
install(FILES ${directxtex_SOURCE_DIR}/LICENSE DESTINATION . RENAME DirectXTex-LICENSE.txt)
