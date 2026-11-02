#include "server.h"
#include "../platform/socket_compat.h"
#include <csignal>
#include <iostream>

int main() {
    if(!initializeSocketRuntime()) {
        std::cerr<<"Socket runtime initialization failed\n"; return 1;
    }
    Server server;
    if(!server.start()) { cleanupSocketRuntime(); return 1; }
    std::cout<<"Press ENTER to stop server..."<<std::endl;
    std::cin.get();
    server.stop();
    cleanupSocketRuntime();
    return 0;
}
