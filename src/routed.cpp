#include "common.hpp"
#include "table.hpp"
#include <stdio.h>
#include <unistd.h>
#include <map>
#include <vector>
#include <memory>
#include <ctime>

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
const unsigned long file_chunk_size = 32;

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

std::shared_ptr<RoutingTable> thisTable;

class LogMsg {
public:
    LogMsg(std::ostream& s, Monitor* lck) : _out_stream(s), _lck(lck) {}
    ~LogMsg() { _lck->unlock(); }
    template<typename Streamable>
    std::ostream& operator<< (const Streamable& data) {
        _out_stream << data;
        return _out_stream;
    }
private:
    std::ostream& _out_stream;
    Monitor* _lck;
};

class Log{
public:
    Log(std::ostream& s = std::cerr) : _file_ptr(NULL) {
        _out_stream = new std::ostream(s.rdbuf());
    }
    ~Log() {
        if (_file_ptr != NULL) {
            _file_ptr->close();
            delete _file_ptr;
            delete _out_stream;
        }
    }
    void setLogFile(const std::string& path) {
        _file_ptr = new std::ofstream(path.c_str(), std::ofstream::out);
        delete _out_stream;
        _out_stream = new std::ostream(_file_ptr->rdbuf());
    }
    LogMsg messageIn(const RouterMessage& msg) {
        _lck.lock();
        *_out_stream << std::endl << "[" << now() << "] "
                     << "[" << MessageType::get(msg.packetType) << ": "
                     << msg.routerID << " -> " << thisTable->getThis() << "]: ";
        return LogMsg(*_out_stream, &_lck);
    }
    LogMsg messageOut(const RouterMessage& msg) {
        _lck.lock();
        *_out_stream << std:: endl << "[" << now() << "] "
                     << "[" << MessageType::get(msg.packetType) << ": "
                     << thisTable->getThis() << " -> " << msg.routerID << "]: ";
        return LogMsg(*_out_stream, &_lck);
    }
    template<typename Streamable>
    LogMsg operator<< (const Streamable& data) {
        _lck.lock();
        *_out_stream << std::endl << "[" << now() << "] " << data;
        return LogMsg(*_out_stream, &_lck);
    }
private:
    Monitor _lck;
    std::ostream* _out_stream;
    std::ofstream* _file_ptr;
    std::string now() {
        char str[100];
        std::ostringstream s;
        auto timet = std::time(nullptr);
        auto time = std::localtime(&timet);
        auto nano = std::clock();
        std::strftime(str, sizeof(str), "%m/%d/%Y %H:%M:%S", time);
        s << str << "." << nano;
        return s.str();
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
          state(NEW) {helloInterval.start(); helloSent.start(); updateInterval.start(); updateSent.start(); BeNeighborSent.start(); updateIntEnd.start();}

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
            logger.messageOut(item.second) << " packet connection to dest error: " << ex.what();
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
            logger.messageOut(item.second) << " packet connection to dest error: " << ex.what();
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
                logger.messageOut(msg) << "Simulating lost packet.";
                return 1; // lost packet
            }
            if(rand()%1000 < 100){ // 10% probability
                logger.messageOut(msg) << "Injecting packet error.";
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
                            logger.messageOut(msg) << "Sent.";
                        break;
                        case 1: // unknown packet type
                        {
                            logger.messageOut(msg) << "Receiver reports packet unrecognized.";
                        }
                        break;
                        case 2: // data integrity error
                        {
                            logger.messageOut(msg) << "Receivier reports packet corrupted. Retrying...";
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
            logger.messageOut(msg) << "Erorr during packet transfer to dest: " << ex.what();
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
            logger.messageOut(item.second) << "Connection to dest failed: " << ex.what();
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

void _sendLSA() {
    logger << "Broadcasting LSA.";

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
    logger << "Sending LSA_Response.";
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
    logger << "Forwarding LSA message to neighbors.";
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

std::string homeDirectory; // directory of uploads and downloads

class IncomingFileTransfer{
public:
    IncomingFileTransfer(const std::string& filename, unsigned int chunks){
        this->filename = filename;
        this->chunks.resize(chunks);
    }
    routerId source;
    std::string filename;
    std::vector<std::unique_ptr<Buffer>> chunks;
};

struct OutgoingFileTransferChunk{
    Stopwatch timer;
    bool ack_sent = false;
    Buffer data;
};

class OutgoingFileTransfer{
public:
    OutgoingFileTransfer(const std::string& filename){
        this->filename = filename;
        std::string filepath = homeDirectory + filename;
        logger << "Opening file \"" << filepath << "\" for transfer.";
        FILE *fp = fopen(filepath.c_str(), "rb");
        if(!fp)
            throw std::runtime_error("File not found");
        while(feof(fp)==0){
            char buf[file_chunk_size];
            size_t l = fread(buf, 1, file_chunk_size, fp);
            if(l==0)
                break;
            chunks.push_back({});
            OutgoingFileTransferChunk& chunk = chunks[chunks.size()-1];
            chunk.data.write(buf, l);
        }
        fclose(fp);
    }
    routerId source;
    routerId destination;
    unsigned long sessionID;
    Stopwatch timer;
    bool ack_sent = false;
    std::string filename;
    std::vector<OutgoingFileTransferChunk> chunks;
};

std::map<std::pair<routerId, unsigned long>, std::unique_ptr<IncomingFileTransfer>> incomingFileTransfers;
Monitor incomingFileTransfersLock;
std::map<unsigned long, std::unique_ptr<OutgoingFileTransfer>> outgoingFileTransfers;
Monitor outgoingFileTransfersLock;

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
                int versionIn;
                std::istringstream iss;
                iss.str((char*)message.payload.data);
                if (!(iss >> versionIn)) {
                    logger.messageIn(message) << "Couldn't parse BeNeighbor message. Ignoring.";
                    return;
                }
                std::string response = "1";
                if (versionIn != version) {
                    logger.messageIn(message) << "Version mismatch. Refusing connection.";
                    response = "0";
                }
                RouterMessage msgOut;
                msgOut.routerID = thisTable->getThis();
                msgOut.packetType = MessageType::BeNeighbor_Response;
                msgOut.payload.write(response.c_str(), response.size() + 1);
                logger.messageIn(message) << "Responding with: " << response;
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
           }
        };
        struct HandleLSA : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                // parse
                linkList newLinks;
                routerId advertisingRID;
                unsigned long seqNum;
                size_t numLinks;
                std::istringstream iss;
                iss.str((char*)message.payload.data);
                if (!(iss >> advertisingRID >> seqNum >> numLinks)) {
                    logger.messageIn(message) << "Couldn't parse LSA message header. Ignoring.";
                    return;
                }
                if (advertisingRID == thisTable->getThis()) {
                    logger.messageIn(message) << "Got LSA with self as advertising router. Ignoring.";
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
                    logger.messageIn(message) << "Couldn't parse LSA message body. Ignoring.";
                    return;
                }

                // ack
                _sendLSA_Ack(message.routerID, seqNum);

                // ignore duplicates (handle cycles in network)
                if (lastSeqNums.count(advertisingRID) && lastSeqNums[advertisingRID] <= seqNum) {
                    logger.messageIn(message) << "Got duplicate LSA (current version: "
                        << lastSeqNums[advertisingRID] << "). Ignoring." << std::endl;
                    return;
                } else {
                    lastSeqNums[advertisingRID] = seqNum;
                }

                // flood
                _forwardLSA(message, seqNum);

                // update our graph & paths
                tableMonitor.lock();
                thisTable->updateLinks(advertisingRID, newLinks);
                logger.messageIn(message) << "DEBUG Network Graph:\n" << thisTable->printGraph();
                tableMonitor.unlock();
            }
        };
        struct HandleFileInit : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                int index=0;
                size_t l;
                char* buf;

                message.payload.read(&l, sizeof(l), index); // read source
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string source(buf, l);
                delete buf;

                message.payload.read(&l, sizeof(l), index); // read destination
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string destination(buf, l);
                delete buf;
                
                if(destination!=thisTable->getThis()){
                    // retransmit to next hop
                    logger.messageIn(message) << "Routing FileInit message";

                    RouterMessage rmsg = message;
                    rmsg.routerID = thisTable->getThis();
                    routerId dest = thisTable->nextHop(destination);
                    thisMessagePusher->schedule(MessageToPush(dest, rmsg));
                    return;
                }
                
                message.payload.read(&l, sizeof(l), index); // read filename
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string filename(buf, l);
                delete buf;
                
                unsigned int n_chunks;
                message.payload.read(&n_chunks, sizeof(n_chunks), index); // read number of chunks
                
                logger.messageIn(message) << "File init from " << source << " to " << destination << ", spanning " << n_chunks << " chunks.";
                
                incomingFileTransfersLock.lock();
                auto& file = incomingFileTransfers[std::pair<routerId, unsigned long>(source, sessionID)];
                file = std::unique_ptr<IncomingFileTransfer>(new IncomingFileTransfer(filename, n_chunks));
                file->source = source;
                incomingFileTransfersLock.unlock();

                // return response
                RouterMessage rmsg;
                rmsg.routerID = thisTable->getThis();
                rmsg.packetType = MessageType::FileInit_Response;
                rmsg.addSessionID(sessionID);

                l = destination.size(); // destination
                rmsg.payload.write(&l, sizeof(l));
                rmsg.payload.write(destination.c_str(), l);

                l = source.size();  //  source
                rmsg.payload.write(&l, sizeof(l));
                rmsg.payload.write(source.c_str(), l);
                
                thisMessagePusher->schedule(MessageToPush(thisTable->nextHop(source), rmsg));
            }
        };
        struct HandleFileChunk : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                int index=0;
                size_t l;
                char* buf;

                message.payload.read(&l, sizeof(l), index); // read source
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string source(buf, l);
                delete buf;

                message.payload.read(&l, sizeof(l), index); // read destination
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string destination(buf, l);
                delete buf;
                
                if(destination!=thisTable->getThis()){
                    // retransmit to next hop
                    logger.messageIn(message) << "Routing FileChunk message";

                    RouterMessage rmsg = message;
                    rmsg.routerID = thisTable->getThis();
                    routerId dest = thisTable->nextHop(destination);
                    thisMessagePusher->schedule(MessageToPush(dest, rmsg));
                    return;
                }
                
                unsigned long seqnum;
                message.payload.read(&seqnum, sizeof(seqnum), index); // read sequence number
                
                unsigned long chunk_size;
                message.payload.read(&chunk_size, sizeof(chunk_size), index); // read chunk size
                
                incomingFileTransfersLock.lock();
                if(incomingFileTransfers.find(std::pair<routerId, unsigned long>(source, sessionID))!=incomingFileTransfers.end()){
                    auto& file = incomingFileTransfers[std::pair<routerId, unsigned long>(source, sessionID)];
                    auto& chunk = file->chunks[seqnum];
                    chunk = std::unique_ptr<Buffer>(new Buffer(chunk_size));
                    message.payload.read(chunk->data, chunk_size, index); // read chunk data
                    bool file_complete = true;
                    for(auto& chnk : file->chunks){
                        if(!chnk)
                            file_complete = false;
                    }
                    if(file_complete){
                        logger.messageIn(message) << "File " << file->filename << " received successfully.";
                        std::string filepath = homeDirectory + file->filename;
                        FILE *fp = fopen(filepath.c_str(), "wb");
                        if(fp){
                            for(auto& chnk : file->chunks){
                                fwrite(chnk->data, 1, chnk->size, fp);
                            }
                            fclose(fp);
                        }
                        incomingFileTransfers.erase(std::pair<routerId, unsigned long>(source, sessionID));
                    }
                }
                incomingFileTransfersLock.unlock();
                
                // TODO
                
                logger.messageIn(message) << "File chunk " << seqnum;
                
                

                // return acknowledgement response
                RouterMessage rmsg;
                rmsg.routerID = thisTable->getThis();
                rmsg.packetType = MessageType::FileChunk_Response;
                rmsg.addSessionID(sessionID);

                l = destination.size(); // destination
                rmsg.payload.write(&l, sizeof(l));
                rmsg.payload.write(destination.c_str(), l);

                l = source.size();  //  source
                rmsg.payload.write(&l, sizeof(l));
                rmsg.payload.write(source.c_str(), l);
                
                rmsg.payload.write(&seqnum, sizeof(seqnum));
                
                thisMessagePusher->schedule(MessageToPush(thisTable->nextHop(source), rmsg));

            }
        };
        struct HandleFileAck : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
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
                bool found = false;
                auto ll = localLinks.begin();
                for (; ll != localLinks.end(); ++ll) {
                    if (ll->second.dest == message.routerID) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    logger.messageIn(message) << "Got message about unkown link. Ignoring.";
                    return;
                }
                LocalLink& link = ll->second;
                link.helloInterval.start();
                std::string response;
                if (message.payload.data[0] != '1') {
                    logger.messageIn(message) << "Neighbor refused request, retrying later.";
                    link.state = DEAD;
                    return;
                }
                logger.messageIn(message) << "Neighbor '" << link.dest << "' accepted.";
                link.updateInterval.start();
                link.state = ALIVE;
                tableMonitor.lock();
                thisTable->updateMyLink(link.dest, link.cost);
                logger.messageIn(message) << "DEBUG Network Graph:\n" << thisTable->printGraph();
                tableMonitor.unlock();
                _sendLSA();
            }
        };
		struct HandleAlive_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                for (auto& ll : localLinks) {
                    if (ll.second.dest == message.routerID) {
                        logger.messageIn(message) << "Confirmed link " << ll.first << " is alive.";
                        ll.second.helloInterval.start();
                        ll.second.state = ALIVE;
                        break;
                    }
                }
            }
        };
        struct HandleLinkCostPing_Response : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
            }
        };
		struct HandleLSA_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long seqNumIn;
                std::istringstream iss;
                iss.str((char*)message.payload.data);
                if (!(iss >> seqNumIn)) {
                    logger.messageIn(message) << "Couldn't parse LSA_Response message. Ignoring.";
                    return;
                }
                for (auto& ll : localLinks) {
                    if (ll.second.dest == message.routerID) {
                        logger.messageIn(message) << "LSA marked as accepted.";
                        ll.second.pending_LSAs.erase(seqNumIn);
                    }
                }
            }
        };
		struct HandleFileInit_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                int index=0;
                size_t l;
                char* buf;

                message.payload.read(&l, sizeof(l), index); // read source
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string source(buf, l);
                delete buf;
                
                message.payload.read(&l, sizeof(l), index); // read destination
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string destination(buf, l);
                delete buf;
                
                
                if(destination!=thisTable->getThis()){
                    // retransmit to next hop
                    logger.messageIn(message) << "Routing FileInit_Response message";
                    RouterMessage rmsg = message;
                    rmsg.routerID = thisTable->getThis();
                    routerId dest = thisTable->nextHop(destination);
                    thisMessagePusher->schedule(MessageToPush(dest, rmsg));
                    return;
                }
                
                outgoingFileTransfersLock.lock();
                auto& file = outgoingFileTransfers[sessionID];
                file->ack_sent = true;
                outgoingFileTransfersLock.unlock();
                logger.messageIn(message) << "File transfer init acknowledged";
            }
        };
		struct HandleFileChunk_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                int index=0;
                size_t l;
                char* buf;

                message.payload.read(&l, sizeof(l), index); // read source
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string source(buf, l);
                delete buf;
                
                message.payload.read(&l, sizeof(l), index); // read destination
                buf = new char[l+1];
                message.payload.read(buf, l, index);
                std::string destination(buf, l);
                delete buf;
                
                
                if(destination!=thisTable->getThis()){
                    // retransmit to next hop
                    logger.messageIn(message) << "Routing FileChunk_Response message";
                    RouterMessage rmsg = message;
                    rmsg.routerID = thisTable->getThis();
                    routerId dest = thisTable->nextHop(destination);
                    thisMessagePusher->schedule(MessageToPush(dest, rmsg));
                    return;
                }
                
                unsigned long seqnum;
                message.payload.read(&seqnum, sizeof(seqnum), index); // read sequence number
                
                
                outgoingFileTransfersLock.lock();
                auto& file = outgoingFileTransfers[sessionID];
                auto& chunk = file->chunks[seqnum];
                chunk.ack_sent = true;
                outgoingFileTransfersLock.unlock();
                logger.messageIn(message) << "File transfer chunk " << seqnum << " acknowledged.";
            }
        };
		struct HandleFileAck_Response : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
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
                    logger << "Sending BeNeighbor request to '" << dest
                        << "' on link " << ll.first << ".";
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
                    logger << "BeNeighbor/Alive check timed out on link " << ll.first
                        << ". Clearing link and scheduling retry.";
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
                /*if(link.helloInterval.elapsed()>link.helloInt){
                    std::cout << "Sending Alive message." << std::endl;
                    RouterMessage msg;
                    msg.routerID = thisTable->getThis();
                    msg.packetType = MessageType::Alive;
                    thisMessagePusher->schedule(MessageToPush(dest, msg));
                    link.helloInterval.start();
                    link.helloSent.start();
                    link.state = HELLO_UPDATE;
                    continue;
                }*/
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
        outgoingFileTransfersLock.lock();
        std::vector<unsigned long> filesComplete;
        for(auto& file : outgoingFileTransfers){
            size_t l;
            if(file.second->ack_sent){
                bool file_sent = true;
                unsigned long seqnum = 0;
                for(auto& chunk : file.second->chunks){
                    if(!chunk.ack_sent){
                        file_sent = false;
                        if(chunk.timer.elapsed()>5000){
                            RouterMessage rmsg;
                            rmsg.routerID = thisTable->getThis();
                            rmsg.packetType = MessageType::FileChunk;
                            rmsg.addSessionID(file.first); // session id
                            
                            l = thisTable->getThis().size();  //  source
                            rmsg.payload.write(&l, sizeof(l));
                            rmsg.payload.write(thisTable->getThis().c_str(), l);
                            
                            l = file.second->destination.size(); // destination
                            rmsg.payload.write(&l, sizeof(l));
                            rmsg.payload.write(file.second->destination.c_str(), l);
                            
                            rmsg.payload.write(&seqnum, sizeof(seqnum)); // sequence number
                            
                            unsigned long chunk_size = chunk.data.size;
                            rmsg.payload.write(&chunk_size, sizeof(chunk_size)); // chunk size
                            
                            rmsg.payload += chunk.data; // chunk
                            
                            routerId dest = thisTable->nextHop(file.second->destination);
                            thisMessagePusher->schedule(MessageToPush(dest, rmsg));
                            chunk.timer.start();
                        }
                    }
                    seqnum++;
                }
                if(file_sent)
                    filesComplete.push_back(file.first);
            } else {
                if(file.second->timer.elapsed()>5000){
                    RouterMessage rmsg;
                    rmsg.routerID = thisTable->getThis();
                    rmsg.packetType = MessageType::FileInit;
                    rmsg.addSessionID(file.first); // session id
                    
                    l = thisTable->getThis().size();  //  source
                    rmsg.payload.write(&l, sizeof(l));
                    rmsg.payload.write(thisTable->getThis().c_str(), l);
                    
                    l = file.second->destination.size(); // destination
                    rmsg.payload.write(&l, sizeof(l));
                    rmsg.payload.write(file.second->destination.c_str(), l);
                    
                    l = file.second->filename.size(); // filename
                    rmsg.payload.write(&l, sizeof(l));
                    rmsg.payload.write(file.second->filename.c_str(), l);
                    
                    unsigned int n_chunks = file.second->chunks.size(); // number of chunks
                    rmsg.payload.write(&n_chunks, sizeof(n_chunks));
                    
                    routerId dest = thisTable->nextHop(file.second->destination);
                    thisMessagePusher->schedule(MessageToPush(dest, rmsg));
                    file.second->timer.start();
                    
                }
            }
        }
        for(auto& f : filesComplete){
            auto& file = outgoingFileTransfers[f];
            logger << "File " << file->filename << " sent successfully.";
            outgoingFileTransfers.erase(f);
        }
        outgoingFileTransfersLock.unlock();
	}

    void executeTask(IncomingMessage& item){
        RouterMessage rmsg;
        rmsg.routerID = item.socket->host;
        int response = 0;
        if(rmsg.readFrom(&item.buffer)){
            if(handlers.find(rmsg.packetType)!=handlers.end()){
                logger.messageIn(rmsg) << "Body: " << rmsg.payload.data;
                (*handlers[rmsg.packetType])(rmsg, item.socket);
                response = 0;
            } else {
                response = 1;
                logger.messageIn(rmsg) << "Unrecognized packet";
            }
        } else {
            response = 2;
            logger.messageIn(rmsg) << "Corrupt packet";
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
                std::string destination;
                std::string filename;
                std::istringstream iss;
                iss.str(line.substr(3));
                if (!(iss >> filename >> destination)) {
                    std::cout << "Invalid arguments, type \"help\" for usage." << std::endl;
                    continue;
                }
                printf("Sending FileInit message to %s ...\n", destination.c_str());
                
                /*RouterMessage msg;
                msg.routerID = _table->getThis();
                msg.packetType = MessageType::FileInit;
                std::string s = "FILE_DATA";
                size_t len = s.size();
                msg.payload.write((char*)s.c_str(), len+1);
                msg.addSessionID(sessionManager.getSessionID());
                _messagePusher->schedule(MessageToPush(destination, msg));*/
                
                
                outgoingFileTransfersLock.lock();
                auto& file = outgoingFileTransfers[sessionManager.getSessionID()];
                file = std::unique_ptr<OutgoingFileTransfer>(new OutgoingFileTransfer(filename));
                file->destination = destination;
                file->source = _table->getThis();
                outgoingFileTransfersLock.unlock();
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
    std::cout << "[log_file=<file_path>] - Absolute path to a file to write logs to (default: stderr)\n";
}

int main(int args, char** argv){
    if (args < 3 || args > 7) {
        usage();
        return 1;
    }

    std::string routerIP(argv[1]);
    std::string fileDir(argv[2]);
    std::string linkFile, logFile;
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
        } else if (!strncmp(argv[i], "log_file=", 9)) {
            logFile = argv[i] + 9;
        }
    }

    if (logFile.size() != 0) {
        logger.setLogFile(logFile);
    }

    if(fileDir.size()==0){
        homeDirectory = std::string("./");
    } else {
        homeDirectory = fileDir;
        while(homeDirectory.size()>0 && homeDirectory.c_str()[0]==' '){
            homeDirectory = std::string(&(homeDirectory.c_str()[1]), homeDirectory.size()-1);
        }
        while(homeDirectory.size()>0 && (homeDirectory.c_str()[homeDirectory.size()-1]==' ' or homeDirectory.c_str()[homeDirectory.size()-1]=='/')){
            homeDirectory = std::string(homeDirectory.c_str(), homeDirectory.size()-1);
        }
        if(homeDirectory.size()==0)
            homeDirectory = ".";
        homeDirectory.append("/");
    }

    UI ui{routerIP, port, false};
    if (!linkFile.empty())
        ui.loadLinks(linkFile);
    if (autoStart)
        ui.start();
    ui.loop();

    return 0;
}
