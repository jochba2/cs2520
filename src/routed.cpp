#include "common.hpp"
#include "table.hpp"
#include <stdio.h>
#include <unistd.h>
#include <map>
#include <vector>
#include <memory>

/*                    .--------- COST_UPDATE -------------.
 *                    V                ^                  |
 * NEW -> ACQUISITION .> ALIVE --------|-> HELLO_UPDATE . |
 *      |             |     ^                           | |
 *      |             |     |---------------------------| |
 *      DEAD <.-------.---------------------------------.-.
 */
enum linkState {
    NEW,
    ACQUISITION,
    ALIVE,
    DEAD,
    HELLO_UPDATE,
    COST_UPDATE
};

const unsigned short default_port = 5678;

namespace MessageType{
    static const int BeNeighbor = 1;
    static const int Alive = 2;
    static const int LinkCostPing = 3;
    static const int LSA = 4;
    static const int FileInit = 5;
    static const int FileChunk = 6;
    static const int FileAck = 7;
    
    static const int BeNeighbor_Response = 8;
    static const int Alive_Response = 9;
    static const int LinkCostPing_Response = 10;
    static const int LSA_Response = 11;
    static const int FileInit_Response = 12;
    static const int FileChunk_Response = 13;
    static const int FileAck_Response = 14;

    static std::string get(int messageType){
        switch(messageType){
            case BeNeighbor: return "BeNeighbor";
            case Alive: return "Alive";
            case LinkCostPing: return "LinkCostPing";
            case LSA: return "LSA";
            case FileInit: return "FileInit";
            case FileChunk: return "FileChunk";
            case FileAck: return "FileAck";

            case BeNeighbor_Response: return "BeNeighbor_Response";
            case Alive_Response: return "Alive_Response";
            case LinkCostPing_Response: return "LinkCostPing_Response";
            case LSA_Response: return "LSA_Response";
            case FileInit_Response: return "FileInit_Response";
            case FileChunk_Response: return "FileChunk_Response";
            case FileAck_Response: return "FileAck_Response";

            default: return "Unknown";
        }
    }
};

class Log{
public:
    void write(const std::string& msg){
        printf("%s\n", msg.c_str());
    }
}logger;

struct {
    Monitor monitor;
    unsigned long getSessionID(){
        monitor.lock();
        unsigned long result = nextSessionID++;
        monitor.unlock();
        return result;
    }
    void retireSessionID(unsigned long sessionID){
        monitor.lock();
        monitor.unlock();
    }
    unsigned long nextSessionID = 0;
}sessionManager;

typedef std::map<unsigned long, std::shared_ptr<RouterMessage>> LSA_Map;
struct LocalLink {
public:
    LocalLink() : dest(""), cost(0), helloInt(0), updateInt(0), state(NEW) {}
    LocalLink(const routerId& rid,
              double cost,
              unsigned long hello,
              unsigned long update)
        : dest(rid),
          cost(cost),
          helloInt(hello),
          updateInt(update),
          state(NEW) {}

    routerId dest;
    double cost;
    unsigned long helloInt;
    unsigned long updateInt;
    linkState state;
    Stopwatch helloInterval, helloSent;
    Stopwatch updateInterval, updateSent;
    Stopwatch BeNeighborSent, updateIntEnd;
    LSA_Map pending_LSAs;
};
typedef std::map<std::string, LocalLink> LinkMap;
LinkMap localLinks;

std::map<routerId, unsigned long> lastSeqNums;


Monitor tableMonitor;
int version = 1;

struct IncomingMessage {
    Buffer buffer;
    TcpSocket *socket;
};

struct RouterMessageHandler{
    virtual void operator()(RouterMessage& message, TcpSocket* socket)=0;
};

typedef std::pair<TcpSocket*, RouterMessage> MessageToPushToSocket;

typedef std::pair<routerId, RouterMessage> MessageToPush;

// thread that synchrously or asynchnously sends messages
class MessagePusher : public WorkerService<MessageToPush>{
public:
    MessagePusher(bool corruptMsgs) : WorkerService<MessageToPush>(){ _corruptMsgs = corruptMsgs;}

