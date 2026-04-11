#ifndef _SOCKET_HPP
#define _SOCKET_HPP

#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
#include <unistd.h>

#if defined(__linux__)
#include <arpa/inet.h>

#else

#pragma comment(lib, "Ws2_32.lib")

#include <stdint.h>
#include <winsock2.h>
#include <sys/types.h>
#include <ws2tcpip.h>

static int socket_count = 0; 
static bool is_initialized = false;

void win_startup() {
    socket_count++;
    static WSADATA wsaData;
    
    if (is_initialized == true)
        return;
    
    is_initialized = true;
    
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR) {
        std::cout << "Error at WSAStartup()\n";
    }
}

void win_cleanup() {
    socket_count--;
    if (socket_count < 1) {
        is_initialized = false;
        WSACleanup();
    }
}

#endif

namespace cppsocket {

/* Encapsulates a file Descriptor related to the socket.
*  This class is designed to manage the lifecycle of a TCP socket file descriptor,
*  ensuring that it is properly closed when the Socket object goes out of scope.
*  It provides methods for sending and receiving data, and it is non-copyable to
*  prevent accidental copying of the socket resource.*/
class Socket {
public:
    /* Constructors */
    Socket(int file_descriptor): m_fd(file_descriptor) {
        #if defined(_WIN32) || defined(_WIN64)
        win_startup();
        #endif
    }
    Socket(Socket& other) noexcept = delete;
    Socket(Socket&& other) noexcept { 
        m_fd = other.m_fd; 
        other.m_fd = -1; 
        #if defined(_WIN32) || defined(_WIN64)
        win_startup();
        #endif
    }

    ~Socket() {
        if (m_fd != -1) {
            #if defined(_WIN32) || defined(_WIN64)
            closesocket(m_fd);
            #else
            close(m_fd);
            #endif
        }
        #if defined(_WIN32) || defined(_WIN64)
        win_cleanup();
        #endif
    }

    /* Recieve bytes from the other end of the connection.
    *  This function first reads the size of the incoming data,
    *  then reads the actual data content based on that size. This
    *  allows it to handle large files that may require multiple recv calls.
    *  @param buffer A reference to a vector that will be resized and filled with the received data.
    *  @throw std::runtime_error on error. */
    void recv(std::vector<char> &buffer) {
        
        if (!isValid()) throw std::runtime_error("Invalid socket");

        uint64_t len = 0;

        /* Recieve file size */
        #if defined(__linux__)
            ::recv(m_fd, &len, sizeof(len), 0);
        #else
            ::recv(m_fd, reinterpret_cast<char*>(&len), sizeof(len), 0);
        #endif

        buffer.resize(len);

        std::size_t total = 0;
        while (total < len) {
            ssize_t n = ::recv(m_fd, buffer.data() + total, buffer.size() - total, 0);
            
            if (n < 0)  throw std::runtime_error("recv failed"); 
            if (n == 0) throw std::runtime_error("Connection closed");

            total += static_cast<std::size_t>(n);
        }
    }

    /* send bytes to the other end of the connection
    *  Sends the size of the data first, followed by the actual data content. This
    *  allows the receiver to know how much data to expect and handle it accordingly,
    *  especially for large files that may require multiple recv calls.
    *  @param data The vector of bytes to send.
    *  @throw std::runtime_error on error. */
    void send(const std::vector<char> &data) {
        
        if (!isValid()) throw std::runtime_error("Invalid socket");

        /* Send data size */
        uint64_t len = data.size();
        #if defined(__linux__)
            ::send(m_fd, &len, sizeof(len), 0);
        #else    
            std::cout << ::send(m_fd, reinterpret_cast<char*>(&len), sizeof(len), 0) << "\n";
        #endif

        /* Send data content */
        std::size_t total = 0;
        while (total < len) {
            ssize_t n = ::send(m_fd, data.data() + total, data.size() - total, 0);
            
            if (n < 0)  throw std::runtime_error("send failed");
            if (n == 0) throw std::runtime_error("Connection closed");

            total += static_cast<std::size_t>(n);
        }
    }

    /* Checks if the socket is valid (i.e., has a valid file descriptor).
    *  @return true if the socket is valid, false otherwise. */
    bool isValid() const {
        return m_fd != -1;
    }

private:
    int m_fd;
};

/* Listens for connections and creates sockets.
*  Responsible for setting up a server socket that listens on a specified port, accepting
*  incoming client connections, and creating Socket objects for each accepted connection. */
class Acceptor {
public:
    /* Constructor */
    Acceptor(const uint16_t &port) {
        #if defined(_WIN32) || defined(_WIN64)
        win_startup();
        #endif

        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        
        if (fd_ == -1)
            throw std::runtime_error("socket() failed");

        #if defined(__linux__)
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        #else
         
        if (0 != ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt)))
            throw std::runtime_error("setsockopt() failed");
        #endif
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed");

        if (::listen(fd_, backlog) < 0)
            throw std::runtime_error("listen() failed");
    }
    
~Acceptor() {
        if (fd_ != -1) {
            #if defined(_WIN32) || defined(_WIN64)
            closesocket(fd_); // FIX: Use closesocket
            #else
            close(fd_);
            #endif
        }
        #if defined(_WIN32) || defined(_WIN64)
        win_cleanup(); // FIX: Was mistakenly win_startup()
        #endif
    }

    /* Accepts a connection from the backlog and creates a socket.
     * @throw std::runtime_error on error. */
    Socket accept() {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        int client_fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) throw std::runtime_error("accept() failed");

        char buf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, buf, sizeof(buf));

        return Socket(client_fd);
    }

private:
    int fd_ = -1;
    inline const static int opt = 1;
    inline const static int backlog = 10;
};

/* Connects to a given address and creates a socket for that connection.
 * @param host The server's IPV4 address.
 * @param port The port that the server is listening to.
 * @return Socket for the accepted connection.
 * @throw std::runtime_error on error. */
Socket connect(const std::string &host, const uint16_t &port) {
    #if defined(_WIN32) || defined(_WIN64)
    win_startup();
    #endif

    int fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    Socket socket(fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("invalid address: " + host);

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("connect() failed");
    
    return std::move(socket);
}

}

#endif
