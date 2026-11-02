CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -I.
LDLIBS ?= -lcrypto

CORE = protocol/packet.cpp protocol/protocol.cpp protocol/file_messages.cpp protocol/message_payloads.cpp protocol/secure_payload.cpp
FILES = client/file_sender.cpp client/file_receiver.cpp
SERVER = server/main.cpp server/server.cpp server/user_manager.cpp server/tcp_session.cpp server/udp_service.cpp server/file_relay.cpp platform/socket_compat.cpp
SERVER_LIB = server/server.cpp server/user_manager.cpp server/tcp_session.cpp server/udp_service.cpp server/file_relay.cpp platform/socket_compat.cpp

.PHONY: all test live-test clean
all: member3_tests server_app

member3_tests: $(CORE) $(FILES) tests/member3_tests.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(FILES) platform/socket_compat.cpp tests/member3_tests.cpp $(LDLIBS) -o $@

server_app: $(CORE) $(SERVER)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(SERVER) $(LDLIBS) -pthread -o $@

server_integration: $(CORE) $(FILES) $(SERVER_LIB) tests/server_integration.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(FILES) $(SERVER_LIB) tests/server_integration.cpp $(LDLIBS) -pthread -o $@

test: member3_tests
	./member3_tests

live-test: server_integration
	./server_integration

clean:
	$(RM) member3_tests server_app server_integration