    TcpSocket* push(const MessageToPush& item){
        TcpClient client;
        client.host = item.first;
        client.port = default_port;
        TcpSocket* sock = NULL;
        int result = 3;
        try{
            sock = client.connect();
            if(sock){
                result = tryToSend(sock, item.second);
                //delete sock;
            }
        }catch(std::exception& ex){
            std::stringstream msg;
            msg << MessageType::get(item.second.packetType) << std::string(" packet connection to ") << item.first << std::string(" error ") << ex.what();
            logger.write(msg.str());
            if(sock)
                delete sock;
            sock = NULL;
        }
        switch(result){
            case 0:
                return sock;
            case 1:
            case 2:
                delete sock;
                return push(item);
            default:
                break;
        }
        if(sock)
            delete sock;
        return NULL;
    }

    bool push(const MessageToPushToSocket& item){
        TcpSocket* sock = item.first;
        int result = 3;
        try{
            if(sock){
                result = tryToSend(sock, item.second);
                //delete sock;
            }
        }catch(std::exception& ex){
            std::stringstream msg;
            msg << MessageType::get(item.second.packetType) << std::string(" packet connection to ") << item.first << std::string(" error ") << ex.what();
            logger.write(msg.str());
            //if(sock)
                //delete sock;
        }
        switch(result){
            case 0:
                return false;
            case 1:
                return false;
            case 2:
                return push(MessageToPush(item.first->host, item.second));
            default:
                return false;
        }
        return false;
    }
    
    void schedule(const MessageToPush& item){
        WorkerService<MessageToPush>::schedule(item);
    }


private:
    bool _corruptMsgs;
    
    int tryToSend(TcpSocket* sock, const RouterMessage& msg){
        // returns 0 for success
        // returns 1 for lost packet
        // returns 2 for corruption response
        // returns 3 for error
        Buffer packet = msg.getPacket();
        if(_corruptMsgs){
            if(rand()%1000 < 50){ // 5% probability
                std::stringstream m;
                m << MessageType::get(msg.packetType) << std::string(" packet lost");
                logger.write(m.str());
                return 1; // lost packet
            }
            if(rand()%1000 < 100){ // 10% probability
                std::stringstream m;
                m << MessageType::get(msg.packetType) << std::string(" packet injecting error");
                logger.write(m.str());
                packet.injectErrors(1); // bit flip
            }
        }
        try{
            bool s = (sock->writeData((char*)packet.data, packet.size) == packet.size);
            s = s && sock->sendEOF();
            if(!s)
                return 3;
            if(!sock->isReadClosed()){
                int response;
                if(sock->readMsg(response)){
                    switch(response){
                        case 0: // message accepted
                            logger.write(std::string("Message sent"));
                        break;
                        case 1: // unknown packet type
                        {
                            std::stringstream m;
                            m << MessageType::get(msg.packetType) << std::string(" packet unrecognized");
                            logger.write(m.str());
                        }
                        break;
                        case 2: // data integrity error
                        {
                            std::stringstream m;
                            m << MessageType::get(msg.packetType) << std::string(" packet corrupted. Retrying...");
                            logger.write(m.str());
                            return 2;
                        }
                        break;
                        default:
                        break;
                    }
                    return 0;
                }
                return 3;
            }
            return 0;
        }catch(std::exception& ex){
            std::stringstream m;
            m << MessageType::get(msg.packetType) << std::string(" packet transfer to ") << sock->host << std::string(" error ") << ex.what();
            logger.write(m.str());
            return 3;
        }
        return 0;
    }
    
    void executeTask(MessageToPush& item){

        TcpClient client;
        client.host = item.first;
        client.port = default_port;
        TcpSocket* sock = NULL;
        int result = 3;
        try{
            sock = client.connect();
            if(sock){
                result = tryToSend(sock, item.second);
                delete sock;
                sock = NULL;
            }
        }catch(std::exception& ex){
            std::stringstream msg;
            msg << MessageType::get(item.second.packetType) << std::string(" packet connection to ") << item.first << std::string(" error ") << ex.what();
            logger.write(msg.str());
            if(sock)
                delete sock;
        }
        if(result==2)
            schedule(item);
    }
    
    void intervalTasks() override {
    }
};

