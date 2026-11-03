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
    clients/client.cpp
    clients/http_bridge.cpp
)
target_include_directories(client_agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(client_agent PRIVATE client_file_transfer chat_protocol nlohmann_json::nlohmann_json)

add_executable(live_web_gateway
    gateway/live_web_gateway_main.cpp
    gateway/live_web_gateway.cpp
    clients/client.cpp
)
target_include_directories(live_web_gateway PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(live_web_gateway PRIVATE client_file_transfer chat_protocol nlohmann_json::nlohmann_json)

# Shared current-protocol server/client/HTTP implementation for integration tests.
add_library(dcap_test_support STATIC
    server/server.cpp
    server/user_manager.cpp
    server/tcp_session.cpp
    server/udp_service.cpp
    server/file_relay.cpp
    clients/client.cpp
    clients/http_bridge.cpp
)
target_include_directories(dcap_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dcap_test_support
    PUBLIC client_file_transfer chat_protocol nlohmann_json::nlohmann_json
)

enable_testing()

# Existing current-protocol baseline tests.
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

# Packet, payload, and file-message codecs.
add_executable(dcap_protocol_edge_tests tests/test_protocol_edges.cpp)
target_link_libraries(dcap_protocol_edge_tests PRIVATE chat_protocol)
add_test(NAME dcap_protocol_edge_tests COMMAND dcap_protocol_edge_tests)

add_executable(dcap_transport_tests tests/test_transport.cpp)
target_link_libraries(dcap_transport_tests PRIVATE chat_protocol)
add_test(NAME dcap_transport_tests COMMAND dcap_transport_tests)

add_executable(dcap_message_codec_tests tests/test_message_codecs.cpp)
target_link_libraries(dcap_message_codec_tests PRIVATE chat_protocol)
add_test(NAME dcap_message_codec_tests COMMAND dcap_message_codec_tests)

add_executable(dcap_file_message_codec_tests tests/test_file_message_codecs.cpp)
target_link_libraries(dcap_file_message_codec_tests PRIVATE chat_protocol)
add_test(NAME dcap_file_message_codec_tests COMMAND dcap_file_message_codec_tests)

# Socket, user/session, and client/server integration.
add_executable(dcap_user_manager_tests
    tests/test_user_manager.cpp
    server/user_manager.cpp
)
target_link_libraries(dcap_user_manager_tests PRIVATE chat_protocol Threads::Threads)
add_test(NAME dcap_user_manager_tests COMMAND dcap_user_manager_tests)

add_executable(dcap_socket_compat_tests tests/test_socket_compat.cpp)
target_link_libraries(dcap_socket_compat_tests PRIVATE chat_protocol)
add_test(NAME dcap_socket_compat_tests COMMAND dcap_socket_compat_tests)

add_executable(dcap_udp_tests tests/test_udp.cpp)
target_link_libraries(dcap_udp_tests PRIVATE dcap_test_support)
add_test(NAME dcap_udp_tests COMMAND dcap_udp_tests)

add_executable(dcap_handshake_tests tests/test_handshake.cpp)
target_link_libraries(dcap_handshake_tests PRIVATE dcap_test_support)
add_test(NAME dcap_handshake_tests COMMAND dcap_handshake_tests)

add_executable(dcap_tcp_control_tests tests/test_tcp_control.cpp)
target_link_libraries(dcap_tcp_control_tests PRIVATE dcap_test_support)
add_test(NAME dcap_tcp_control_tests COMMAND dcap_tcp_control_tests)

add_executable(dcap_client_validation_tests tests/test_client_validation.cpp)
target_link_libraries(dcap_client_validation_tests PRIVATE dcap_test_support)
add_test(NAME dcap_client_validation_tests COMMAND dcap_client_validation_tests)

add_executable(dcap_concurrency_tests tests/test_concurrency.cpp)
target_link_libraries(dcap_concurrency_tests PRIVATE dcap_test_support)
add_test(NAME dcap_concurrency_tests COMMAND dcap_concurrency_tests)

# File sender/receiver state machine and real server UDP-relay flows.
add_executable(dcap_file_transfer_edge_tests tests/test_file_transfer_edges.cpp)
target_link_libraries(dcap_file_transfer_edge_tests PRIVATE client_file_transfer)
add_test(NAME dcap_file_transfer_edge_tests COMMAND dcap_file_transfer_edge_tests)

add_executable(dcap_file_transfer_integration_tests tests/test_file_transfer_integration.cpp)
target_link_libraries(dcap_file_transfer_integration_tests PRIVATE dcap_test_support)
add_test(NAME dcap_file_transfer_integration_tests COMMAND dcap_file_transfer_integration_tests)

add_executable(dcap_file_relay_tests tests/test_file_relay.cpp)
target_link_libraries(dcap_file_relay_tests PRIVATE dcap_test_support)
add_test(NAME dcap_file_relay_tests COMMAND dcap_file_relay_tests)

# Encryption/key-file edge cases and HTTP/web integration.
add_executable(dcap_security_edge_tests tests/test_security_edges.cpp)
target_link_libraries(dcap_security_edge_tests PRIVATE chat_protocol)
add_test(NAME dcap_security_edge_tests COMMAND dcap_security_edge_tests)

add_executable(dcap_http_bridge_tests tests/test_http_bridge.cpp)
target_link_libraries(dcap_http_bridge_tests PRIVATE dcap_test_support)
add_test(NAME dcap_http_bridge_tests COMMAND dcap_http_bridge_tests)

add_executable(dcap_live_web_gateway_tests
    tests/test_live_web_gateway.cpp
    gateway/live_web_gateway.cpp
)
target_link_libraries(dcap_live_web_gateway_tests PRIVATE dcap_test_support)
add_test(NAME dcap_live_web_gateway_tests COMMAND dcap_live_web_gateway_tests)

add_executable(dcap_web_asset_tests tests/test_web_assets.cpp)
add_test(NAME dcap_web_asset_tests COMMAND dcap_web_asset_tests)

set_tests_properties(
    dcap_web_asset_tests
    dcap_http_bridge_tests
    dcap_live_web_gateway_tests
    PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# All tests that bind the fixed DCAP TCP/UDP ports must run one at a time.
set_tests_properties(
    dcap_connection_tests
    dcap_login_tests
    dcap_chat_tests
    dcap_udp_tests
    dcap_handshake_tests
    dcap_tcp_control_tests
    dcap_client_validation_tests
    dcap_concurrency_tests
    dcap_file_transfer_integration_tests
    dcap_file_relay_tests
    dcap_http_bridge_tests
    dcap_live_web_gateway_tests
    PROPERTIES RUN_SERIAL TRUE
)
