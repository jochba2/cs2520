#ifndef _SOCKETS_HPP
#define _SOCKETS_HPP

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <unistd.h>

class TcpSocket;

class TcpClient {
public:
    std::string host;
    unsigned short port;
    inline TcpSocket* connect();
};

class TcpServer {
public:
    inline TcpServer(unsigned short port);
    inline ~TcpServer();
    unsigned short port;
    inline TcpSocket* acceptConnection();
    inline bool waitForConnection(unsigned long timeout);
private:
    int sockfd;
};

class TcpSocket {
public:
    std::string host;
    inline ~TcpSocket();

    /* Uses compile-time polymorphism and static casting to get "free" serialization of an
     * object */
    template<typename T> inline bool readMsg(T& msg){ // read entire message
        unsigned int len;
        bool result = readMsg((void*)(&msg), len);
        return result || len==sizeof(T);
    }
    template<typename T> inline bool writeMsg(T& msg){ // write entire message
        return writeMsg((void*)(&msg), (unsigned int)sizeof(T));
    }
    inline bool readMsg(void* msg, unsigned int &len); // read entire message
    inline bool writeMsg(void* msg, unsigned int len); // write entire message
    inline int readData(char* data, int len);
    inline int writeData(char* data, int len);
    inline bool sendEOF();

private:
    TcpSocket(int sockfd) : sockfd(sockfd), readFinished(false), writeFinished(false){};
    int sockfd;
    bool readFinished, writeFinished;
    friend class TcpServer;
    friend class TcpClient;
};


inline TcpServer::TcpServer(unsigned short port){
    struct sockaddr_in serv_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        char erno[20];
        sprintf(erno, "%i", sockfd);
        std::string err("Error - could not start server :");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0){
        char erno[20];
        sprintf(erno, "%i", port);
        std::string err("Error - could not bind port ");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }
    listen(sockfd,5);
}

inline TcpSocket* TcpServer::acceptConnection(){
    int newsockfd;
    struct sockaddr_in cli_addr;
    socklen_t clilen;
    clilen = sizeof(cli_addr);
    newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0){
        char erno[20];
        sprintf(erno, "%i", newsockfd);
        std::string err("Error - could not accept :");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }

    char str[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &(cli_addr.sin_addr), str, INET_ADDRSTRLEN);

    TcpSocket* result = new TcpSocket(newsockfd);
    result->host = str;
    return result;

}

inline bool TcpServer::waitForConnection(unsigned long timeout){
    long arg;
    int ires;
    struct timeval tv;
    fd_set myset;
    arg = fcntl(sockfd, F_GETFL, NULL);
    arg |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, arg);

    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    FD_ZERO(&myset);
    FD_SET(sockfd, &myset);
    ires = select(sockfd+1, &myset, NULL, NULL, &tv);
    arg = fcntl(sockfd, F_GETFL, NULL);
    arg &= (~O_NONBLOCK);
    fcntl(sockfd, F_SETFL, arg);
    return ires>0;
}

inline TcpServer::~TcpServer(){
    ::close(sockfd);
}


inline TcpSocket* TcpClient::connect(){
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        char erno[20];
        sprintf(erno, "%i", sockfd);
        std::string err("Error - could open socket :");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }
    server = gethostbyname(host.c_str());
    if (server == NULL){
        std::string err("Error - no such host :");
        err = err + host;
        throw std::runtime_error(err.c_str());
    }
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(port);
    if (int er = ::connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0){
        char erno[20];
        sprintf(erno, "%i", er);
        std::string err("Error - could not connect :");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }

    char str[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &(server->h_addr), str, INET_ADDRSTRLEN);
    TcpSocket* result = new TcpSocket(sockfd);
    result->host = str;
    return result;
}

inline TcpSocket::~TcpSocket(){
    ::close(sockfd);
}

inline bool TcpSocket::readMsg(void* msg, unsigned int &len){

    int n = readData((char*)(&len), 4);
    if(n<4)
    {
        return false;
    }
    n = readData((char*)msg, len);
    if((unsigned int)n<len)
    {
        return false;
    }
    return true;
}

/* NB: This is a little brittle -- if char type is not 4 bytes, this won't work. */
inline bool TcpSocket::writeMsg(void* msg, unsigned int len){
    int n = writeData((char*)(&len), 4);
    if(n<4)
    {
        return false;
    }
    n = writeData((char*)msg, len);
    if((unsigned int)n<len)
    {
        return false;
    }
    return true;
}

inline int TcpSocket::readData(char* data, int len){
    if(len==0 || readFinished)
    {
        return 0;
    }
    int n = ::read(sockfd,data,len);
    if(n==0)
    {
        readFinished = true;
    }
    if (n < 0)
    {
        char erno[20];
        sprintf(erno, "%i", n);
        std::string err("Error - could not read from socket :");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }
    return n;
}

inline int TcpSocket::writeData(char* data, int len){
    if(len==0 || writeFinished)
    {
        return 0;
    }
    int n = ::write(sockfd,data,len);
    if(n==0)
    {
        writeFinished = true;
    }
    if (n < 0)
    {
        char erno[20];
        sprintf(erno, "%i", n);
        std::string err("Error - could not write to socket :");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }
    return n;
}

inline bool TcpSocket::sendEOF(){
    if(writeFinished)
    {
        return false;
    }
    int n = ::shutdown(sockfd, 1);
    if(n==0)
    {
        writeFinished = true;
    }
    if (n < 0)
    {
        char erno[20];
        sprintf(erno, "%i", n);
        std::string err("Error - could not send EOF to socket :");
        err = err + erno;
        throw std::runtime_error(err.c_str());
    }
    return n;
}

#endif /* _SOCKETS_HPP */