std::shared_ptr<MessagePusher> thisMessagePusher;

std::shared_ptr<RoutingTable> thisTable;

void _sendLSA() {
    std::cout << "Broadcasting LSA." << std::endl;

    // build message
    std::shared_ptr<RouterMessage> msgOut = std::make_shared<RouterMessage>();
    msgOut->routerID = thisTable->getThis();
    msgOut->packetType = MessageType::LSA;
    unsigned long seqNum = sessionManager.getSessionID();
    std::ostringstream oss;
    oss << thisTable->getThis() << " ";     // Advertising Router field
    oss << seqNum << " ";                   // LSA SeqNum
    size_t count = 0;
    for (auto& ll : localLinks) {
        if (!(ll.second.state == ALIVE ||
              ll.second.state == HELLO_UPDATE ||
              ll.second.state == COST_UPDATE)) { continue; }
        count++;
    }
    oss << count << std::endl;  // # links
    for (auto& ll : localLinks) {
        if (!(ll.second.state == ALIVE ||
              ll.second.state == HELLO_UPDATE ||
              ll.second.state == COST_UPDATE)) { continue; }
        oss << ll.first << " ";             // link ID
        oss << ll.second.dest << " ";       // link dest
        oss << ll.second.cost << std::endl; //link cost
    }
    std::string body = oss.str();
    msgOut->payload.write(body.c_str(), body.size() + 1);

    // broadcast to all my ALIVE neighbors
    for (auto& ll : localLinks) {
        if (!(ll.second.state == ALIVE ||
              ll.second.state == HELLO_UPDATE ||
              ll.second.state == COST_UPDATE)) { continue; }
        ll.second.pending_LSAs[seqNum] = msgOut;
        thisMessagePusher->schedule(MessageToPush(ll.second.dest, *msgOut));
    }
}

void _sendLSA_Ack(const routerId& dest, unsigned long seqNum) {
    std::cout << "Sending LSA_Response." << std::endl;
    RouterMessage msgOut;
    msgOut.routerID = thisTable->getThis();
    msgOut.packetType = MessageType::LSA_Response;
    std::ostringstream oss;
    oss << seqNum;
    std::string body = oss.str();
    msgOut.payload.write(body.c_str(), body.size() + 1);
    thisMessagePusher->schedule(MessageToPush(dest, msgOut));
}

void _forwardLSA(const RouterMessage& msgIn, unsigned long seqNum) {
    // copy message
    std::cout << "Forwarding LSA message to neighbors." << std::endl;
    std::shared_ptr<RouterMessage> msgOut = std::make_shared<RouterMessage>();
    msgOut->routerID = thisTable->getThis();
    msgOut->packetType = MessageType::LSA;
    msgOut->payload = msgIn.payload;

    // broadcast to all my ALIVE neighbors, except the link this came from
    for (auto& ll : localLinks) {
        if (!(ll.second.state == ALIVE ||
              ll.second.state == HELLO_UPDATE ||
              ll.second.state == COST_UPDATE)) { continue; }
        if (ll.second.dest == msgIn.routerID) { continue; }
        ll.second.pending_LSAs[seqNum] = msgOut;
        thisMessagePusher->schedule(MessageToPush(ll.second.dest, *msgOut));
    }
}

