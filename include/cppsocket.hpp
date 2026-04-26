#ifndef _SOCKETIPV6_HPP
#define _SOCKETIPV6_HPP

#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
#include <unistd.h>
#include <map>
#include <algorithm>
#include <optional>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#else

#pragma comment(lib, "Ws2_32.lib")
#include <stdint.h>
#include <winsock2.h>
#include <sys/types.h>
#include <ws2tcpip.h>
#define poll WSAPoll
#include <mutex>
#include <atomic>

static std::atomic<int> socket_count = 0;
static std::atomic<bool> is_initialized = false;
static std::mutex wsa_mutex;

void win_startup() {
    socket_count++;
    
    std::lock_guard<std::mutex> lock(wsa_mutex);
    if (is_initialized) return;
    
    is_initialized = true;
    static WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR) {
        is_initialized = false;
        throw std::runtime_error("WSAStartup() failed");
    }
}

void win_cleanup() {
    std::lock_guard<std::mutex> lock(wsa_mutex);
    if (--socket_count < 1 && is_initialized) {
        is_initialized = false;
        WSACleanup();
    }
}
#endif

namespace {

/* Manual implementation of serialization of uint64 to big-endian bytes */
inline uint64_t hostToNet64(uint64_t value) {
    uint64_t result = 0;
    for (int i = 7; i >= 0; --i) {
        reinterpret_cast<uint8_t*>(&result)[i] = value & 0xFF;
        value >>= 8;
    }
    return result;
}

inline uint64_t netToHost64(uint64_t value) {
    return hostToNet64(value);
}

};

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
        if (m_fd != -1)
        win_startup();
        #endif
    }
    Socket(Socket& other) noexcept = delete;
    Socket(Socket&& other) noexcept {
        m_fd = other.m_fd; 
        other.m_fd = -1;
    }

    ~Socket() {
        if (m_fd == -1) return;

        #if defined(_WIN32) || defined(_WIN64)
        closesocket(m_fd);        
        win_cleanup();
        #else
        close(m_fd);
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

        uint64_t lenNet = 0;
        
        std::size_t headerTotal = 0;
        while (headerTotal < sizeof(lenNet)) {
            ssize_t n = ::recv(m_fd, reinterpret_cast<char*>(&lenNet) + headerTotal, sizeof(lenNet) - headerTotal, 0);

            if (n == 0) throw std::runtime_error("Connection closed reading header");
            if (n < 0)  throw std::runtime_error("recv failed reading header");

            headerTotal += static_cast<std::size_t>(n);
        }

        uint64_t len = netToHost64(lenNet);
        buffer.resize(len);

        std::size_t payloadTotal = 0;
        while (payloadTotal < len) {
            ssize_t n = ::recv(m_fd, buffer.data() + payloadTotal, buffer.size() - payloadTotal, 0);
            
            if (n < 0)  throw std::runtime_error("recv failed reading payload"); 
            if (n == 0) throw std::runtime_error("Connection closed reading payload");

            payloadTotal += static_cast<std::size_t>(n);
        }
    }
    
    /* String overload of recv */
    void recv(std::string &buffer) {
        std::vector<char> temp;
        recv(temp);
        buffer = std::string(temp.begin(), temp.end());
    }

    /* send bytes to the other end of the connection
    *  Sends the size of the data first, followed by the actual data content. This
    *  allows the receiver to know how much data to expect and handle it accordingly,
    *  especially for large files that may require multiple recv calls.
    *  @param data The vector of bytes to send.
    *  @throw std::runtime_error on error. */
    void send(const std::vector<char> &data) {
        
        if (!isValid()) throw std::runtime_error("Invalid socket");

        uint64_t len = data.size();
        uint64_t lenNet = hostToNet64(len);

        /* Send data size */
        std::size_t headerTotal = 0;
        while (headerTotal < sizeof(uint64_t)) {
            ssize_t n = ::send(m_fd, reinterpret_cast<char*>(&lenNet) + headerTotal, sizeof(lenNet) - headerTotal, 0);

            if (n < 0)  throw std::runtime_error("send failed sending header");
            if (n == 0) throw std::runtime_error("Connection closed sending header");

            headerTotal += static_cast<std::size_t>(n);
        }
        
        /* Send data content */
        std::size_t payloadTotal = 0;
        while (payloadTotal < len) {
            ssize_t n = ::send(m_fd, data.data() + payloadTotal, data.size() - payloadTotal, 0);
            
            if (n < 0)  throw std::runtime_error("send failed sending payload");
            if (n == 0) throw std::runtime_error("Connection closed sending payload");

            payloadTotal += static_cast<std::size_t>(n);
        }
    }

    /* String overload of send */
    void send(const std::string &data) {
        std::vector<char> temp(data.begin(), data.end());
        send(temp);
    }

    /* Checks if the socket is valid (i.e., has a valid file descriptor).
    *  @return true if the socket is valid, false otherwise. */
    bool isValid() const {
        return m_fd != -1;
    }

    const int getFd() const { return m_fd; }

