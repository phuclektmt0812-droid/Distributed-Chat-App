cmake_minimum_required(VERSION 3.20)
project(DistributedChatApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${NLOHMANN_JSON_INCLUDE_DIR}")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

add_library(chat_protocol STATIC
    protocol/packet.cpp
    protocol/protocol.cpp
    protocol/message_payloads.cpp
    protocol/file_messages.cpp
    protocol/secure_payload.cpp
    platform/socket_compat.cpp
)
target_include_directories(chat_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chat_protocol PUBLIC OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(chat_protocol PUBLIC ws2_32)
endif()

add_executable(server_app
    server/main.cpp
    server/server.cpp
    server/user_manager.cpp
    server/tcp_session.cpp
    server/udp_service.cpp
    server/file_relay.cpp
)
target_link_libraries(server_app PRIVATE chat_protocol)

add_library(client_file_transfer STATIC
    clients/file_sender.cpp
    clients/file_receiver.cpp
)
target_include_directories(client_file_transfer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(client_file_transfer PUBLIC chat_protocol)

add_executable(client_agent
    clients/client_agent_main.cpp
    clients/dcap_client.cpp
    clients/dcap_http_bridge.cpp
)
target_include_directories(client_agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(client_agent PRIVATE client_file_transfer chat_protocol nlohmann_json::nlohmann_json)

# Shared current-protocol server/client implementation for integration tests.
add_library(dcap_test_support STATIC
    server/server.cpp
    server/user_manager.cpp
    server/tcp_session.cpp
    server/udp_service.cpp
    server/file_relay.cpp
    clients/dcap_client.cpp
)
target_include_directories(dcap_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dcap_test_support
    PUBLIC client_file_transfer chat_protocol nlohmann_json::nlohmann_json
)

enable_testing()

add_executable(dcap_protocol_tests tests/test_protocol.cpp)
target_link_libraries(dcap_protocol_tests PRIVATE chat_protocol)
add_test(NAME dcap_protocol_tests COMMAND dcap_protocol_tests)

add_executable(dcap_connection_tests tests/test_connection.cpp)
target_link_libraries(dcap_connection_tests PRIVATE dcap_test_support)
add_test(NAME dcap_connection_tests COMMAND dcap_connection_tests)

add_executable(dcap_login_tests tests/test_login.cpp)
target_link_libraries(dcap_login_tests PRIVATE dcap_test_support)
add_test(NAME dcap_login_tests COMMAND dcap_login_tests)

add_executable(dcap_chat_tests tests/test_chat.cpp)
target_link_libraries(dcap_chat_tests PRIVATE dcap_test_support)
add_test(NAME dcap_chat_tests COMMAND dcap_chat_tests)

add_executable(dcap_file_transfer_tests tests/test_file_transfer.cpp)
target_link_libraries(dcap_file_transfer_tests PRIVATE client_file_transfer)
add_test(NAME dcap_file_transfer_tests COMMAND dcap_file_transfer_tests)

add_executable(dcap_security_tests tests/security_tests.cpp)
target_link_libraries(dcap_security_tests PRIVATE chat_protocol)
add_test(NAME dcap_security_tests COMMAND dcap_security_tests)

set_tests_properties(
    dcap_connection_tests
    dcap_login_tests
    dcap_chat_tests
    PROPERTIES RUN_SERIAL TRUE
)
