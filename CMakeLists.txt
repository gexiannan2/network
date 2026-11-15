cmake_minimum_required(VERSION 3.16)
project(MemoryDealOriginalNetworkOnly LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Actual retained original networking classes. No shared-memory or order matching code.
add_library(memorydeal_network STATIC
    net_core.cpp
    MyTcpServer.cpp
    MyBaseServer.cpp
    MyServer.cpp
    MyServerTask.cpp
    base/MySocket.cpp
    base/MySockTask.cpp
    base/MySockTaskManager.cpp
    base/MySockTaskPool.cpp
    base/MyThread.cpp
)
target_include_directories(memorydeal_network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(memorydeal_network PUBLIC ws2_32 psapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(memorydeal_network PUBLIC Threads::Threads)
endif()

add_executable(server server.cpp)
target_link_libraries(server PRIVATE memorydeal_network)
set_target_properties(server PROPERTIES OUTPUT_NAME "net_server")

add_executable(client net_client.cpp)
target_link_libraries(client PRIVATE memorydeal_network)
set_target_properties(client PROPERTIES OUTPUT_NAME "net_client")

add_executable(protocol_test net_protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE memorydeal_network)
set_target_properties(protocol_test PROPERTIES OUTPUT_NAME "net_protocol_test")

add_executable(churn_test net_churn_test.cpp)
target_link_libraries(churn_test PRIVATE memorydeal_network)
set_target_properties(churn_test PROPERTIES OUTPUT_NAME "net_churn_test")

add_executable(load_test net_load_test.cpp)
target_link_libraries(load_test PRIVATE memorydeal_network)
set_target_properties(load_test PROPERTIES OUTPUT_NAME "net_load_test")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(tgt IN ITEMS memorydeal_network server client protocol_test churn_test load_test)
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()
