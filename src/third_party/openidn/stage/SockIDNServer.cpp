// -------------------------------------------------------------------------------------------------
//  File SockIDNServer.cpp
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


// Standard libraries
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <chrono>

// Platform includes
#include <errno.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>      /* IP address conversion stuff */
#include <net/if.h>
#include <netdb.h>          /* getnameinfo */
#endif

// Project headers
#include "../shared/ODFTools.hpp"

// Module header
#include "SockIDNServer.hpp"
#if !defined(OPENIDN_BRIDGE_MODE)
#define OPENIDN_BRIDGE_MODE 0
#endif
#if defined(__linux__)
#include <fstream>
#endif



// -------------------------------------------------------------------------------------------------
//  Typedefs
// -------------------------------------------------------------------------------------------------

typedef struct 
{
    RECV_COOKIE cookie;                                 // Must be first element!

    struct sockaddr_storage remoteAddr;
    socklen_t remoteAddrSize;

    OPENIDN_SOCKET fdSocket;
    uint8_t *sendBufferPtr;
    unsigned sendBufferSize;

    // Diagnostics
    char diagString[64];                                // A diagnostic string (optional)

} SOCK_RECV_CONTEXT;



// -------------------------------------------------------------------------------------------------
//  Variables
// -------------------------------------------------------------------------------------------------

static int plt_monoValid = 0;
#if !defined(_WIN32)
static struct timespec plt_monoRef = { 0 };
static uint32_t plt_monoTimeUS = 0;
#endif



// -------------------------------------------------------------------------------------------------
//  Tools
// -------------------------------------------------------------------------------------------------

static int plt_validateMonoTime()
{
#if defined(_WIN32)
    plt_monoValid = 1;
    return 0;
#else
    if(!plt_monoValid)
    {
        // Initialize time reference
        if(clock_gettime(CLOCK_MONOTONIC, &plt_monoRef) < 0) return -1;

        // Initialize internal time randomly
        plt_monoTimeUS = (uint32_t)((plt_monoRef.tv_sec * 1000000ul) + (plt_monoRef.tv_nsec / 1000));
        plt_monoValid = 1;
    }

    return 0;
#endif
}


static uint32_t plt_getMonoTimeUS()
{
#if defined(_WIN32)
    static const auto plt_monoRef = std::chrono::steady_clock::now();
    const auto tsNow = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(tsNow - plt_monoRef).count();
    return static_cast<uint32_t>(elapsed & 0xFFFFFFFFu);
#else
    extern struct timespec plt_monoRef;
    extern uint32_t plt_monoTimeUS;

    // Get current time
    struct timespec tsNow, tsDiff;
    clock_gettime(CLOCK_MONOTONIC, &tsNow);

    // Determine difference to reference time
    if(tsNow.tv_nsec < plt_monoRef.tv_nsec)
    {
        tsDiff.tv_sec = (tsNow.tv_sec - plt_monoRef.tv_sec) - 1;
        tsDiff.tv_nsec = (1000000000 + tsNow.tv_nsec) - plt_monoRef.tv_nsec;
    }
    else
    {
        tsDiff.tv_sec = tsNow.tv_sec - plt_monoRef.tv_sec;
        tsDiff.tv_nsec = tsNow.tv_nsec - plt_monoRef.tv_nsec;
    }

    // Update current time
    plt_monoTimeUS += (uint32_t)((uint64_t)tsDiff.tv_sec * (uint64_t)1000000);
    uint32_t diffMicroInt = tsDiff.tv_nsec / 1000;
    uint32_t diffMicroFrc = tsDiff.tv_nsec % 1000;
    plt_monoTimeUS += diffMicroInt;
    tsDiff.tv_nsec -= diffMicroFrc;

    // Update system time reference. Note: For both (ref and diff) tv_nsec < 1s => sum < 2000000000 (2s)
    plt_monoRef.tv_sec += tsDiff.tv_sec;
    plt_monoRef.tv_nsec += tsDiff.tv_nsec;
    if(plt_monoRef.tv_nsec >= 1000000000)
    {
        plt_monoRef.tv_sec++;
        plt_monoRef.tv_nsec -= 1000000000;
    }

    return plt_monoTimeUS;
#endif
}

