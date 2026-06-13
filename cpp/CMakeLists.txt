cmake_minimum_required(VERSION 3.15)
project(SmartTrafficController LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(smart_traffic_controller
    main.cpp
    Junction.cpp
    RLAgent.cpp
    ConflictConfig.cpp
    NeighborCoordConfig.cpp
    PhaseConfig.cpp
    SelfTests.cpp
    ThresholdConfig.cpp
    Simulation.cpp
)

# WinHTTP for HTTP client on Windows
if(WIN32)
    target_link_libraries(smart_traffic_controller PRIVATE winhttp bcrypt)
endif()

# ----------------------------------------------------------------------------
# traffic_server — C++ replacement for python/server/app.py.
# Owns public port 8000 (Router) and serves HTTP + WebSocket on the same port.
# ----------------------------------------------------------------------------
include(FetchContent)

# cpp-httplib (header-only). Reuses cached source under build/_deps if present.
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
)
FetchContent_MakeAvailable(httplib)

# nlohmann/json (header-only)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(traffic_server
    server/server_main.cpp
    server/TrafficServer.cpp
    server/WebSocketHub.cpp
    server/Router.cpp
    server/Database.cpp
    server/JwtAuth.cpp
)

target_include_directories(traffic_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${httplib_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
)

target_compile_definitions(traffic_server PRIVATE
    CPPHTTPLIB_NO_EXCEPTIONS=1
    _WIN32_WINNT=0x0601
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(WIN32)
    target_link_libraries(traffic_server PRIVATE
        ws2_32 odbc32 advapi32 bcrypt crypt32
    )
endif()
