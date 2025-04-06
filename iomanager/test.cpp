#include "ioscheduler.h"
#include <iostream>
#include <sys/types.h>         
#include <sys/socket.h>
#include <netinet/in.h>
#include <string>
#include <arpa/inet.h>
#include <errno.h>

char recv_data[4096];

const char data[] = "GET / HTTP/1.0\r\n\r\n";

int sock;

void func()
{
    recv(sock, recv_data, 4096, 0);
    std::cout << recv_data << std::endl << std::endl;
}

void func2()
{
    send(sock, data, sizeof(data), 0);
}

int main(void)
{
    IOManager manager(4);

    sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(80);
    server.sin_addr.s_addr = inet_addr("127.0.0.1");

    fcntl(sock, F_SETFL, O_NONBLOCK);

    connect(sock, (struct sockaddr*)&server, sizeof(server));

    manager.addEvent(sock, IOManager::WRITE, &func2);
    manager.addEvent(sock, IOManager::READ, &func);

    std::cout << "event has been posted\n\n";



    return 0;
}