static int plt_socketError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int plt_setNonBlocking(OPENIDN_SOCKET fdSocket)
{
#if defined(_WIN32)
    u_long nonBlocking = 1;
    return ioctlsocket(fdSocket, FIONBIO, &nonBlocking);
#else
    return fcntl(fdSocket, F_SETFL, O_NONBLOCK);
#endif
}

static bool plt_socketIsInvalid(OPENIDN_SOCKET fdSocket)
{
#if defined(_WIN32)
    return fdSocket == INVALID_SOCKET;
#else
    return fdSocket < 0;
#endif
}

static void plt_closeSocket(OPENIDN_SOCKET fdSocket)
{
#if defined(_WIN32)
    closesocket(fdSocket);
#else
    close(fdSocket);
#endif
}


static int sockaddr_cmp(struct sockaddr *a, struct sockaddr *b)
{
    if(a->sa_family != b->sa_family) return 1;

    if(a->sa_family == AF_INET)
    {
        struct sockaddr_in *aIn = (struct sockaddr_in *)a;
        struct sockaddr_in *bIn = (struct sockaddr_in *)b;
        if(aIn->sin_port != bIn->sin_port) return 1;
        if(aIn->sin_addr.s_addr != bIn->sin_addr.s_addr) return 1;
    }
    else if(a->sa_family == AF_INET6)
    {
        struct sockaddr_in6 *aIn6 = (struct sockaddr_in6 *)a;
        struct sockaddr_in6 *bIn6 = (struct sockaddr_in6 *)b;
        if(aIn6->sin6_port != bIn6->sin6_port) return 1;
        if(memcmp(aIn6->sin6_addr.s6_addr, bIn6->sin6_addr.s6_addr, sizeof(aIn6->sin6_addr.s6_addr)) != 0) return 1;

        if(aIn6->sin6_flowinfo != bIn6->sin6_flowinfo) return 1;
        if(aIn6->sin6_scope_id != bIn6->sin6_scope_id) return 1;
    }
    else
    {
        return -1;
    }

    return 0;
}

// =================================================================================================
//  Struct RECV_COOKIE
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

uint8_t *RECV_COOKIE::getSendBuffer(ODF_ENV *env, unsigned sendBufferSize)
{
    SOCK_RECV_CONTEXT *rxContext = (SOCK_RECV_CONTEXT *)this;

    if(sendBufferSize > rxContext->sendBufferSize) return (uint8_t *)0;

    return rxContext->sendBufferPtr;
}


void RECV_COOKIE::sendResponse(unsigned sendLen)
{
    SOCK_RECV_CONTEXT *rxContext = (SOCK_RECV_CONTEXT *)this;

    struct sockaddr *sendAddrPtr = (struct sockaddr *)&rxContext->remoteAddr;
#if OPENIDN_BRIDGE_MODE
    socklen_t sendAddrSize = rxContext->remoteAddrSize;
    if(sendAddrSize == 0) sendAddrSize = sizeof(rxContext->remoteAddr);
#else
    socklen_t sendAddrSize = sizeof(rxContext->remoteAddr);
#endif
    sendto(rxContext->fdSocket, (char *)rxContext->sendBufferPtr, sendLen, 0, sendAddrPtr, sendAddrSize);
}



// =================================================================================================
//  class TaxiSource
//
// -------------------------------------------------------------------------------------------------

class TaxiSource: public _ODF_TAXI_SOURCE
{
    friend class SockIDNServer;

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    std::atomic<unsigned> taxiCount;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    TaxiSource()
    {
        taxiCount.store(0);
    }

    virtual ~TaxiSource()
    {
    }

    virtual ODF_TAXI_BUFFER *allocTaxiBuffer(uint16_t payloadLen)
    {
        ODF_TAXI_BUFFER *taxiBuffer = (ODF_TAXI_BUFFER *)malloc(sizeof(ODF_TAXI_BUFFER) + payloadLen);
        if(taxiBuffer != (ODF_TAXI_BUFFER *)0)
        {
            memset(taxiBuffer, 0, sizeof(ODF_TAXI_BUFFER));
            taxiCount++;
        }
        return taxiBuffer;
    }