class WorkerThread : public WorkerService<IncomingMessage>{
public:
    WorkerThread(std::shared_ptr<RoutingTable> rt) : _table(rt) {
        /*-------------------- Message Handlers --------------------------*/

        /* Messages we initiate:
         * 1. Be_Neighbor_Request
         * 2. Alive
         * 3. Link_Cost_Ping
         * 4. LSA
         * 5. FileInit
         * 6. FileChunk
         */
        struct HandleBeNeighbor : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
                int versionIn;
                std::istringstream iss;
                iss.str((char*)message.payload.data);
                if (!(iss >> versionIn)) {
                    std::cout << "Couldn't parse BeNeighbor message. Ignoring." << std::endl;
                    return;
                }
                std::string response = "1";
                if (versionIn != version) {
                    std::cout << "Version mismatch. Refusing connection." << std::endl;
                    response = "0";
                }
                RouterMessage msgOut;
                msgOut.routerID = thisTable->getThis();
                msgOut.packetType = MessageType::BeNeighbor_Response;
                msgOut.payload.write(response.c_str(), response.size() + 1);
                thisMessagePusher->schedule(MessageToPush(message.routerID, msgOut));
            }
        };
        struct HandleAlive : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                RouterMessage msg;
                msg.routerID = thisTable->getThis();
                msg.packetType = MessageType::Alive_Response;
                thisMessagePusher->schedule(MessageToPush(message.routerID, msg));
            }
        };
        struct HandleLinkCostPing : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };
        struct HandleLSA : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);

                // parse
                linkList newLinks;
                routerId advertisingRID;
                unsigned long seqNum;
                size_t numLinks;
                std::istringstream iss;
                iss.str((char*)message.payload.data);
                if (!(iss >> advertisingRID >> seqNum >> numLinks)) {
                    std::cout << "Couldn't parse LSA message header. Ignoring." << std::endl;
                    return;
                }
                if (advertisingRID == thisTable->getThis()) {
                    std::cout << "Got LSA with self as advertising router. Ignoring." << std::endl;
                    return;
                }
                std::string linkID;
                routerId linkDest;
                double linkCost;
                while (iss >> linkID >> linkDest >> linkCost) {
                    newLinks.push_back(linkWeight(linkDest, linkCost));
                    numLinks--;
                }
                if (numLinks) {
                    std::cout << "Couldn't parse LSA message body. Ignoring." << std::endl;
                    return;
                }

                // ack
                std::cout << "Got LSA message." << std::endl;
                _sendLSA_Ack(message.routerID, seqNum);

                // ignore duplicates (handle cycles in network)
                if (lastSeqNums.count(advertisingRID) && lastSeqNums[advertisingRID] <= seqNum) {
                    std::cout << "Got duplicate LSA. Ignoring." << std::endl;
                    return;
                } else {
                    lastSeqNums[advertisingRID] = seqNum;
                }

                // flood
                _forwardLSA(message, seqNum);

                // update our graph & paths
                tableMonitor.lock();
                thisTable->updateLinks(advertisingRID, newLinks);
                std::cout << "Network Graph:\n" << thisTable->printGraph();
                tableMonitor.unlock();
            }
        };
        struct HandleFileInit : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), sessionID, message.payload.data);
                RouterMessage msg;
                msg.routerID = thisTable->getThis();
                msg.packetType = MessageType::FileChunk;
                std::string s = "FILE_CHUNK";
                size_t len = s.size();
                msg.payload.write((char*)s.c_str(), len+1);
                msg.addSessionID(sessionID);
                //thisMessagePusher->push(MessageToPushToSocket(socket, msg));
                thisMessagePusher->schedule(MessageToPush(message.routerID, msg));
            }
        };
        struct HandleFileChunk : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), sessionID, message.payload.data);
                sessionManager.retireSessionID(sessionID);
            }
        };
        struct HandleFileAck : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), sessionID, message.payload.data);
                sessionManager.retireSessionID(sessionID);
            }
        };
        
		/* Messages we respond with:
		 * 1. Be_Neighbor_Response
		 * 2. Alive_Response
		 * 3. Link_Cost_Pong
		 * 4. FileAck
		 */
        struct HandleBeNeighbor_Response : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
                bool found = false;
                auto ll = localLinks.begin();
                for (; ll != localLinks.end(); ++ll) {
                    if (ll->second.dest == message.routerID) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::cout << "Got message on unkown link. Ignoring." << std::endl;
                    return;
                }
                LocalLink& link = ll->second;
                link.helloInterval.start();
                std::string response;
                if (message.payload.data[0] != '1') {
                    std::cout << "Neighbor refused request, retrying later." << std::endl;
                    link.state = DEAD;
                    return;
                }
                std::cout << "Neighbor '" << link.dest << "' accepted." << std::endl;
                link.updateInterval.start();
                link.state = ALIVE;
                tableMonitor.lock();
                thisTable->updateMyLink(link.dest, link.cost);
                std::cout << "Network Graph:\n" << thisTable->printGraph();
                tableMonitor.unlock();
                _sendLSA();
            }
        };
		struct HandleAlive_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
                for (auto& ll : localLinks) {
                    if (ll.second.dest == message.routerID) {
                        std::cout << "Confirmed link is alive." << std::endl;
                        ll.second.helloInterval.start();
                        ll.second.state = ALIVE;
                        break;
                    }
                }
            }
        };
        struct HandleLinkCostPing_Response : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };
		struct HandleLSA_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
                unsigned long seqNumIn;
                std::istringstream iss;
                iss.str((char*)message.payload.data);
                if (!(iss >> seqNumIn)) {
                    std::cout << "Couldn't parse LSA_Response message. Ignoring." << std::endl;
                    return;
                }
                std::cout << "Got LSA_Response." << std::endl;
                for (auto& ll : localLinks) {
                    if (ll.second.dest == message.routerID) {
                        ll.second.pending_LSAs.erase(seqNumIn);
                    }
                }
            }
        };
		struct HandleFileInit_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };
		struct HandleFileChunk_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };
		struct HandleFileAck_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };

        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::BeNeighbor, new HandleBeNeighbor()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::Alive, new HandleAlive()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::LinkCostPing, new HandleLinkCostPing()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::LSA, new HandleLSA()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::FileInit, new HandleFileInit()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::FileChunk, new HandleFileChunk()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::FileAck, new HandleFileAck()));
		
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::BeNeighbor_Response, new HandleBeNeighbor_Response()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::Alive_Response, new HandleAlive_Response()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::LinkCostPing_Response, new HandleLinkCostPing_Response()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::LSA_Response, new HandleLSA_Response()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::FileInit_Response, new HandleFileInit_Response()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::FileChunk_Response, new HandleFileChunk_Response()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(MessageType::FileAck_Response, new HandleFileAck_Response()));
        /*-------------------- Message Handlers --------------------------*/
    }
    ~WorkerThread(){
        for(std::map<int, RouterMessageHandler*>::iterator it=handlers.begin(); it!=handlers.end(); ++it){
            delete it->second;
        }
    }
    std::map<int, RouterMessageHandler*> handlers;
