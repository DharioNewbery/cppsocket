#include "../include/cppsocket.hpp"
#include <iostream>

int main() {
    /* create an acceptor and accept a new connection */
    cppsocket::Acceptor acceptor(8080);

    std::cout << "Server is listening on port 8080...\n";
    auto client_socket = acceptor.accept(); // Waits for connection
    std::cout << "client accepted!\n";

    /* recieve message into buffer */
    std::vector<char> buffer;
    client_socket.recv(buffer);

    /* convert buffer to string and print it */
    std::string msg = std::string(buffer.begin(), buffer.end());
    std::cout << "Message from client: " << msg << "\n";

    return 0;
}