    virtual void freeTaxiBuffer(ODF_TAXI_BUFFER *taxiBuffer)
    {
        taxiCount--;
        free(taxiBuffer);
    }
};



// =================================================================================================
//  class ODFEnvironment
//
// -------------------------------------------------------------------------------------------------

class ODFEnvironment: public ODF_ENV
{
    friend class SockIDNServer;

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    TaxiSource taxiSource;


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    virtual ~ODFEnvironment()
    {
    }

    virtual void trace(int traceOp, const char *format, va_list ap)
    {
        // Copy arg pointer. Note: This function ends apCopy, the caller ends ap !!
        va_list apCopy;
        va_copy(apCopy, ap);

        if((traceOp == ODF_TRACEOP_LOG_FATAL) || (traceOp == ODF_TRACEOP_LOG_ERROR))
        {
            printf("\x1B[1;31m");
            vprintf(format, apCopy);
            printf("\x1B[0m");
            printf("\n");
            fflush(stdout);
        }
        else if(traceOp == ODF_TRACEOP_LOG_WARN)
        {
            printf("\x1B[1;33m");
            vprintf(format, apCopy);
            printf("\x1B[0m");
            printf("\n");
            fflush(stdout);
        }
        else
        {
            vprintf(format, apCopy);
            printf("\n");
            fflush(stdout);
        }

        va_end(apCopy);
    }

    virtual uint32_t getClockUS()
    {
        return plt_getMonoTimeUS();
    }
};



// =================================================================================================
//  Class SockIDNHelloConnection
//
// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

SockIDNHelloConnection::SockIDNHelloConnection(RECV_COOKIE *cookie, uint8_t clientGroup, char *logIdent):
    IDNHelloConnection(clientGroup, logIdent)
{
    SOCK_RECV_CONTEXT *rxContext = (SOCK_RECV_CONTEXT *)cookie;

    // Copy client address
    memcpy(&(this->clientAddr), &rxContext->remoteAddr, sizeof(this->clientAddr));
}


SockIDNHelloConnection::~SockIDNHelloConnection()
{
}


int SockIDNHelloConnection::clientMatchIDNHello(RECV_COOKIE *cookie, uint8_t clientGroup)
{
    SOCK_RECV_CONTEXT *rxContext = (SOCK_RECV_CONTEXT *)cookie;

    int rcCmp = sockaddr_cmp((struct sockaddr *)&rxContext->remoteAddr, (struct sockaddr *)&clientAddr);
    if(rcCmp != 0) return rcCmp;

    return Inherited::clientMatchIDNHello(cookie, clientGroup);
}




// =================================================================================================
//  Class OpenIDNServer
//
// -------------------------------------------------------------------------------------------------
//  scope: private
// -------------------------------------------------------------------------------------------------

