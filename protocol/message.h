#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

enum class MessageType {
    LOGIN,
    LOGIN_OK,
    LOGIN_FAIL,
    LOGOUT,
    USER_LIST,
    CHAT_ALL,
    CHAT_PRIVATE,
    CHAT_DELIVER,
    CHAT_ERROR,
    PING,
    PONG,
    FILE_START,
    FILE_ACCEPT,
    FILE_REJECT,
    FILE_END,
    FILE_COMPLETE,
    ERROR
};

struct Message {
    MessageType type;
    nlohmann::json payload;
};
std::string serializeMessage(const Message& message);

bool deserializeMessage(
    const std::string& data,
    Message& message
);