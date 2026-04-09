#include "../include/cppsocket.hpp"
#include <iostream>

int main() {
    
    /* connects to the server at 127.0.0.1:8080 (localhost) */
    std::string ipv4_address = "127.0.0.1";
    int port = 8080;
    auto server_socket = cppsocket::connect(ipv4_address, port);
    std::cout << "Connected to server at " << ipv4_address << ":" << port << "\n";

    /* send a friendly message to server */
    std::string msg = "Hello, Server!";
    std::vector<char> buffer(msg.begin(), msg.end());
    server_socket.send(buffer);

    std::cout << "Message sent to server: " << msg << "\n";

    return 0;
}