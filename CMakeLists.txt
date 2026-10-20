# Castlevania: Curse of Darkness (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(castlevania LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/(game|launcher)/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# A release (release.ps1) builds Castlevania.exe without the game code: the runtime, the host code and the exe's side
# of the split that split/glue.py wrote, the game code coming from CastlevaniaGame.dll, which the player's PC builds
# on the first start (src/common/setup.cpp). The generated folder is still read for image_symbols.h, the image's
# addresses, which the runtime is compiled with.
option(CV_SPLIT "Castlevania.exe without the game code, for a release" OFF)
if(CV_SPLIT)
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/generated/game)
    set(GLUE ${CMAKE_CURRENT_SOURCE_DIR}/out/split-glue)
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp)
    set(xbe_file ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe)
    file(SHA1 ${xbe_file} xbe_sha1)
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/Castlevania_xbe.cpp CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";
")
    add_executable(Castlevania WIN32 ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${GAME_SOURCES} ${XBE_IMGUI_SOURCES}
                   ${CMAKE_CURRENT_BINARY_DIR}/Castlevania_xbe.cpp ${GLUE}/host_glue.cpp)
    target_include_directories(Castlevania PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${XBE_SDK_DIR}/runtime ${GEN})
    target_include_directories(Castlevania SYSTEM PRIVATE ${imgui_SOURCE_DIR})
    target_compile_options(Castlevania PRIVATE -fno-strict-aliasing)
    target_compile_definitions(Castlevania PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS CV_SPLIT)
    target_link_libraries(Castlevania PRIVATE tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> d3d12 dxgi dxguid d3dcompiler xaudio2 ole32 windowscodecs dbghelp winmm dwmapi ws2_32)
    target_link_options(Castlevania PRIVATE -fuse-ld=lld -g)
else()
    # target name = GAME_EXE in build.bat, xbe = GAME_XBE
    add_xbe_image(Castlevania default.xbe game.toml ${GAME_PARTITIONS} game)
endif()

# the launcher beside it: game files check, settings, play; no game code, the xbe's SHA-1 add_xbe_image wrote
add_executable(CastlevaniaLauncher WIN32
    src/launcher/launcher.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/Castlevania_xbe.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(CastlevaniaLauncher SYSTEM PRIVATE ${imgui_SOURCE_DIR})
target_compile_definitions(CastlevaniaLauncher PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(CastlevaniaLauncher PRIVATE -Wno-nontrivial-memcall)  # Dear ImGui's own sources
target_link_libraries(CastlevaniaLauncher PRIVATE tomlplusplus::tomlplusplus d3d11 dxgi d3dcompiler bcrypt comdlg32 dwmapi shell32)
target_link_options(CastlevaniaLauncher PRIVATE -fuse-ld=lld)
add_dependencies(Castlevania CastlevaniaLauncher)