int SockIDNServer::receiveUDP(ODF_ENV *env, OPENIDN_SOCKET fdSocket, uint32_t usRecvTime)
{
    TracePrinter tpr(env, "IDNServer~receiveUDP");

    // Receive the next packet via UDP. Assume error.
    int result = -1;
    ODF_TAXI_BUFFER *taxiBuffer = (ODF_TAXI_BUFFER *)0;
    do
    {
        uint8_t sendBuffer[0x10000];

        // Create/Populate the receive context/cookie
        SOCK_RECV_CONTEXT rxContext;
        RECV_COOKIE *cookie = &rxContext.cookie;
        memset(&rxContext, 0, sizeof(rxContext));
        rxContext.fdSocket = fdSocket;
        rxContext.sendBufferPtr = sendBuffer;
        rxContext.sendBufferSize = sizeof(sendBuffer);

#if OPENIDN_BRIDGE_MODE
        // Read the datagram from the socket first. On some platforms, FIONREAD can
        // race with actual receive length and report stale lengths.
        uint8_t recvBuffer[0x10000];
        socklen_t remoteAddrLen = sizeof(rxContext.remoteAddr);
        struct sockaddr *remoteAddrPtr = (struct sockaddr *)&(rxContext.remoteAddr);
        int recvLen = recvfrom(fdSocket, (char *)recvBuffer, sizeof(recvBuffer), 0, remoteAddrPtr, &remoteAddrLen);
        rxContext.remoteAddrSize = remoteAddrLen;

        // No packet processing in case of errors
        if (recvLen < 0)
        {
#if defined(_WIN32)
            if (plt_socketError() == WSAEWOULDBLOCK)
#else
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN))
#endif
            {
                result = 0;
                break;
            }
            tpr.logError("recv: recvfrom() failed, errno=%d", plt_socketError());
            break;
        }

        // No payload: ignore
        if ((recvLen <= 0) || (recvLen > 0xFFFF))
        {
            result = 0;
            break;
        }

        // Create a taxi buffer matching the actual payload size.
        TaxiSource *taxiSource = &static_cast<ODFEnvironment *>(env)->taxiSource;
        taxiBuffer = taxiSource->allocTaxiBuffer((uint16_t)recvLen);
        if (taxiBuffer == (ODF_TAXI_BUFFER *)0)
        {
            tpr.logError("recv: Out of memory");
            break;
        }

        // Populate taxi buffer and copy payload.
        taxiBuffer->taxiSource = taxiSource;
        taxiBuffer->payloadLen = (uint16_t)recvLen;
        taxiBuffer->payloadPtr = (void *)&taxiBuffer[1];
        taxiBuffer->sourceRefTime = usRecvTime;
        memcpy(taxiBuffer->payloadPtr, recvBuffer, (size_t)recvLen);
#else
        // Get number of octets ready in the input buffer
        int payloadLen;
        if (ioctl(fdSocket, FIONREAD, (char *)&payloadLen, sizeof(payloadLen)) < 0)
        {
            tpr.logError("recv: ioctl(FIONREAD) failed, errno=%d", errno);
            break;
        }

        // Check for invalid receive length (should not happen)
        if ((payloadLen <= 0) || (payloadLen > 0xFFFF))
        {
            // NOP receive, ignore the error (report success)
            tpr.logError("recv: ioctl(FIONREAD) returned invalid datagram length %d", payloadLen);
            recvfrom(fdSocket, sendBuffer, sizeof(sendBuffer), 0, (struct sockaddr *)0, (socklen_t *)0);
            result = 0;
            break;
        }

        // Create a taxi buffer
        TaxiSource *taxiSource = &static_cast<ODFEnvironment *>(env)->taxiSource;
        taxiBuffer = taxiSource->allocTaxiBuffer(payloadLen);
        if (taxiBuffer == (ODF_TAXI_BUFFER *)0)
        {
            tpr.logError("recv: Out of memory");
            break;
        }

        // Populate the taxi buffer. Note: TaxiSource did memset(0) for the header
        taxiBuffer->taxiSource = taxiSource;
        taxiBuffer->payloadLen = (uint16_t)payloadLen;
        taxiBuffer->payloadPtr = (void *)&taxiBuffer[1];
        taxiBuffer->sourceRefTime = usRecvTime;

        // Read the datagram from the socket
        socklen_t remoteAddrLen = sizeof(rxContext.remoteAddr);
        struct sockaddr *remoteAddrPtr = (struct sockaddr *)&(rxContext.remoteAddr);
        int recvLen = recvfrom(fdSocket, (char *)taxiBuffer->payloadPtr, payloadLen, 0, remoteAddrPtr, &remoteAddrLen);

        // No packet processing in case of errors
        if (recvLen < 0)
        {
            tpr.logError("recv: recvfrom() failed, errno=%d", errno);
            break;
        }

        // Check for buffer length and receive length match
        if (payloadLen != recvLen)
        {
            tpr.logError("recv: buffer length / receive length mismatch, %u != %u", payloadLen, recvLen);
            break;
        }
