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
