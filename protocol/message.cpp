#include "../protocol/message.h"

#include <string>

namespace {

std::string messageTypeToString(MessageType type) {
    switch (type) {
        case MessageType::LOGIN:
            return "LOGIN";
        case MessageType::LOGIN_OK:
            return "LOGIN_OK";
        case MessageType::LOGIN_FAIL:
            return "LOGIN_FAIL";
        case MessageType::LOGOUT:
            return "LOGOUT";
        case MessageType::USER_LIST:
            return "USER_LIST";
        case MessageType::CHAT_ALL:
            return "CHAT_ALL";
        case MessageType::CHAT_PRIVATE:
            return "CHAT_PRIVATE";
        case MessageType::CHAT_DELIVER:
            return "CHAT_DELIVER";
        case MessageType::CHAT_ERROR:
            return "CHAT_ERROR";
        case MessageType::PING:
            return "PING";
        case MessageType::PONG:
            return "PONG";
        case MessageType::FILE_START:
            return "FILE_START";
        case MessageType::FILE_ACCEPT:
            return "FILE_ACCEPT";
        case MessageType::FILE_REJECT:
            return "FILE_REJECT";
        case MessageType::FILE_END:
            return "FILE_END";
        case MessageType::FILE_COMPLETE:
            return "FILE_COMPLETE";
        case MessageType::ERROR:
            return "ERROR";
    }

    return "ERROR";
}

bool stringToMessageType(
    const std::string& value,
    MessageType& type
) {
    if (value == "LOGIN") {
        type = MessageType::LOGIN;
    } else if (value == "LOGIN_OK") {
        type = MessageType::LOGIN_OK;
    } else if (value == "LOGIN_FAIL") {
        type = MessageType::LOGIN_FAIL;
    } else if (value == "LOGOUT") {
        type = MessageType::LOGOUT;
    } else if (value == "USER_LIST") {
        type = MessageType::USER_LIST;
    } else if (value == "CHAT_ALL") {
        type = MessageType::CHAT_ALL;
    } else if (value == "CHAT_PRIVATE") {
        type = MessageType::CHAT_PRIVATE;
    } else if (value == "CHAT_DELIVER") {
        type = MessageType::CHAT_DELIVER;
    } else if (value == "CHAT_ERROR") {
        type = MessageType::CHAT_ERROR;
    } else if (value == "PING") {
        type = MessageType::PING;
    } else if (value == "PONG") {
        type = MessageType::PONG;
    } else if (value == "FILE_START") {
        type = MessageType::FILE_START;
    } else if (value == "FILE_ACCEPT") {
        type = MessageType::FILE_ACCEPT;
    } else if (value == "FILE_REJECT") {
        type = MessageType::FILE_REJECT;
    } else if (value == "FILE_END") {
        type = MessageType::FILE_END;
    } else if (value == "FILE_COMPLETE") {
        type = MessageType::FILE_COMPLETE;
    } else if (value == "ERROR") {
        type = MessageType::ERROR;
    } else {
        return false;
    }

    return true;
}

} // namespace

std::string serializeMessage(const Message& message) {
    nlohmann::json serialized = nlohmann::json::object();
    serialized["type"] = messageTypeToString(message.type);
    serialized["payload"] = message.payload;
    return serialized.dump();
}

bool deserializeMessage(
    const std::string& data,
    Message& message
) {
    try {
        const nlohmann::json parsed = nlohmann::json::parse(data);
        if (!parsed.is_object() ||
            !parsed.contains("type") ||
            !parsed.contains("payload") ||
            !parsed["type"].is_string() ||
            !parsed["payload"].is_object()) {
            return false;
        }

        MessageType parsedType;
        if (!stringToMessageType(
                parsed["type"].get<std::string>(),
                parsedType
            )) {
            return false;
        }

        message.type = parsedType;
        message.payload = parsed["payload"];
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}