#endif

        // Build readable client name (for diagnostics)
        int rcNameInfo = getnameinfo(remoteAddrPtr, remoteAddrLen,
                                     cookie->diagString, sizeof(cookie->diagString),
                                     NULL, 0, NI_NUMERICHOST);
        if(rcNameInfo != 0)
        {
            tpr.logError("prep: getnameinfo() failed, rc=%d", rcNameInfo);
            break;
        }

        // Append client port to name (for diagnostics)
        unsigned short addrFamily = remoteAddrPtr->sa_family;
        if(addrFamily == AF_INET)
        {
            unsigned nameLen = strlen(cookie->diagString);
            char *bufferPtr = &cookie->diagString[nameLen];
            unsigned bufferSize = sizeof(cookie->diagString) - nameLen;
            struct sockaddr_in *sockAddrIn = (struct sockaddr_in *)remoteAddrPtr;
            snprintf(bufferPtr, bufferSize, ":%u", ntohs(sockAddrIn->sin_port));
        }
        else if(addrFamily == AF_INET6)
        {
            unsigned nameLen = strlen(cookie->diagString);
            char *bufferPtr = &cookie->diagString[nameLen];
            unsigned bufferSize = sizeof(cookie->diagString) - nameLen;
            struct sockaddr_in6 *sockAddrIn6 = (struct sockaddr_in6 *)remoteAddrPtr;
            snprintf(bufferPtr, bufferSize, ":%u", ntohs(sockAddrIn6->sin6_port));
        }
        else
        {
            tpr.logWarn("%s|prep: Unsupported address family", cookie->diagString);
            break;
        }

        // Process the received packet. Note: Buffer has been passed or discarded!
        processCommand(env, cookie, taxiBuffer);
        taxiBuffer = (ODF_TAXI_BUFFER *)0;
        result = 0;
    }
    while (0);

    // Free receive buffer in case allocated
    if(taxiBuffer != (ODF_TAXI_BUFFER *)0) taxiBuffer->discard();
    return result;
}


int SockIDNServer::mainNetLoop(ODF_ENV *env, OPENIDN_SOCKET fdSocket)
{
    TracePrinter tpr(env, "SockIDNServer~mainNetLoop");

    int result = 0;
    while (threadStop.load() == false)
    {
        // Initialize read file descriptor set
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fdSocket, &rfds);

        // Calculate timeout.
        unsigned timeoutMS = 20;
        struct timeval tv;
        tv.tv_sec = timeoutMS / 1000;
        tv.tv_usec = (timeoutMS % 1000) * 1000;

        // Wait for data, remember the ready/reception time
        int numReady = select(
#if defined(_WIN32)
            0,
#else
            fdSocket + 1,
#endif
            &rfds, 0, 0, &tv);
        uint32_t usRecvTime = plt_getMonoTimeUS();
        if(numReady < 0)
        {
            // Error / Interrupt -- report success in case of interrupt
#if defined(_WIN32)
            if (plt_socketError() != WSAEINTR)
#else
            if (errno != EINTR)
#endif
            {
                tpr.logError("select() failed, errno=%d", plt_socketError());
                result = -1;
            }
            break;
        }
        else if(numReady > 0)
        {
            // Receive the packet, terminate in case of errors
            if((result = receiveUDP(env, fdSocket, usRecvTime)) < 0)
            {
                break;
            }
        }

        // Check connections and sessions for timeouts or cleanup (after graceful close)
        // Note: For housekeeping, usTime == 0 issues a shutdown !!
        if(usRecvTime == 0) usRecvTime++;
        housekeeping(env, usRecvTime);
    }

    return result;
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

IDNHelloConnection *SockIDNServer::createConnection(RECV_COOKIE *cookie, uint8_t clientGroup, char *logIdent)
{
    // General note: In case of limited resources, maybe use a statically allocated connections
    return new SockIDNHelloConnection(cookie, clientGroup, logIdent); 
}


ODFSession *SockIDNServer::createSession(char *logIdent, IDNServer *idnServer)
{
    // General note: In case of limited resources, maybe use a statically allocated sessions
    return new ODFSession(logIdent, idnServer);
}


// -------------------------------------------------------------------------------------------------
//  scope: public
// -------------------------------------------------------------------------------------------------

SockIDNServer::SockIDNServer(LLNode<ServiceNode> *firstService):
    IDNServer(firstService)
{
    threadStop.store(false);
}


SockIDNServer::~SockIDNServer()
{
}


void SockIDNServer::stopServer()
{
    threadStop.store(true);
}

