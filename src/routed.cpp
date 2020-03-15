#include "common.hpp"
#include "table.hpp"
#include <stdio.h>
#include <signal.h>
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

struct MessageType{
    static const int BeNeighbor = 1;
    static const int Alive = 2;
    static const int LinkCostPing = 3;
    static const int LSA = 4;
    static const int FileInit = 5;
    static const int FileChunk = 6;
    
    static const int LinkCostPing_Response = 7;
    static const int LSA_Response = 8;
    static const int FileInit_Response = 9;
    static const int FileChunk_Response = 10;

    static std::string get(int messageType){
        switch(messageType){
            case BeNeighbor: return "BeNeighbor";
            case Alive: return "Alive";
            case LinkCostPing: return "LinkCostPing";
            case LSA: return "LSA";
            case FileInit: return "FileInit";
            case FileChunk: return "FileChunk";

            case LinkCostPing_Response: return "LinkCostPing_Response";
            case LSA_Response: return "LSA_Response";
            case FileInit_Response: return "FileInit_Response";
            case FileChunk_Response: return "FileChunk_Response";

            default: return "Unknown";
        }
    }
};

class Log{
public:
    void write(const std::string& msg){
        printf("%s\n", msg.c_str());
    }
}log;

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
};
typedef std::map<std::string, LocalLink> LinkMap;
LinkMap localLinks;


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
            msg << MessageType::get(item.second.packetType) << std::string(" packet connection to ") << item.first << std::string(" error");
            log.write(msg.str());
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
            msg << MessageType::get(item.second.packetType) << std::string(" packet connection to ") << item.first << std::string(" error");
            log.write(msg.str());
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
                log.write(m.str());
                return 1; // lost packet
            }
            if(rand()%1000 < 100){ // 10% probability
                std::stringstream m;
                m << MessageType::get(msg.packetType) << std::string(" packet injecting error");
                log.write(m.str());
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
                            log.write(std::string("Message sent"));
                        break;
                        case 1: // unknown packet type
                        {
                            std::stringstream m;
                            m << MessageType::get(msg.packetType) << std::string(" packet unrecognized");
                            log.write(m.str());
                        }
                        break;
                        case 2: // data integrity error
                        {
                            std::stringstream m;
                            m << MessageType::get(msg.packetType) << std::string(" packet corrupted. Retrying...");
                            log.write(m.str());
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
            m << MessageType::get(msg.packetType) << std::string(" packet transfer to ") << sock->host << std::string(" error");
            log.write(m.str());
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
            msg << MessageType::get(item.second.packetType) << std::string(" packet connection to ") << item.first << std::string(" error");
            log.write(msg.str());
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
            }
        };
		struct HandleAlive : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
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
            }
        };
        struct HandleFileInit : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                unsigned long sessionID = message.getSessionID();
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), sessionID, message.payload.data);
                RouterMessage msg;
                msg.routerID = getIpByHost("localhost");//_table->getThis();
                msg.packetType = MessageType::FileChunk;
                std::string s = "FILE_ACK";
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

		/* Messages we respond with:
		 * 1. Be_Neighbor_Response
		 * 2. Alive_Response
		 * 3. Link_Cost_Pong
		 * 4. FileAck
		 */
        struct HandleBeNeighborResp : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };
		struct HandleAliveResp : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };
        struct HandleLinkCostPong : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };
		struct HandleFileAck : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%s, %i, %s)\n", message.routerID.c_str(), message.packetType, message.payload.data);
            }
        };

        handlers.insert(std::pair<int, RouterMessageHandler*>(1, new HandleBeNeighbor()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(2, new HandleAlive()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(3, new HandleLinkCostPing()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(4, new HandleLSA()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(5, new HandleFileInit()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(6, new HandleFileChunk()));
		
        handlers.insert(std::pair<int, RouterMessageHandler*>(7, new HandleLinkCostPing()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(8, new HandleLSA()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(9, new HandleFileInit()));
        handlers.insert(std::pair<int, RouterMessageHandler*>(10, new HandleFileChunk()));
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
		// for each localLink():
		//   switch(linkState) {
		//   case NEW:
		//   case DEAD:
		//     send Be_Neighbor_Request for each link
		//     advance link state machine for each link
		//   case ACQUISITION:
		//     if (time > BeNeighborSent) { advanceState(DEAD); }
		//   case ALIVE:
		//     if (time > helloInterval) { sendHello(); advanceState(HELLO_UPDATE) }
		//     if (time > updateInterval) { sendPing(); advanceState(COST_UPDATE) }
		//   case HELLO_UPDATE:
		//     if (time > helloSent) { advanceState(DEAD); }
		//   case COST_UPDATE:
		//     if (time > updateIntEnd) { recomputeCost(link); updateTable(); sendLSA(); }
		//   }
		//
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
                log.write(msg.str());
                (*handlers[rmsg.packetType])(rmsg, item.socket);
                response = 0;
            } else {
                response = 1;
                log.write(std::string("Received unrecognized packet"));
            }
        } else {
            response = 2;
            log.write(std::string("Received corrupt packet"));
        }
        
        item.socket->writeMsg(response);

        /*TextMessage response;
        response.text = "Hello world!";
        bool s = true;
        if(s){
            s = s && response.send(item.socket);
            if(s){
                printf("Replied\n");
            }
        }*/
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
	UI(const routerId& rid, unsigned short port) {
		_running = false;
		_port = port;
		_table = std::make_shared<RoutingTable>(rid);
		_worker = std::make_shared<WorkerThread>(_table);
        _messagePusher = std::make_shared<MessagePusher>(true);
        thisWorker = _worker;
        thisMessagePusher = _messagePusher;
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
				_start();
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
			else if (line.find("show-table") != std::string::npos)
				_showTable();
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
		std::cout << "Debug: lid - " << linkID << ", rid - " << destRouterID
				  << ", cost - " << cost << ", helloInt - " << helloInt
				  << ", updateInt - " << updateInt << std::endl;
		if (destRouterID == "0" && cost == 0 && helloInt == 0 && updateInt == 0) {
			auto i = localLinks.find(linkID);
			if (i != localLinks.cend())
				localLinks.erase(i);
			std::cout << "Deleted link \"" << linkID << "\"." << std::endl;
			return;
		} else if (cost == 0 || helloInt == 0 || updateInt == 0) {
			std::cout << "Invalid arguments, type \"help\" for usage." << std::endl;
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
		}
		version = version_in;
		std::cout << "Version set to " << version << "." << std::endl;
	}

	void _start() {
		if (_running) {
			std::cout << "Router is already running." << std::endl;
			return;
		}
		//TODO: insert links into table
		_worker->start();
        _messagePusher->start();
		_listener->start(_port);
		std::cout << "Router v" << version << " started." << std::endl;
		_running = true;
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
		std::cout << "Path:" << std::endl;
	}

	void _showTable() {
		std::cout << "Table:{}" << std::endl;
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
			std::cout << "\n\tshow-table - Print the current routing table";
			std::cout << std::endl;
		} else if (cmd == "help start") {
			std::cout << "start" << std::endl;
			std::cout << "\n\tStart communicating with other routers. Configuration cannot be";
			std::cout << " changed manually after this point.\n";
		} else if (cmd == "help show-table") {
			std::cout << "show-table" << std::endl;
			std::cout << "\n\tPrints the current routing table.\n";
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
			std::cout << "\n\t<hello> - (unsigned long) The interval between alive messages (ms)";
			std::cout << "\n\t<update> - (unsigned long) The interval between updating link cost (ms)\n";
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
    std::cout << "[link_list=<file_path>] [corrupt_msgs]\n\n";
    std::cout << "<router_ip> - The IP address and ID assigned to this router\n";
    std::cout << "<file_dir> - Absolute path to the directory to send/recv files\n";
    std::cout << "[link_list=<file_path>] - Absolute path to a file with links to add to router\n";
    std::cout << "[corrupt_msgs=<bool>] - 1 to simulate packet and network errors (default: 0)\n";
}

int main(int args, char** argv){
    if (args < 3 || args > 5) {
        usage();
        return 1;
    }

    std::string routerIP(argv[1]);
    std::string fileDir(argv[2]);
    std::string linkFile;
    unsigned short port = default_port;
    bool corruptMsgs = false;
    for (int i = 3; i < args; i++) {
        if (!strncmp(argv[i], "corrupt_msgs=", 13)) {
            corruptMsgs = (argv[i][13] == '1');
        } else if (!strncmp(argv[i], "link_list=", 10)) {
            linkFile = argv[i] + 10;
        }
    }

	UI ui{routerIP, port};
    if (!linkFile.empty())
        ui.loadLinks(linkFile);
    ui.loop();

    return 0;
}