private:
	std::shared_ptr<RoutingTable> _table;

	// for sending link association, alive, and cost messages
	// also for retransmitting unacked file chunks
	void intervalTasks() override {
        for (auto& ll : localLinks) {
            LocalLink& link = ll.second;
            std::string& dest = link.dest;
            switch (link.state) {
            case NEW:
            case DEAD:
                if (link.helloInterval.elapsed() > link.helloInt) {
                    RouterMessage msg;
                    msg.routerID = thisTable->getThis();
                    msg.packetType = MessageType::BeNeighbor;
                    std::ostringstream oss;
                    oss << version;
                    std::string body = oss.str();
                    msg.payload.write(body.c_str(), body.size() + 1);
                    std::cout << "Sending BeNeighbor request to '" << dest << "'." << std::endl;
                    thisMessagePusher->schedule(MessageToPush(dest, msg));
                    link.helloSent.start();
                    link.state = ACQUISITION;
                }
                break;
            case ACQUISITION:
            case HELLO_UPDATE:
                // we didn't get a BeNeighbor_Response or Alive_Response in time
                // take the link offline for a little while
                if (link.helloSent.elapsed() > link.helloInt) {
                    std::cout << "BeNeighbor/Alive check timed out. Clearing link and scheduling retry." << std::endl;
                    link.helloInterval.start();
                    link.state = DEAD;
                    tableMonitor.lock();
                    thisTable->updateMyLink(link.dest, 0);
                    tableMonitor.unlock();
                    _sendLSA();
                }
                break;
            case ALIVE:
                /*for (auto& LSA_msg : link.pending_LSAs) {
                    std::cout << "Retransmitting unACK'd LSA message." << std::endl;
                    thisMessagePusher->schedule(MessageToPush(link.dest, *(LSA_msg.second)));
                }*/
                if(link.helloInterval.elapsed()>link.helloInt){
                    std::cout << "Sending Alive message." << std::endl;
                    RouterMessage msg;
                    msg.routerID = thisTable->getThis();
                    msg.packetType = MessageType::Alive;
                    thisMessagePusher->schedule(MessageToPush(dest, msg));
                    link.helloInterval.start();
                    link.helloSent.start();
                    link.state = HELLO_UPDATE;
                    continue;
                }
                /*if(link.updateInterval.elapsed()>link.updateInt){
                    RouterMessage msg;
                    msg.routerID = thisTable->getThis();
                    msg.packetType = MessageType::Alive;
                    thisMessagePusher->schedule(MessageToPush(dest, msg));
                    link.updateInterval.start();
                    link.updateSent.start();
                    link.state = COST_UPDATE;
                }*/
                break;
            case COST_UPDATE:
                //TODO: check for update response
                break;
            }
        }
        // for each outgoingFile():
        //   if (time > ackInterval) { resendFileChunks(); }
	}

    void executeTask(IncomingMessage& item){
        RouterMessage rmsg;
        rmsg.routerID = item.socket->host;
        int response = 0;
        if(rmsg.readFrom(&item.buffer)){
            if(handlers.find(rmsg.packetType)!=handlers.end()){
                std::stringstream msg;
                msg << std::string("Received ") << MessageType::get(rmsg.packetType) << std::string(" packet from ") << rmsg.routerID;
                logger.write(msg.str());
                (*handlers[rmsg.packetType])(rmsg, item.socket);
                response = 0;
            } else {
                response = 1;
                logger.write(std::string("Received unrecognized packet"));
            }
        } else {
            response = 2;
            logger.write(std::string("Received corrupt packet"));
        }
        
        item.socket->writeMsg(response);

        delete item.socket;
    }
};

