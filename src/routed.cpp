#include "common.hpp"
#include "table.hpp"
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <map>
#include <vector>
#include <memory>

struct IncomingMessage{
    Buffer buffer;
    TcpSocket *socket;
};

struct RouterMessageHandler{
    virtual void operator()(RouterMessage& message, TcpSocket* socket)=0;
};

class WorkerThread : public WorkerService<IncomingMessage>{
public:
    WorkerThread(const routerId& id) : _table(id) {
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

	RoutingTable _table;

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

// Any thread can pick up the signal, so protect it with a lock
Monitor monitor;
bool exitShowMsgs = false;
void sig_handler(int signo)
{
    if (signo == SIGINT){
        monitor.lock();
        exitShowMsgs = true;
        monitor.unlock();
    }
}


class UI {
public:
	UI(std::shared_ptr<WorkerThread> wt) : _worker(wt) {}
	void loop() {
		constexpr char PROMPT[] = "routed> ";
		std::cout << PROMPT;
		while (std::cin) {
			std::string line;
			std::getline(std::cin, line);
			if (line.find("help") != std::string::npos)
				help(line);
			else if (line.find("exit") != std::string::npos)
				return;
			else if (line.find("show-msgs") != std::string::npos)
				showMsgs();
			else if (line.find("scp") != std::string::npos)
				break; //TODO
			else if (line.find("set-version") != std::string::npos)
				break; //TODO
			else if (line.find("set-link") != std::string::npos)
				break; //TODO
			else if (line.find("show-path") != std::string::npos)
				break; //TODO
			std::cout << PROMPT;
		}
	}

	typedef struct linkFileRow {
		linkFileRow(std::string lid, std::string rid, double c) :
			linkID(lid), destRouterID(rid), linkCost(c) {}
		std::string linkID;
		std::string destRouterID;
		double linkCost;
	} linkFileRow;

	void loadLinks(const std::string& linkFile) {
		std::ifstream linkStream(linkFile.c_str());
		std::string linkID;
		std::string destRouterID;
		double linkCost = 0.0;
		std::vector<linkFileRow> initLinks;

		while (linkStream >> linkID >> destRouterID >> linkCost) {
			initLinks.emplace_back(linkID, destRouterID, linkCost);
		}

		//TODO: Add work queue items to handle the addition of these links
	}

private:
	std::shared_ptr<WorkerThread> _worker;

	void showMsgs() {
		monitor.lock();
		exitShowMsgs = false;
		monitor.unlock();

		std::cout << "Listening to messages... (ctl-c to return to menu)" << std::endl;
		while (!exitShowMsgs) {
			//TODO: read from buffer of messages sent/rcvd to print here
			std::cout << "..." << std::endl;
		}
	}

	void help(std::string& cmd) {
		if (cmd == "help") {
			std::cout << "CLI Usage:";
			std::cout << "\n\thelp - Print this message";
			std::cout << "\n\texit - Shut down the router";
			std::cout << "\n\tscp - Send a file to another router";
			std::cout << "\n\tset-version - Change the routing protocol version";
			std::cout << "\n\tset-link - Add/Update/Delete a link on this router";
			std::cout << "\n\tshow-path - Use this router's table to calculate";
			std::cout << "the path to anothe router";
			std::cout << "\n\tshow-msgs - Print the messages sent/rcvd by this router";
			std::cout << std::endl;
		} else if (cmd == "help show-msgs") {
			std::cout << "show-msgs" << std::endl;
			std::cout << "\n\tPrints a live stream of the messages this router is";
			std::cout << " or receiving. Use ctl-c to exit to the CLI menu.\n";
		} else if (cmd == "help scp") {
			std::cout << "scp <file_name> <destination_ip>" << std::endl;
			std::cout << "\n\tSends a packetized file from directory <file_dir> over the";
			std::cout << " router network to another router's directory <file_dir>.\n";
			std::cout << "\n\t<file_name> - The name of the file in path <file_dir>";
			std::cout << "\n\t<destination_ip> - The IP of the router to send the file to\n";
		} else if (cmd == "help set-version") {
			std::cout << "set-version <version>" << std::endl;
			std::cout << "\n\tUpdates the routing protocol version on-the-fly.\n";
			std::cout << "\n\t<version> - (int) The new version to set\n";
		} else if (cmd == "help set-link") {
			std::cout << "set-link <link_id> <destination_router_ip> <cost>" << std::endl;
			std::cout << "\n\tAdds/Updates/Delets a link on this router.\n";
			std::cout << "\n\t<link_id> - The ID of the link";
			std::cout << "\n\t<destination_router_ip> - The IP of the router on the other end of the link";
			std::cout << "\n\t<cost> - (double) The initial cost estimate of this link. Updates automatically\n";
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
    std::cout << "[link_list=<file_path>] [port=<int>] [corrupt_msgs]\n\n";
    std::cout << "<router_ip> - The IP address assigned to this router\n";
    std::cout << "<file_dir> - Absolute path to the directory to send/recv files\n";
    std::cout << "[link_list=<file_path>] - Absolute path to a file with links to add to router\n";
    std::cout << "[port=<int>] - Port to listen on (default: 5678)\n";
    std::cout << "[corrupt_msgs=<bool>] - 1 to simulate packet and network errors (default: 0)\n";
}

int main(int args, char** argv){
    if (args < 2) {
        usage();
        return 1;
    }

    std::string routerIP(argv[1]);
    std::string fileDir(argv[2]);
    std::string linkFile;
    unsigned short port = 5678;
    bool corruptMsgs = false;
    for (int i = 3; i < args; i++) {
        if (!strncmp(argv[i], "port=", 5)) {
            port = atoi(argv[i] + 5);
        } else if (!strncmp(argv[i], "corrupt_msgs=", 13)) {
            corruptMsgs = (argv[i][13] == '1');
        } else if (!strncmp(argv[i], "link_list=", 10)) {
            linkFile = argv[i] + 10;
        }
    }

    std::shared_ptr<WorkerThread> wt = std::make_shared<WorkerThread>(routerIP);
    wt->start();

	ConnectionServer server{wt};
    server.start(port);

	UI ui{wt};

    if (signal(SIGINT, sig_handler) == SIG_ERR)
        throw std::runtime_error("Can't register INT signal handler");
    printf("Router started (%s).\n", routerIP.c_str());

    if (!linkFile.empty())
        ui.loadLinks(linkFile);

    ui.loop();

    server.stop();
    wt->stop();

    return 0;
}
