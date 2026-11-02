cmake_minimum_required(VERSION 3.20)
project(DistributedChatApp LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The networking implementation currently requires Windows Winsock.")
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${NLOHMANN_JSON_INCLUDE_DIR}")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

add_library(chat_core STATIC
    clients/client.cpp
    clients/connection.cpp
    clients/file_transfer.cpp
    protocol/message.cpp
    protocol/udp_packet.cpp
    server/client_session.cpp
    server/message_router.cpp
    server/server.cpp
    server/user_manager.cpp
)
target_compile_features(chat_core PUBLIC cxx_std_17)
target_include_directories(chat_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chat_core PUBLIC nlohmann_json::nlohmann_json ws2_32)

enable_testing()

foreach(test_name IN ITEMS connection login chat file_transfer)
    add_executable(test_${test_name} tests/test_${test_name}.cpp)
    target_link_libraries(test_${test_name} PRIVATE chat_core)
    add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()