bool SockIDNServer::waitUntilStarted(std::chrono::milliseconds timeout, std::string& error)
{
    std::unique_lock<std::mutex> lock(startupMutex);
    if (!startupCv.wait_for(lock, timeout, [this] { return startupFinished; }))
    {
        error = "IDN server startup timed out.";
        return false;
    }

    error = startupError;
    return startupSucceeded;
}

void SockIDNServer::signalStartupSuccess()
{
    {
        std::lock_guard<std::mutex> lock(startupMutex);
        startupFinished = true;
        startupSucceeded = true;
        startupError.clear();
    }
    startupCv.notify_all();
}

void SockIDNServer::signalStartupFailure(const char *message)
{
    {
        std::lock_guard<std::mutex> lock(startupMutex);
        startupFinished = true;
        startupSucceeded = false;
        startupError = message ? message : "IDN server startup failed.";
    }
    startupCv.notify_all();
}


void SockIDNServer::networkThreadFunc()
{
    printf("Starting Network Thread\n");

    ODFEnvironment odfEnv;
    ODF_ENV *env = &odfEnv;
    
#if defined(_WIN32)
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        printf("WSAStartup() failed, errno=%d\n", plt_socketError());
        signalStartupFailure("WSAStartup() failed");
        return;
    }
#endif

    if(plt_validateMonoTime() < 0)
    {
        printf("Cannot initialize monotonic time\n");
        signalStartupFailure("Cannot initialize monotonic time");
#if defined(_WIN32)
        WSACleanup();
#endif
        return;
    }


    // Create UDP socket
    OPENIDN_SOCKET fdSocket;
    if (plt_socketIsInvalid(fdSocket = socket(PF_INET, SOCK_DGRAM, 0)))
    {
        printf("socket() failed, errno=%d\n", plt_socketError());
        signalStartupFailure("socket() failed");
#if defined(_WIN32)
        WSACleanup();
#endif
        return;
    }

    // Set non-blocking
    if (plt_setNonBlocking(fdSocket) < 0)
    {
        printf("Socket non-blocking failed, errno=%d\n", plt_socketError());
        plt_closeSocket(fdSocket);
        signalStartupFailure("Socket non-blocking failed");
#if defined(_WIN32)
        WSACleanup();
#endif
        return;
    }

    // Bind to local port (any interface)
    struct sockaddr_in sockaddr;
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    sockaddr.sin_port = htons(IDNVAL_HELLO_UDP_PORT);
    if (bind(fdSocket, (struct sockaddr *) &sockaddr, sizeof(sockaddr))<0)
    {
        printf("bind() failed, errno=%d\n", plt_socketError());
        plt_closeSocket(fdSocket);
        signalStartupFailure("bind() failed");
#if defined(_WIN32)
        WSACleanup();
#endif
        return;
    }

