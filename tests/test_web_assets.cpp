#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
std::string read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "missing web asset: " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void testHtmlContract(const std::string& html) {
    const std::vector<std::string> ids = {
        "connection-status", "login-section", "chat-section", "login-form",
        "bridge-url", "username", "logout-button", "user-list",
        "global-conversation", "conversation-title", "messages", "message-form",
        "message-input", "upload-form", "file-input", "transfer-list"
    };
    for (const auto& id : ids)
        require(html.find("id=\"" + id + "\"") != std::string::npos,
                "HTML is missing required element id: " + id);
    require(html.find("app.js") != std::string::npos, "HTML does not load app.js");
    require(html.find("style.css") != std::string::npos, "HTML does not load style.css");
}
void testJavascriptContract(const std::string& js) {
    const std::vector<std::string> endpoints = {
        "/status", "/login", "/logout", "/global_message", "/private_message",
        "/user_list", "/events", "/file_decision", "/file_upload"
    };
    for (const auto& endpoint : endpoints)
        require(js.find(endpoint) != std::string::npos,
                "web client does not reference bridge endpoint: " + endpoint);
    require(js.find("DOMContentLoaded") != std::string::npos,
            "web client does not wait for DOM initialization");
    require(js.find("textContent") != std::string::npos,
            "web client should render untrusted text through textContent");
}
}
int main() {
    try {
        const auto html = read("web/index.html");
        const auto js = read("web/app.js");
        const auto css = read("web/style.css");
        testHtmlContract(html);
        testJavascriptContract(js);
        require(!css.empty(), "stylesheet is empty");
        std::cout << "Web asset contract tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Web asset tests failed: " << e.what() << '\n';
        return 1;
    }
}