std::shared_ptr<WorkerThread> thisWorker;


class ConnectionServer : public TcpThreadedServer{
public:
    ConnectionServer(std::shared_ptr<WorkerThread> wt) : _worker(wt) {}
    inline void processConnection(TcpSocket *socket){
        IncomingMessage msg;
        msg.buffer.readFrom(socket);
        msg.socket = socket;
        _worker->schedule(msg);
    }
private:
    std::shared_ptr<WorkerThread> _worker;
};

class UI {
public:
    UI(const routerId& rid, unsigned short port, bool corruptMsgs) {
        _running = false;
        _port = port;
        _table = std::make_shared<RoutingTable>(rid);
        _worker = std::make_shared<WorkerThread>(_table);
        _messagePusher = std::make_shared<MessagePusher>(corruptMsgs);
        thisWorker = _worker;
        thisMessagePusher = _messagePusher;
        thisTable = _table;
        _listener.reset(new ConnectionServer(_worker));
    }

    void loop() {
        constexpr char PROMPT[] = "routed> ";
        std::cout << PROMPT;
        while (std::cin) {
            std::string line;
            std::getline(std::cin, line);
            if (line.find("help") != std::string::npos)
                _help(line);
            else if (line.find("exit") != std::string::npos) {
                _exit();
                return;
            } else if (line.find("start") != std::string::npos)
                start();
            else if (line.find("scp") != std::string::npos)
            {
                // --------------------- TEST ---------------------
                std::string destination;
                std::istringstream iss;
                iss.str(line.substr(3));
                if (!(iss >> destination)) {
                    std::cout << "Invalid arguments, type \"help\" for usage." << std::endl;
                    continue;
                }
                printf("Sending FileInit message to %s ...\n", destination.c_str());
                
                RouterMessage msg;
                msg.routerID = _table->getThis();
                msg.packetType = MessageType::FileInit;
                std::string s = "FILE_DATA";
                size_t len = s.size();
                msg.payload.write((char*)s.c_str(), len+1);
                msg.addSessionID(sessionManager.getSessionID());
                _messagePusher->schedule(MessageToPush(destination, msg));
                
                // --------------------- /TEST ---------------------
                //break; //TODO
            }
            else if (line.find("set-version") != std::string::npos)
                _setVersion(line.substr(11));
            else if (line.find("set-link") != std::string::npos)
                _setLink(line.substr(8));
            else if (line.find("show-path") != std::string::npos)
                _showPath(line.substr(9));
            else if (line.find("show-graph") != std::string::npos)
                _showGraph();
            else if(line.size() > 0)
                std::cout << "Unrecognized command" << std::endl;
            std::cout << PROMPT;
        }
    }

