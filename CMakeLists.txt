cmake_minimum_required(VERSION 3.20)
project(DistributedChatApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

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

# File-transfer endpoints are built as a reusable library. The legacy JSON
# client/HTTP bridge is intentionally not linked: it still speaks the old
# length-prefixed JSON protocol and must be ported to DCAP before use.
add_library(client_file_transfer STATIC
    clients/file_sender.cpp
    clients/file_receiver.cpp
)
target_link_libraries(client_file_transfer PUBLIC chat_protocol)
target_include_directories(client_file_transfer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
# The previous tests target the retired JSON protocol. Add DCAP protocol,
# session, and file-transfer tests before registering CTest targets.
