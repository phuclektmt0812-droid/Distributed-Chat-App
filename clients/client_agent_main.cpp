#include "http_bridge.h
#include <iostream>
#include <string>

int main(int argc,char** argv) {
    const std::string serverAddress = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t serverPort = argc > 2
        ? static_cast<std::uint16_t>(std::stoi(argv[2])) : 54000;
    const std::uint16_t httpPort = argc > 3
        ? static_cast<std::uint16_t>(std::stoi(argv[3])) : 8080;

    DcapHttpBridge bridge(serverAddress,serverPort,httpPort);
    if(!bridge.start()) {
        std::cerr << "Could not start local HTTP bridge on 127.0.0.1:" << httpPort << "\n";
        return 1;
    }
    std::cout << "Client Agent ready. Server " << serverAddress << ":" << serverPort
              << ", local UI bridge http://127.0.0.1:" << httpPort << "\n";
    std::cout << "Open http://127.0.0.1:" << httpPort << "/ in a browser and press ENTER here to stop.\n";
    std::cin.get();
    bridge.stop();
    return 0;
}
