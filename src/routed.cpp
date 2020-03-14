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
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
		struct HandleAlive : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
        struct HandleLinkCostPing : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
		struct HandleLSA : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
        struct HandleFileInit : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
		struct HandleFileChunk : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
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
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
		struct HandleAliveResp : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
        struct HandleLinkCostPong : public RouterMessageHandler{
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
            }
        };
		struct HandleFileAck : public RouterMessageHandler {
            void operator()(RouterMessage& message, TcpSocket* socket){
                printf("Success (%i, %i, %s)\n", message.routerID, message.packetType, message.payload.data);
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
};

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
				break; //TODO
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
		_listener->start(_port);
		std::cout << "Router v" << version << " started." << std::endl;
		_running = true;
	}

	void _exit() {
		if (_running) {
			_listener->stop();
			_worker->stop();
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
    unsigned short port = 5678;
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