    void loadLinks(const std::string& linkFile) {
        std::string linkID;
        std::string destRouterID;
        double cost;
        unsigned long helloInt;
        unsigned long updateInt;
        int count = 0;
        std::ifstream linkStream(linkFile.c_str());
        while (linkStream >> linkID >> destRouterID >> cost >> helloInt >> updateInt) {
            localLinks[linkID] = LocalLink(destRouterID, cost, helloInt, updateInt);
            count++;
        }
        std::cout << "Loaded " << count << " links at startup.";
    }

    void start() {
        if (_running) {
            std::cout << "Router is already running." << std::endl;
            return;
        }
        _worker->start();
        _messagePusher->start();
        _listener->start(_port);
        std::cout << "Router v" << version << " started." << std::endl;
        _running = true;
    }

private:
    bool _running;
    unsigned short _port;
    std::shared_ptr<RoutingTable> _table;
    std::shared_ptr<WorkerThread> _worker;
    std::shared_ptr<MessagePusher> _messagePusher;
    std::unique_ptr<ConnectionServer> _listener;

    void _setLink(const std::string& cmd) {
        if (_running) {
            std::cout << "Cannnot modify state from CLI after calling \"start\"." << std::endl;
            return;
        }
        std::string linkID;
        std::string destRouterID;
        double cost;
        unsigned long helloInt;
        unsigned long updateInt;
        std::istringstream iss;
        iss.str(cmd);
        if (!(iss >> linkID >> destRouterID >> cost >> helloInt >> updateInt)) {
            std::cout << "Invalid arguments, type \"help\" for usage." << std::endl;
            return;
        }
        if (destRouterID == "0" && cost == 0 && helloInt == 0 && updateInt == 0) {
            auto i = localLinks.find(linkID);
            if (i != localLinks.cend())
                localLinks.erase(i);
            std::cout << "Deleted link \"" << linkID << "\"." << std::endl;
            return;
        } else if (cost == 0 || helloInt == 0 || updateInt == 0) {
            std::cout << "Invalid arguments, type \"help\" for usage." << std::endl;
            return;
        }
        LocalLink& l = localLinks[linkID];
        l.dest = destRouterID;
        l.cost = cost;
        l.helloInt = helloInt;
        l.updateInt = updateInt;
        std::cout << "Modified link \"" << linkID << "\"." << std::endl;
    }

    void _setVersion(const std::string& cmd) {
        if (_running) {
            std::cout << "Cannnot modify state from CLI after calling \"start\"." << std::endl;
            return;
        }
        int version_in;
        std::istringstream iss;
        iss.str(cmd);
        if (!(iss >> version_in)) {
            std::cout << "Invalid argument, type \"help\" for usage." << std::endl;
            return;
        }
        version = version_in;
        std::cout << "Version set to " << version << "." << std::endl;
    }

    void _exit() {
        if (_running) {
            _listener->stop();
            _worker->stop();
            _messagePusher->stop();
        }
        std::cout << "Goodbye." << std::endl;
    }

    void _showPath(const std::string& cmd) {
        if (!_running) {
            std::cout << "Router isn't running yet, can't compute paths." << std::endl;
            return;
        }
        routerId dest;
        std::istringstream iss;
        iss.str(cmd);
        if (!(iss >> dest)) {
            std::cout << "Invalid argument, type \"help\" for usage." << std::endl;
            return;
        }
        tableMonitor.lock();
        std::string route = _table->printRoute(dest);
        tableMonitor.unlock();
        std::cout << "Path:" << std::endl << route;
    }

    void _showGraph() {
        if (!_running) {
            std::cout << "Router isn't running yet, can't compute network graph." << std::endl;
            return;
        }
        tableMonitor.lock();
        std::string graph = _table->printGraph();
        tableMonitor.unlock();
        std::cout << "Network Graph:" << std::endl << graph;
    }

