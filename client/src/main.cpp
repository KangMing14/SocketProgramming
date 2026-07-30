#include "control/Session.h"

int main() {
    SOCKET serverSock = Session::connectToServer("127.0.0.1", 4567);
    
    if (serverSock != INVALID_SOCKET) {
        Session::runClientSession(serverSock);
        closesocket(serverSock);
        WSACleanup();
    }
    
    return 0;
}