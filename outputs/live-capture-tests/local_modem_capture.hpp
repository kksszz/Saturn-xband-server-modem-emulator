#pragma once
// Loopback-only diagnostic sink. It implements no XBAND service or authentication.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <cstdint>

class LocalModemCapture {
    SOCKET client=INVALID_SOCKET,peer=INVALID_SOCKET;
    bool wsa=false;
public:
    std::vector<uint8_t> captured;
    LocalModemCapture()=default;
    LocalModemCapture(const LocalModemCapture&)=delete;
    LocalModemCapture& operator=(const LocalModemCapture&)=delete;
    ~LocalModemCapture(){close();if(wsa) WSACleanup();}
    bool connected() const {return client!=INVALID_SOCKET && peer!=INVALID_SOCKET;}
    void close(){if(client!=INVALID_SOCKET) closesocket(client);if(peer!=INVALID_SOCKET) closesocket(peer);client=peer=INVALID_SOCKET;}
    bool open() {
        close();captured.clear();
        if(!wsa){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)!=0)return false;wsa=true;}
        SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(listener==INVALID_SOCKET)return false;
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
        int length=sizeof(address);
        bool ok=bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 &&
            listen(listener,1)==0 && getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0;
        if(ok){
            client=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
            u_long nonblocking=1;
            ok=client!=INVALID_SOCKET && ioctlsocket(client,FIONBIO,&nonblocking)==0;
            if(ok && connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR) {
                ok=WSAGetLastError()==WSAEWOULDBLOCK;
                if(ok){fd_set ready;FD_ZERO(&ready);FD_SET(client,&ready);timeval limit{2,0};ok=select(0,nullptr,&ready,nullptr,&limit)>0;}
            }
            int error=0,errorLength=sizeof(error);
            if(ok)ok=getsockopt(client,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&errorLength)==0 && error==0;
            if(ok){fd_set ready;FD_ZERO(&ready);FD_SET(listener,&ready);timeval limit{2,0};ok=select(0,&ready,nullptr,nullptr,&limit)>0;}
            if(ok){peer=accept(listener,nullptr,nullptr);ok=peer!=INVALID_SOCKET && ioctlsocket(peer,FIONBIO,&nonblocking)==0;}
        }
        closesocket(listener);
        if(!ok)close();
        return ok;
    }
    bool sendByte(uint8_t v){
        if(!connected())return false;
        char byte=static_cast<char>(v);
        // Backpressure/failure is explicit; never silently drop a byte and report success.
        if(send(client,&byte,1,0)!=1){close();return false;}
        return true;
    }
    bool poll(){
        if(!connected())return false;
        char buffer[1024];
        for(;;){
            int count=recv(peer,buffer,sizeof(buffer),0);
            if(count>0){
                if(captured.size()+count>65536){close();return false;}
                captured.insert(captured.end(),buffer,buffer+count);
            } else if(count==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK)return true;
            else {close();return false;}
        }
    }
    bool sendPeerByte(uint8_t value){
        if(!connected())return false;
        char byte=static_cast<char>(value);
        if(send(peer,&byte,1,0)!=1){close();return false;}
        return true;
    }
    // Read only available UART capacity. Excess data remains in the TCP buffer.
    int receiveClient(uint8_t *buffer,int capacity){
        if(!connected() || capacity<0 || capacity>256)return -1;
        if(capacity==0)return 0;
        int count=recv(client,reinterpret_cast<char*>(buffer),capacity,0);
        if(count>0)return count;
        if(count==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK)return 0;
        close();return -1;
    }
};
