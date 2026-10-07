cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(SaturnXbandServerModem VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
add_subdirectory(components/xband)
if(WIN32)
    find_package(nlohmann_json 3.12.0 QUIET CONFIG)
    if(NOT nlohmann_json_FOUND)
        include(FetchContent)
        FetchContent_Declare(json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
        FetchContent_MakeAvailable(json)
    endif()
    add_executable(xband-server outputs/live-capture-tests/pb3_service_server.cpp)
    target_compile_features(xband-server PRIVATE cxx_std_20)
    target_compile_definitions(xband-server PRIVATE
        PB3_SERVER_WINDOW=1 PB3_PAIR_CONTROL=1 PB3_SERVER_LOGIN=1 XBAND_FRONTEND_SERVER=1
        PB3_DATE_FIXTURE_TEST=1 PB3_LEVEL_FIXTURE_TEST=1 PB3_GAME_RANKING_SETTINGS=1
        PB3_USAGE_AREA_SETTINGS=1 PB3_REGION_TABLE=1 PB3_REGION_TOWN=1
        PB3_VF_POINT_LEDGER=1 PB3_STANDBY_DIALOG=1 PB3_INTRO_COMPACT_STYLE=1
        PB3_LOCAL_MAIL_JOURNAL=1 PB3_SERVER_POINT_LEDGER=1 PB3_USAGE_TIME_POLICY=1)
    target_compile_options(xband-server PRIVATE /utf-8)
    target_include_directories(xband-server PRIVATE outputs/live-capture-tests)
    target_link_libraries(xband-server PRIVATE xband::components nlohmann_json::nlohmann_json
        ws2_32 bcrypt iphlpapi user32 gdi32 gdiplus comctl32)
endif()