private:
    int m_fd;
};

/* Listens for connections and creates sockets.
*  Responsible for setting up a server socket that listens on a specified port, accepting
*  incoming client connections, and creating Socket objects for each accepted connection. */
class Acceptor {
private:
    /* Wrapper to sock options.
     * @returns 0 if all is ok. */
    bool setOptions(bool isIpv4) {
        int res[3];
        #if defined(__linux__)
        if (isIpv4) res[0] = 0;
        else res[0] = ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        res[1] = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
        res[2] = ::fcntl(fd, F_SETFL, O_NONBLOCK); 
        #else
        if (isIpv4) res[0] = 0;
        else res[0] = ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &v6only, sizeof(v6only));
        res[1] = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &reuseaddr, sizeof(reuseaddr));
        res[2] = ::ioctlsocket(fd, FIONBIO, (u_long*) &reuseaddr);
        #endif

        for (int i = 0; i < 3; i++)
        if (res[i] != 0) return false; // Error

        return true; // Success
    }

    /* Associates a local address with a socket.
     * @param port the port that will be used.
     * @returns 0 if all is ok. */
    int bind(int port, bool isIpv4) {
        if (isIpv4) {
            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);
            addr.sin_addr.s_addr = INADDR_ANY;
            return ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        } else {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port   = htons(port);
            addr.sin6_addr   = in6addr_any;
            return ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        }
    }
    
public:
    /* Constructor */
    Acceptor(const uint16_t &port) {
        #if defined(_WIN32) || defined(_WIN64)
        win_startup();
        #endif

        fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        
        bool isIpv4 = false;
        if (fd == -1) {
            fd = ::socket(AF_INET, SOCK_STREAM, 0); // IPv4 fallback
            if (fd == -1) throw std::runtime_error("socket() failed");
            isIpv4 = true;
        };

        if (!setOptions(isIpv4))        throw std::runtime_error("setsockopt() failed");
        if (bind(port, isIpv4) != 0)            throw std::runtime_error("bind() failed");
        if (::listen(fd, backlog) != 0) throw std::runtime_error("listen() failed");
    }
    
    const int getFd() const { return fd; }

    ~Acceptor() {
        if (fd == -1) return;
        #if defined(_WIN32) || defined(_WIN64)
        closesocket(fd);
        win_cleanup();

        #else
        close(fd);
        #endif
    }

    /* Accepts a connection from the backlog and creates a socket.
     * @throw std::runtime_error on error. */
    std::optional<Socket> accept() {
        sockaddr_in6 client_addr{};
        socklen_t len = sizeof(client_addr);

        int client_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                throw std::runtime_error("accept() failed.");
            }

            return std::nullopt;
        }
        
        char buf[INET6_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET6, &client_addr.sin6_addr, buf, sizeof(buf));

        return Socket(client_fd);
    }

