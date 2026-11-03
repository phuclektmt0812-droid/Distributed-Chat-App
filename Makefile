# Distributed Chat App - portable GNU Make build
# Requires GNU Make, a C++17 compiler, and OpenSSL development files.

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -I.
LDLIBS ?= -lcrypto
THREADS ?= -pthread

ifeq ($(OS),Windows_NT)
  EXEEXT ?= .exe
  LDLIBS += -lws2_32
  # GnuWin32 Make on Windows uses cmd.exe for built-in commands.
  SHELL := cmd.exe
  .SHELLFLAGS := /C
  MKDIR_BUILD = if not exist "$(TEST_BUILD_DIR)" mkdir "$(TEST_BUILD_DIR)"
  RUN_TESTS = powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-tests.ps1 -TestDirectory "$(TEST_BUILD_DIR)" -Tests "$(TESTS)"
  RUN_ONE = powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-one-test.ps1 -Executable "$(TEST_BUILD_DIR)/$*$(EXEEXT)"
  CLEAN_BUILD = if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"
  CLEAN_APPS = if exist "server_app$(EXEEXT)" del /Q "server_app$(EXEEXT)" & if exist "client_agent$(EXEEXT)" del /Q "client_agent$(EXEEXT)"
else
  EXEEXT ?=
  SHELL := /bin/sh
  .SHELLFLAGS := -c
  MKDIR_BUILD = mkdir -p "$(TEST_BUILD_DIR)"
  RUN_TESTS = set -e; for name in $(TESTS); do echo ""; echo "========== $$name =========="; "./$(TEST_BUILD_DIR)/$$name$(EXEEXT)"; done; echo ""; echo "All $(words $(TESTS)) test programs passed."
  RUN_ONE = "./$<"
  CLEAN_BUILD = rm -rf "$(BUILD_DIR)"
  CLEAN_APPS = rm -f server_app$(EXEEXT) client_agent$(EXEEXT)
endif

BUILD_DIR ?= make-build
TEST_BUILD_DIR := $(BUILD_DIR)/tests

CORE = protocol/packet.cpp protocol/protocol.cpp protocol/file_messages.cpp \
       protocol/message_payloads.cpp protocol/secure_payload.cpp \
       platform/socket_compat.cpp

SERVER = server/main.cpp server/server.cpp server/user_manager.cpp \
         server/tcp_session.cpp server/udp_service.cpp server/file_relay.cpp

SERVER_SUPPORT = server/server.cpp server/user_manager.cpp server/tcp_session.cpp \
                 server/udp_service.cpp server/file_relay.cpp

FILE_TRANSFER = clients/file_sender.cpp clients/file_receiver.cpp
CLIENT = clients/client_agent_main.cpp clients/client.cpp clients/http_bridge.cpp \
         $(FILE_TRANSFER)
CLIENT_SUPPORT = clients/client.cpp clients/http_bridge.cpp

DCAP_TEST_SUPPORT = $(CORE) $(SERVER_SUPPORT) $(CLIENT_SUPPORT) $(FILE_TRANSFER)

TESTS = dcap_protocol_tests \
        dcap_connection_tests \
        dcap_login_tests \
        dcap_chat_tests \
        dcap_file_transfer_tests \
        dcap_security_tests \
        dcap_protocol_edge_tests \
        dcap_transport_tests \
        dcap_message_codec_tests \
        dcap_file_message_codec_tests \
        dcap_user_manager_tests \
        dcap_socket_compat_tests \
        dcap_udp_tests \
        dcap_handshake_tests \
        dcap_tcp_control_tests \
        dcap_client_validation_tests \
        dcap_concurrency_tests \
        dcap_file_transfer_edge_tests \
        dcap_file_transfer_integration_tests \
        dcap_file_relay_tests \
        dcap_security_edge_tests \
        dcap_http_bridge_tests \
        dcap_web_asset_tests

.PHONY: all app server clients tests test test-one check clean list-tests $(TESTS) \
        $(addprefix test-,$(TESTS))

all: app tests

app: server clients

server: server_app$(EXEEXT)
clients: client_agent$(EXEEXT)

server_app$(EXEEXT): $(CORE) $(SERVER)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(SERVER) $(LDLIBS) $(THREADS) -o $@

client_agent$(EXEEXT): $(CORE) $(CLIENT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(CLIENT) $(LDLIBS) $(THREADS) -o $@

define TEST_template
$(TEST_BUILD_DIR)/$(1)$(EXEEXT): $(2) $(3)
	@$(MKDIR_BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(2) $(3) $(LDLIBS) $(THREADS) -o $$@

.PHONY: $(1)
$(1): $(TEST_BUILD_DIR)/$(1)$(EXEEXT)
endef

$(eval $(call TEST_template,dcap_protocol_tests,tests/test_protocol.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_connection_tests,tests/test_connection.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_login_tests,tests/test_login.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_chat_tests,tests/test_chat.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_file_transfer_tests,tests/test_file_transfer.cpp $(FILE_TRANSFER),$(CORE)))
$(eval $(call TEST_template,dcap_security_tests,tests/security_tests.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_protocol_edge_tests,tests/test_protocol_edges.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_transport_tests,tests/test_transport.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_message_codec_tests,tests/test_message_codecs.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_file_message_codec_tests,tests/test_file_message_codecs.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_user_manager_tests,tests/test_user_manager.cpp server/user_manager.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_socket_compat_tests,tests/test_socket_compat.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_udp_tests,tests/test_udp.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_handshake_tests,tests/test_handshake.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_tcp_control_tests,tests/test_tcp_control.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_client_validation_tests,tests/test_client_validation.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_concurrency_tests,tests/test_concurrency.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_file_transfer_edge_tests,tests/test_file_transfer_edges.cpp $(FILE_TRANSFER),$(CORE)))
$(eval $(call TEST_template,dcap_file_transfer_integration_tests,tests/test_file_transfer_integration.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_file_relay_tests,tests/test_file_relay.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_security_edge_tests,tests/test_security_edges.cpp,$(CORE)))
$(eval $(call TEST_template,dcap_http_bridge_tests,tests/test_http_bridge.cpp,$(DCAP_TEST_SUPPORT)))
$(eval $(call TEST_template,dcap_web_asset_tests,tests/test_web_assets.cpp,))

tests: $(addprefix $(TEST_BUILD_DIR)/,$(addsuffix $(EXEEXT),$(TESTS)))

test: tests
ifeq ($(OS),Windows_NT)
	@$(RUN_TESTS)
else
	@$(RUN_TESTS)
endif

check: test

.PHONY: run-%
run-%: $(TEST_BUILD_DIR)/%$(EXEEXT)
ifeq ($(OS),Windows_NT)
	@$(RUN_ONE)
else
	@$(RUN_ONE)
endif

$(foreach name,$(TESTS),$(eval test-$(name): run-$(name)))

test-one: run-$(TEST)

list-tests:
	@echo $(TESTS)

clean:
	@$(CLEAN_BUILD)
	@$(CLEAN_APPS)
