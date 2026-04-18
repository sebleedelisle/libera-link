// -------------------------------------------------------------------------------------------------
//  File SockIDNServer.hpp
//
//  Copyright (c) 2020-2025 DexLogic, Dirk Apitz. All Rights Reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//  SOFTWARE.
//
// -------------------------------------------------------------------------------------------------
//  Change History:
//
//  07/2017 Dirk Apitz, created
//  01/2024 Dirk Apitz, modifications and integration into OpenIDN
//  04/2025 Dirk Apitz, independence from network layer through derived classes (Linux/LwIP support)
// -------------------------------------------------------------------------------------------------


#ifndef SOCKIDNSERVER_HPP
#define SOCKIDNSERVER_HPP


// Standard libraries
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using OPENIDN_SOCKET = SOCKET;
#else
#include <sys/socket.h>
using OPENIDN_SOCKET = int;
#endif

// Project headers
#include "../server/IDNServer.hpp"



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class SockIDNHelloConnection: public IDNHelloConnection
{
    typedef IDNHelloConnection Inherited;

    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    struct sockaddr_storage clientAddr;             // The network address of the client


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    SockIDNHelloConnection(RECV_COOKIE *cookie, uint8_t clientGroup, char *logIdent);
    virtual ~SockIDNHelloConnection();

    // -- Inherited Members -------------
    virtual int clientMatchIDNHello(RECV_COOKIE *cookie, uint8_t clientGroup);
};



class SockIDNServer: public IDNServer
{
    typedef IDNServer Inherited;

    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    std::atomic<bool> threadStop;
    std::mutex startupMutex;
    std::condition_variable startupCv;
    bool startupFinished = false;
    bool startupSucceeded = false;
    std::string startupError;

    int receiveUDP(ODF_ENV *env, OPENIDN_SOCKET fdSocket, uint32_t usRecvTime);
    int mainNetLoop(ODF_ENV *env, OPENIDN_SOCKET fdSocket);
    void signalStartupSuccess();
    void signalStartupFailure(const char *message);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    // -- Inherited Members -------------
    virtual IDNHelloConnection *createConnection(RECV_COOKIE *cookie, uint8_t clientGroup, char *logIdent);
    virtual ODFSession *createSession(char *logIdent, IDNServer *idnServer);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    SockIDNServer(LLNode<ServiceNode> *firstService);
    virtual ~SockIDNServer();

    void stopServer();
    bool waitUntilStarted(std::chrono::milliseconds timeout, std::string& error);
    void networkThreadFunc();
};


#endif
