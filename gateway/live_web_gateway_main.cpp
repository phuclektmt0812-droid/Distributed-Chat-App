#include "live_web_gateway.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::uint16_t parsePort(const char* value, std::uint16_t fallback) {
    if (!value) return fallback;
    const auto parsed = std::stoul(value);
    if (parsed == 0 || parsed > 65535)
        throw std::invalid_argument("Port must be between 1 and 65535");
    return static_cast<std::uint16_t>(parsed);
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 5) {
            std::cerr << "Usage: live_web_gateway [server-ip] [server-tcp-port] "
                         "[http-port] [web-root]\n";
            return 2;
        }

        const std::string serverAddress = argc > 1 ? argv[1] : "127.0.0.1";
        const auto serverPort = parsePort(argc > 2 ? argv[2] : nullptr, 54000);
        const auto httpPort = parsePort(argc > 3 ? argv[3] : nullptr, 8081);
        const std::string webRoot = argc > 4 ? argv[4] : "web";

        LiveWebGateway gateway(serverAddress, serverPort, httpPort, webRoot);
        if (!gateway.start()) {
            std::cerr << "Could not start the public web gateway. "
                         "Check the port, web assets, and server configuration.\n";
            return 1;
        }

        std::cout << "Open the gateway URL in a browser. Press ENTER to stop.\n";
        std::cin.get();
        gateway.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Gateway startup failed: " << error.what() << '\n';
        return 2;
    }
}