private:
    int fd = -1;
    inline const static int reuseaddr = 1; // Reuse socket for new connections after first one happens (1 = true, 0 = false)
    inline const static int v6only = 0;    // Accepts only ipv6 connections (1 = true, 0 = false)
    inline const static int backlog = 10;
};

/* Helper function to check which protocol the ip address is using.
 * @param A given ip string.
 * @return 0 if Ipv4, 1 if Ipv6 and -1 if invalid ip. */ 
int checkIPVersion(const std::string& ip) {
    unsigned char buf[sizeof(struct in6_addr)]; // Sufficient size for both IPv4/Ipv6

    if (inet_pton(AF_INET, ip.c_str(), buf) == 1) {
        return 0;
    }

    if (inet_pton(AF_INET6, ip.c_str(), buf) == 1) {
        return 1;
    }
    return -1;
}

/* Connects to a given address and creates a socket for that connection.
 * @param host The server's IPV4 address.
 * @param port The port that the server is listening to.
 * @return Socket for the accepted connection.
 * @throw std::runtime_error on error. 
 * @warning No current support to hostname resolution. */
 Socket connect(const std::string &host, const uint16_t &port) {

    int fd;

    int ipVersion = checkIPVersion(host);

    if (ipVersion == 0) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);

        ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            #if defined(_WIN32) || defined(_WIN64)
            closesocket(fd);
            #else
            close(fd);
            #endif
            throw std::runtime_error("connect() failed");
        }

    } else if (ipVersion == 1) {
        fd = ::socket(AF_INET6, SOCK_STREAM, 0);

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);

        ::inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            #if defined(_WIN32) || defined(_WIN64)
            closesocket(fd);
            #else
            close(fd);
            #endif
            throw std::runtime_error("connect() failed");
        }
    } else {
        throw std::runtime_error("invalid address: " + host);
    }
    
    return Socket(fd);
}


/* Multi-Socket Poller implementation */
class Poller {
public:
    void add(Acceptor &acceptor, short events = POLLIN) {
        add(acceptor.getFd(), events);
    }

    void add(Socket &&socket, short events = POLLIN) {
        add(socket.getFd(), events);
        m_clients.emplace(socket.getFd(), std::move(socket));
    }

    int wait(int timeout_ms = -1) {
        if (m_fds.empty()) return 0;

        #if defined(__linux__)
        int res = ::poll(m_fds.data(), m_fds.size(), timeout_ms);
        #else
        int res = WSAPoll(m_fds.data(), m_fds.size(), timeout_ms);
        #endif

        if (res < 0) throw std::runtime_error("poll() failed");
        return res;
    }

    bool isReady(int fd, short event = POLLIN) const {
        for (const auto& pfd : m_fds) {
            if (pfd.fd == fd) {
                return (pfd.revents & event) != 0;
            }
        }
        return false;
    }

    template<typename F>
    void execute(F func) {
        for (auto it = m_clients.begin(); it != m_clients.end(); ) {
            int clientFd = it->first;
            cppsocket::Socket& clientSock = it->second;
            
            if (isReady(clientFd, POLLIN)) {
                try {
                    func(clientSock);
                    ++it;
                } catch (const std::runtime_error& e) {
                    m_fds.erase(
                        std::remove_if(m_fds.begin(), m_fds.end(),
                            [clientFd](const pollfd& p) { return p.fd == clientFd; }),
                        m_fds.end()
                    );
                    it = m_clients.erase(it);
                }
            } else {
                ++it;
            }
        }
    }

    const std::vector<pollfd>& getFds() const {
        return m_fds;
    }

private:
    std::vector<pollfd> m_fds;
    std::map<int, Socket> m_clients;

    void add(int fd, short events = POLLIN) {
        if (fd == -1) throw std::runtime_error("Trying to add invalid fd to poller");

        for (auto& pfd : m_fds) {
            if (pfd.fd == fd) {
                pfd.events = events;
                return;
            }
        }

        m_fds.push_back({fd, events, 0});
    }
};


}

#endif