    void _help(std::string& cmd) {
        if (cmd == "help") {
            std::cout << "CLI Usage:";
            std::cout << "\n\thelp - Print this message";
            std::cout << "\n\texit - Shut down the router";
            std::cout << "\n\tstart - Start communicating with other routers";
            std::cout << "\n\tscp - Send a file to another router";
            std::cout << "\n\tset-version - Change the routing protocol version";
            std::cout << "\n\tset-link - Add/Update/Delete a link on this router";
            std::cout << "\n\tshow-path - Use this router's table to calculate";
            std::cout << "the path to another router";
            std::cout << "\n\tshow-graph - Print the this node's current network graph";
            std::cout << std::endl;
        } else if (cmd == "help start") {
            std::cout << "start" << std::endl;
            std::cout << "\n\tStart communicating with other routers. Configuration cannot be";
            std::cout << " changed manually after this point.\n";
        } else if (cmd == "help show-graph") {
            std::cout << "show-graph" << std::endl;
            std::cout << "\n\tPrints this node's view of the current network graph.\n";
        } else if (cmd == "help scp") {
            std::cout << "scp <file_name> <destination_ip>" << std::endl;
            std::cout << "\n\tSends a packetized file from directory <file_dir> over the";
            std::cout << " router network to another router's directory <file_dir>.\n";
            std::cout << "\n\t<file_name> - The name of the file in path <file_dir>";
            std::cout << "\n\t<destination_ip> - The IP of the router to send the file to\n";
        } else if (cmd == "help set-version") {
            std::cout << "set-version <version>" << std::endl;
            std::cout << "\n\tSets the routing protocol version.\n";
            std::cout << "\n\t<version> - (int) The new version to set\n";
        } else if (cmd == "help set-link") {
            std::cout << "set-link <link_id> <destination_router_ip> <cost> <hello> <update>" << std::endl;
            std::cout << "\n\tAdd/Update/Delete a link on this router. All fields are always required.";
            std::cout << " For delete, set all values besides link_id to 0.\n";
            std::cout << "\n\t<link_id> - The ID of the link";
            std::cout << "\n\t<destination_router_ip> - The IP of the router on the other end of the link";
            std::cout << "\n\t<cost> - (double) The initial cost of the link";
            std::cout << "\n\t<hello> - (unsigned long) The interval between alive messages (sec)";
            std::cout << "\n\t<update> - (unsigned long) The interval between updating link cost (sec)\n";
        } else if (cmd == "help show-path") {
            std::cout << "show-path <destination_ip>" << std::endl;
            std::cout << "\n\tCalculates the path to <destination_ip> using this router's";
            std::cout << " current routing table and displays all hops.\n";
            std::cout << "\n\t<destination_ip> - IP address of the destination router\n";
        } else {
            std::cout << "Unrecognized command" << std::endl;
        }
    }
};

void usage() {
    std::cout << "Usage: ./routed <router_ip> <file_dir> ";
    std::cout << "[link_list=<file_path>] [corrupt_msgs] [auto_start]\n\n";
    std::cout << "<router_ip> - The IP address and ID assigned to this router\n";
    std::cout << "<file_dir> - Absolute path to the directory to send/recv files\n";
    std::cout << "[link_list=<file_path>] - Absolute path to a file with links to add to router\n";
    std::cout << "[corrupt_msgs=<bool>] - 1 to simulate packet and network errors (default: 0)\n";
    std::cout << "[auto_start=<bool>] - start router automatically (default: 0)\n";
}

int main(int args, char** argv){
    if (args < 3 || args > 6) {
        usage();
        return 1;
    }

    std::string routerIP(argv[1]);
    std::string fileDir(argv[2]);
    std::string linkFile;
    unsigned short port = default_port;
    bool corruptMsgs = false;
    bool autoStart = false;
    for (int i = 3; i < args; i++) {
        if (!strncmp(argv[i], "corrupt_msgs=", 13)) {
            corruptMsgs = (argv[i][13] == '1');
        } else if (!strncmp(argv[i], "link_list=", 10)) {
            linkFile = argv[i] + 10;
        } else if (!strncmp(argv[i], "auto_start=", 11)) {
            autoStart = (argv[i][11] == '1');
        }
    }

    UI ui{routerIP, port, false};
    if (!linkFile.empty())
        ui.loadLinks(linkFile);
    if (autoStart)
        ui.start();
    ui.loop();

    return 0;
}
