# Red Dead Multiplayer (rdmp): a sandbox multiplayer mod for Red Dead Redemption (PC), built on the MafiaHub Framework.
#
#   build.bat                            configure + build everything into build\bin (Release)
#   cmake -S . -B build -G Ninja         by hand (an x64 MSVC prompt); the Framework SDK comes from FrameworkPin.cmake
#                                        and is cached in FW_SDK_CACHE_DIR (environment or -D)
#
# Targets (all land in build/bin):
#   RDMP.exe         launcher: starts the local server if asked, starts RDR.exe and loads rdmp.dll into it
#   rdmp.dll         the game side: boot, sandbox, natives, puppets, radar; loads rdmp_client.dll
#   rdmp_client.dll  the Framework client (networking, replication, client scripting) driven by rdmp.dll each frame
#   RDMPServer.exe   dedicated server (Framework server + server scripting)
#   rdmp_bot.exe     headless test client: joins a server as a fake player

cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/FrameworkBootstrap.cmake")

project(rdmp CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "rdmp builds for 64-bit Windows only (RDR.exe is x64): use an x64 MSVC toolchain (vcvars64).")
endif()

fw_bootstrap_finish()

set(RDMP_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

# --- game side (no Framework code: static CRT, so nothing else has to be next to RDR.exe) ---------------------------
file(GLOB RDMP_GAME_SOURCES CONFIGURE_DEPENDS "${RDMP_SRC}/game/*.cpp")
add_library(rdmp SHARED ${RDMP_GAME_SOURCES})
set_target_properties(rdmp PROPERTIES PREFIX "" MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_include_directories(rdmp PRIVATE "${RDMP_SRC}" "${RDMP_SRC}/game")
target_compile_definitions(rdmp PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(rdmp PRIVATE /EHsc /utf-8)
target_link_libraries(rdmp PRIVATE ws2_32)
# rdmp.map + rdmp.pdb resolve "rdmp.dll+offset" lines in crash logs.
target_link_options(rdmp PRIVATE /MAP /DEBUG /OPT:REF /OPT:ICF)

add_executable(RDMP "${RDMP_SRC}/launcher/launcher.cpp")
set_target_properties(RDMP PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_definitions(RDMP PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(RDMP PRIVATE /EHsc /utf-8)
target_link_libraries(RDMP PRIVATE comdlg32 shell32 user32)

# --- Framework side -------------------------------------------------------------------------------------------------
find_package(minhook CONFIG REQUIRED)
add_library(rdmp_client SHARED
    "${RDMP_SRC}/client/client.cpp"
    "${RDMP_SRC}/client/api_core.cpp"
    "${RDMP_SRC}/client/api_players.cpp"
    "${RDMP_SRC}/client/api_hud.cpp"
    "${RDMP_SRC}/client/api_input.cpp"
    "${RDMP_SRC}/client/api_natives.cpp"
    "${RDMP_SRC}/client/hud.cpp"
    "${RDMP_SRC}/client/frontend.cpp"
    "${RDMP_SRC}/client/server_query.cpp"
    "${RDMP_SRC}/client/overlay.cpp")
target_link_libraries(rdmp_client PRIVATE Framework::Client minhook::minhook d3d12 dxgi dinput8 dxguid)
target_include_directories(rdmp_client PRIVATE "${RDMP_SRC}")
target_compile_definitions(rdmp_client PRIVATE NOMINMAX _USE_MATH_DEFINES)
target_link_options(rdmp_client PRIVATE /MANIFEST:NO /DEBUG /OPT:REF /OPT:ICF)
fw_stage_runtime(rdmp_client CLIENT)

add_executable(RDMPServer
    "${RDMP_SRC}/server/main.cpp"
    "${RDMP_SRC}/server/state.cpp"
    "${RDMP_SRC}/server/api_players.cpp"
    "${RDMP_SRC}/server/api_elements.cpp"
    "${RDMP_SRC}/server/api_blips.cpp"
    "${RDMP_SRC}/server/api_hud.cpp")
target_link_libraries(RDMPServer PRIVATE Framework::Server)
target_include_directories(RDMPServer PRIVATE "${RDMP_SRC}")
target_compile_definitions(RDMPServer PRIVATE NOMINMAX _USE_MATH_DEFINES)
target_link_options(RDMPServer PRIVATE /DEBUG /OPT:REF /OPT:ICF)
fw_stage_runtime(RDMPServer SERVER)

add_executable(rdmp_bot "${RDMP_SRC}/bot/main.cpp")
target_link_libraries(rdmp_bot PRIVATE Framework::Client)
target_include_directories(rdmp_bot PRIVATE "${RDMP_SRC}")
target_compile_definitions(rdmp_bot PRIVATE NOMINMAX _USE_MATH_DEFINES)
fw_stage_runtime(rdmp_bot CLIENT)

# --- data ------------------------------------------------------------------------------------------------------------
# resources/ (scripts) is copied over build/bin/resources on every build, so the repository holds the only real copy.
# config/rdmp.ini is copied only when build/bin has none, so local edits to build/bin/rdmp.ini survive rebuilds.
add_custom_target(rdmp_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_LIST_DIR}/resources" "${CMAKE_BINARY_DIR}/bin/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_LIST_DIR}/ui" "${CMAKE_BINARY_DIR}/bin/ui"
    COMMAND ${CMAKE_COMMAND} "-DSRC=${CMAKE_CURRENT_LIST_DIR}/config/rdmp.ini" "-DDST=${CMAKE_BINARY_DIR}/bin/rdmp.ini"
            -P "${CMAKE_CURRENT_LIST_DIR}/cmake/CopyIfMissing.cmake"
    COMMENT "Staging resources and rdmp.ini"
    VERBATIM)
add_dependencies(RDMPServer rdmp_data)
add_dependencies(rdmp_client rdmp_data)
