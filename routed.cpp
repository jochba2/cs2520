#include "common.hpp"
#include<stdio.h>
#include<signal.h>
#include<unistd.h>
#include <map>

struct IncomingMessage{
    Buffer buffer;
    TcpSocket *socket;
};

struct RouterMessageHandler{
    virtual void operator()(RouterMessage& message, TcpSocket* socket)=0;
};

class WorkerThread : public WorkerService<IncomingMessage>{
public:
    WorkerThread(){
        /*-------------------- Message Handlers --------------------------*/

        struct Handler_1 : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
        handlers.insert(std::pair<int, RouterMessageHandler*>(5678, new Handler_1()));

        /*-------------------- Message Handlers --------------------------*/
    }
    ~WorkerThread(){
        for(std::map<int, RouterMessageHandler*>::iterator it=handlers.begin(); it!=handlers.end(); ++it){
            delete it->second;
        }
    }
    std::map<int, RouterMessageHandler*> handlers;
private:
    void executeTask(IncomingMessage &item){
        RouterMessage rmsg;
        if(rmsg.readFrom(&item.buffer)){
            if(handlers.find(rmsg.packetType)!=handlers.end()){
                (*handlers[rmsg.packetType])(rmsg, item.socket);
            } else {
                printf("Unknown packet\n");
            }
        } else {
            printf("Integrity error\n");
        }

        TextMessage response;
        response.text = "Hello world!";
        bool s = true;
        if(s){
            s = s && response.send(item.socket);
            if(s){
                printf("Replied\n");
            }
        }
        delete item.socket;
    }
}workerThread; 


class ConnectionServer : public TcpThreadedServer{
public:
    inline void processConnection(TcpSocket *socket){
        IncomingMessage msg;
        msg.buffer.readFrom(socket);
        msg.socket = socket;
        workerThread.schedule(msg);
    }
}server;


Monitor monitor;
bool toExit = false;
void sig_handler(int signo)
{
    if (signo == SIGINT){
        printf("Terminating...\n");
        monitor.lock();
        toExit = true;
        monitor.unlock();
        monitor.notifyAll();
    }
}

int main(int args, char** argv){
    char buffer[65536];
    unsigned short port = atoi(argv[1]);
    workerThread.start();
    server.start(port);
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        throw std::runtime_error("Can't register INT signal handler");
    printf("Router started (%s).\n", getIpByHost("localhost").c_str());
    monitor.lock();
    while(!toExit)
        monitor.wait(1000);
    monitor.unlock();
    server.stop();
    workerThread.stop();
    return 0;
}