#if defined(__linux__)
    uint8_t unitID[UNITID_SIZE] = { 0 };
    struct ifconf ifc;
    char buf[1024];

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(fdSocket, SIOCGIFCONF, &ifc) == -1)
    {
        printf("Problem ioctl SIOCGIFCONF\n");
        plt_closeSocket(fdSocket);
        signalStartupFailure("Problem ioctl SIOCGIFCONF");
        return;
    }

    printf("Checking network interfaces ... \n");

    int success = 0;
    struct ifreq ifr;
    struct ifreq* it = ifc.ifc_req;
    const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));
    for (; it != end; ++it)
    {
        strcpy(ifr.ifr_name, it->ifr_name);
        printf("%s: ", it->ifr_name);

        if (ioctl(fdSocket, SIOCGIFFLAGS, &ifr) == 0)
        {
            int not_loopback = ! (ifr.ifr_flags & IFF_LOOPBACK);

            //get IP
            ioctl(fdSocket, SIOCGIFADDR, &ifr);
            printf("inet %s ", inet_ntoa( ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr ));

            //get netmask
            ioctl(fdSocket, SIOCGIFNETMASK, &ifr);
            printf("netmask %s\n", inet_ntoa( ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr ));

            if ( not_loopback ) // don't count loopback
            {
                //get MAC (EUI-48)
                if (ioctl(fdSocket, SIOCGIFHWADDR, &ifr) == 0)
                {
                    success = 1;
                    break;
                }
            }
        }
        else
        {
            printf("Problem enumerate ioctl SIOCGIFHWADDR\n");
            plt_closeSocket(fdSocket);
            signalStartupFailure("Problem enumerate ioctl SIOCGIFHWADDR");
            return;
        }
    }

    unsigned char mac_address[6] = { 0 };
    if (success)
    {
        memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);
    }
    else
    {
        // Backup method of getting MAC address
        std::ifstream file;
        file.open("/sys/class/net/end0/address");
        if (file.is_open())
        {
            char buffer[19] = { 0 };
            file.read(buffer, 18);            
            if (file) 
            {
                sscanf(buffer, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac_address[0], &mac_address[1], &mac_address[2], &mac_address[3], &mac_address[4], &mac_address[5]);
            }
            else 
            {
                printf("Error reading file for MAC address or file has less than 8 bytes\n");
            }   
            file.close();
        }
        else
            printf("Error reading file for MAC address\n");
    }

    printf("MAC address / ether ");
    printf("%02x:", mac_address[0]);
    printf("%02x:", mac_address[1]);
    printf("%02x:", mac_address[2]);
    printf("%02x:", mac_address[3]);
    printf("%02x:", mac_address[4]);
    printf("%02x ", mac_address[5]);
    printf("\n\n");

    unitID[0] = 7;
    unitID[1] = 1;
    unitID[2] = mac_address[0];
    unitID[3] = mac_address[1];
    unitID[4] = mac_address[2];
    unitID[5] = mac_address[3];
    unitID[6] = mac_address[4];
    unitID[7] = mac_address[5];
    //uint8_t hostName[HOST_NAME_SIZE] = { 0 };
    //gethostname((char *)hostName, sizeof(hostName));
    //setHostName(hostName, sizeof(hostName));
    setUnitID(unitID, sizeof(unitID));
#elif OPENIDN_BRIDGE_MODE
    uint8_t unitID[UNITID_SIZE] = { 0 };
    uint8_t hostName[HOST_NAME_SIZE] = { 0 };
    if(gethostname(reinterpret_cast<char *>(hostName), sizeof(hostName) - 1) != 0)
    {
        snprintf(reinterpret_cast<char *>(hostName), sizeof(hostName), "libera-link");
    }

    // Non-Linux fallback: deterministic per-host UnitID.
    uint64_t signature = 1469598103934665603ULL; // FNV-1a offset basis
    for (unsigned i = 0; i < sizeof(hostName) && hostName[i] != 0; ++i)
    {
        signature ^= static_cast<uint64_t>(hostName[i]);
        signature *= 1099511628211ULL;
    }

    unitID[0] = 7;
    unitID[1] = 1;
    unitID[2] = static_cast<uint8_t>((signature >> 40) & 0xFF);
    unitID[3] = static_cast<uint8_t>((signature >> 32) & 0xFF);
    unitID[4] = static_cast<uint8_t>((signature >> 24) & 0xFF);
    unitID[5] = static_cast<uint8_t>((signature >> 16) & 0xFF);
    unitID[6] = static_cast<uint8_t>((signature >> 8) & 0xFF);
    unitID[7] = static_cast<uint8_t>(signature & 0xFF);

    // UnitID must represent a unicast identifier (clear multicast bit).
    unitID[2] &= 0xFE;

    setHostName(hostName, sizeof(hostName));
    setUnitID(unitID, sizeof(unitID));
#endif
    // ---------------------------------------------------------------------------------------------

    signalStartupSuccess();

    // Run main loop
    mainNetLoop(env, fdSocket);

    // ---------------------------------------------------------------------------------------------

    // Abandon remaining clients
    // Note: For housekeeping, usRecvTime == 0 issues a shutdown !!
    housekeeping(env, 0);

    // Close network Socket
    plt_closeSocket(fdSocket);

#if defined(_WIN32)
    WSACleanup();
#endif

    // Print status
    printf("IDN server terminated. Taxi count = %u\n", odfEnv.taxiSource.taxiCount.load());
}
