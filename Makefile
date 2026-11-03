CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -I.
LDLIBS ?= -lcrypto
THREADS ?= -pthread

ifeq ($(OS),Windows_NT)
LDLIBS += -lws2_32
endif

CORE = protocol/packet.cpp protocol/protocol.cpp protocol/file_messages.cpp \
       protocol/message_payloads.cpp protocol/secure_payload.cpp \
       platform/socket_compat.cpp
SERVER = server/main.cpp server/server.cpp server/user_manager.cpp \
         server/tcp_session.cpp server/udp_service.cpp server/file_relay.cpp
CLIENT = clients/client_agent_main.cpp clients/client.cpp clients/http_bridge.cpp \
         clients/file_sender.cpp clients/file_receiver.cpp

.PHONY: all server clients clean
all: server clients

server: server_app

clients: client_agent

server_app: $(CORE) $(SERVER)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(SERVER) $(LDLIBS) $(THREADS) -o $@

client_agent: $(CORE) $(CLIENT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(CLIENT) $(LDLIBS) $(THREADS) -o $@

clean:
	$(RM) server_app client_agent
