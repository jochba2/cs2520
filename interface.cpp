#include "common.hpp"
#include<stdio.h>
#include<signal.h>
#include<unistd.h>



int main(int args, char** argv){
    char buffer[65536];
    TcpClient client;
    client.host = std::string(argv[1]);
    client.port = atoi(argv[2]);
    TcpSocket *socket = client.connect();
    printf("Type message to send to the server:");
    scanf("%s", buffer);
    unsigned short len = strlen(buffer);
    
    {
        RouterMessage rmsg;
        rmsg.routerID = 1234;
        rmsg.packetType = 5678;
        rmsg.payload.write(buffer, len);
        Buffer packet = rmsg.getPacket();
        //packet.injectErrors(1);
        socket->writeData((char*)packet.data, packet.size);
    }
    socket->sendEOF();
    TextMessage msg;
    msg.receive(socket);
    delete socket;
    printf("Router answered [%s]\n", msg.text.c_str());

    return 0;

}