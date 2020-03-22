#ifndef _STRUCTURES_HPP
#define _STRUCTURES_HPP

#include "sockets.hpp"
#include <list>
#include <map>
#include <iostream>
#include <fstream>
#include <memory>
#include <set>
#include <algorithm>
#include <string>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <time.h>

#include "thread.hpp"

class Stopwatch{
public:
    Stopwatch(){start();}
    void start(){
        clock_gettime(CLOCK_REALTIME, &_abstime);
    }
    unsigned long elapsed(){
        timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        return (t.tv_sec - _abstime.tv_sec)*1000 + (t.tv_nsec - _abstime.tv_nsec)/1000000;
    }
private:
    timespec _abstime;
   
};

// Worker thread
// override executeTask to set item processing
// run schedule() to schedule item for detached async execution
template<typename T>
class WorkerService : public Thread{
public:
    void execute(){
        monitor.lock();
        while(!toStop()){
            while(!(requestQueue.size()>0 || toStop())) {
                monitor.wait(1000);
				intervalTasks();
			}
            if(toStop())
                break;
            while(requestQueue.size()>0){
                T item = requestQueue.back();
                requestQueue.pop_back();
                monitor.unlock();
                monitor.notify();
                executeTask(item);
                monitor.lock();
            }
        }
        monitor.unlock();
    }
    void schedule(const T& item){
        {
            monitor.lock();
            requestQueue.push_front(item);
            monitor.unlock();
        }
        monitor.notify();
    }
protected:
	virtual void intervalTasks() = 0;
    virtual void executeTask(T& item)=0;
private:
    std::list<T> requestQueue;
};

// Threaded server
// accepts connections on a separate thread
// override processConnection() to set the connection processing
class TcpThreadedServer : protected Thread{
public:
    TcpThreadedServer() : Thread() {server = NULL;}
    ~TcpThreadedServer(){stop();}
    void start(unsigned short port){
        if(server != NULL){
            if(this->port == port)
                return;
            stop();
        }
        this->port = port;
        server = new TcpServer(port);
        try{
            Thread::start();
        } catch(std::exception& ex){
            delete server;
            throw;
        }
    }
    void stop(){
        if(server!=NULL){
            Thread::stop();
            delete server;
            server = NULL;
        }
    }
    void execute(){
        while(!toStop()){
            if(server->waitForConnection(1)){
                TcpSocket *socket = server->acceptConnection();
                if(socket!=NULL){
                    processConnection(socket);
                }
            }

        }
    }
protected:
    virtual void processConnection(TcpSocket *socket)=0;
private:
    unsigned short port;
    TcpServer *server;
};

static inline std::string getIpByHost(std::string host)
{
    std::string ip = host;
    struct hostent *he;
    struct in_addr **addr_list;
    //bool found = false;
    if ( (he = gethostbyname( host.c_str() ) ) != NULL){
        addr_list = (struct in_addr **) he->h_addr_list;
        for(int i = 0; addr_list[i] != NULL; i++){
            char iip[101];
            strncpy(iip , inet_ntoa(*addr_list[i]), 100);
            ip = std::string(iip);
            //found = true;
            break;
        }
    }
    return ip;
}

static inline void force_directory(std::string directory_name)
{
    DIR* dir;
    char errmsg[512];
    std::string delimiter = "/";

    size_t pos = 0;
    std::string token;
    std::string cdir;
    while ((pos = directory_name.find(delimiter)) != std::string::npos) {
        token = directory_name.substr(0, pos);
        cdir += token;
        dir = opendir(cdir.c_str());
        if (dir)
        {
            closedir(dir);
        }
        else if (ENOENT == errno)
        {
            if(mkdir(cdir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)!=0){
                sprintf(errmsg, "Directory %s can't be created", cdir.c_str());
                throw std::runtime_error(errmsg);
            }
        }
        else
        {
            sprintf(errmsg, "Directory %s can't be opened", cdir.c_str());
            throw std::runtime_error(errmsg);
        }
        cdir += "/";
        directory_name.erase(0, pos + delimiter.length());
    }
    cdir += directory_name;
    dir = opendir(cdir.c_str());
    if (dir)
    {
        closedir(dir);
    }
    else if (ENOENT == errno)
    {
        if(mkdir(cdir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)!=0){
            sprintf(errmsg, "Directory %s can't be created", cdir.c_str());
            throw std::runtime_error(errmsg);
        }
    }
    else
    {
        sprintf(errmsg, "Directory %s can't be opened", cdir.c_str());
        throw std::runtime_error(errmsg);
    }
}

struct Message{
    virtual bool send(TcpSocket* socket){return true;};
    virtual bool receive(TcpSocket* socket){return true;};
    void sendString(const std::string& str, TcpSocket *socket){
        int sz=str.size();
        socket->writeMsg(sz);
        socket->writeData(const_cast<char*>(str.c_str()), sz);
    }
    void receiveString(std::string& str, TcpSocket *socket){
        int sz;
        socket->readMsg(sz);
        std::ostringstream os;
        while(sz>0){
            char buf[512];
            int s = sz>512?512:sz;
            socket->readData(buf, s);
            os.write(buf, s);
            sz -= s;
        }
        str = os.str();
    }
};

struct TextMessage : public Message{
    std::string text;
    bool send(TcpSocket* socket){
        sendString(text, socket);
        return true;
    };
    bool receive(TcpSocket* socket){
        receiveString(text, socket);
        return true;
    };
};


#endif // _STRUCTURES_HPP
