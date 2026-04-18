// -------------------------------------------------------------------------------------------------
//  File IDNServer.cpp
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


#ifndef IDN_SERVER_HPP
#define IDN_SERVER_HPP


// Project headers
#include "idn-hello.h"
#include "LLNode.hpp"
#include "IDNSession.hpp"
#include "IDNService.hpp"



// Forward declarations
class IDNServer;

// Node types (incomplete, not declared)
class ConnectionNode;
class SessionNode;


// -------------------------------------------------------------------------------------------------
//  Defines
// -------------------------------------------------------------------------------------------------

// Shortcuts for struct field sizes
#define UNITID_SIZE sizeof(((IDNHDR_SCAN_RESPONSE *)0)->unitID)
#define HOST_NAME_SIZE sizeof(((IDNHDR_SCAN_RESPONSE *)0)->hostName)


// -------------------------------------------------------------------------------------------------
//  Typedefs
// -------------------------------------------------------------------------------------------------

typedef struct _RECV_COOKIE
{
    // Diagnostics
    char diagString[64];                            // A diagnostic string (optional)

    uint8_t *getSendBuffer(ODF_ENV *env, unsigned sendBufferSize);
    void sendResponse(unsigned sendLen);

} RECV_COOKIE;



// -------------------------------------------------------------------------------------------------
//  Classes
// -------------------------------------------------------------------------------------------------

class ODFSession: public IDNSession, public LLNode<SessionNode>
{
    typedef IDNSession Inherited;

    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    IDNServer *idnServer;                           // The server, this session was created from
    int sessionState;                               // The state of the session

    bool inputTimeValid;                            // Indicates processing of messages
    uint32_t inputTimeUS;                           // Last message reception in microseconds

    unsigned pipelineEvents[IDNVAL_CHANNEL_COUNT];  // Channel/Routing flags (since last acknowledge)


    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    // -- Inherited Members -------------
    virtual void openChannel(ODF_ENV *env, IDN_CHANNEL *channel);
    virtual void closeChannel(ODF_ENV *env, IDN_CHANNEL *channel, bool abortFlag);
    virtual SERVICE_HANDLE requestService(ODF_ENV *env, uint8_t channelID, uint8_t serviceID, uint8_t serviceMode);
    virtual void releaseService(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd);
    virtual CONDUIT_HANDLE requestConduit(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd, uint8_t serviceMode);
    virtual void releaseConduit(ODF_ENV *env, uint8_t channelID, SERVICE_HANDLE serviceHnd, CONDUIT_HANDLE conduitHnd, bool abortFlag);
    virtual void input(ODF_ENV *env, SERVICE_HANDLE serviceHnd, CONDUIT_HANDLE conduitHnd, ODF_TAXI_BUFFER *taxiBuffer);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    ODFSession(char *logIdent, IDNServer *idnServer);
    virtual ~ODFSession();

    virtual uint16_t clearPipelineEvents(unsigned channelID);

    virtual void cancelGracefully(ODF_ENV *env);
    virtual void cancelImmediately(ODF_ENV *env);

    virtual bool checkTeardown(ODF_ENV *env, uint32_t envTimeUS);

    // -- Inherited Members -------------
    virtual void reset(ODF_ENV *env);
    virtual int processChannelMessage(ODF_ENV *env, ODF_TAXI_BUFFER *taxiBuffer);

    // -- Inline Methods ----------------
    int getSessionState() { return sessionState; }
};



class IDNHelloConnection: public LLNode<ConnectionNode>
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    uint8_t clientGroup;                            // The group, the client is running in
    char logIdent[64];                              // A diagnostic ident string for logging
    uint32_t inputTimeUS;                           // Last packet reception in microseconds


    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    ODFSession *session;                            // The session associated with this connection

    virtual void cleanupClose(ODF_ENV *env);
    virtual void cleanupAbort(ODF_ENV *env);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    uint16_t inputEvents;                           // Link/Message flags (since last acknowledge)
    int16_t sequenceValid;                          // Validity of the next sequence number
    uint16_t nextSequence;                          // The next sequence number expected
    unsigned errCntSequence;                        // The number of sequence errors


    IDNHelloConnection(uint8_t clientGroup, char *logIdent);
    virtual ~IDNHelloConnection();

    virtual int clientMatchIDNHello(RECV_COOKIE *cookie, uint8_t clientGroup);

    virtual void handleLinkClose(ODF_ENV *env);
    virtual void handleTimeout(ODF_ENV *env, int32_t usDiffTime);
    virtual void handleAbort(ODF_ENV *env);
    virtual bool checkTeardown(ODF_ENV *env, uint32_t envTimeUS);

    // -- Inline Methods ----------------
    char *getLogIdent() { return logIdent; }
    void setSession(ODFSession *session) { this->session = session; }
    ODFSession *getSession() { return session; }
    void updateInputTime(uint32_t inputTimeUS) { this->inputTimeUS = inputTimeUS; }
};



class IDNServer
{
    // ------------------------------------------ Members ------------------------------------------

    ////////////////////////////////////////////////////////////////////////////////////////////////
    private:

    LLNode<ServiceNode> *firstService;              // List of services
    uint8_t cfgUnitID[UNITID_SIZE];                 // The unitID to report on scan requests
    uint8_t cfgHostName[HOST_NAME_SIZE];            // The host name to report on scan requests

    LLNode<ConnectionNode> *firstConnection;        // List of client connections
    LLNode<SessionNode> *firstSession;              // List of server connections

    uint16_t clientGroupMask;                       // Allowed client groups


    void processStreamMessage(ODF_ENV *env, IDNHelloConnection *connection, ODF_TAXI_BUFFER *taxiBuffer);
    void processRtPacket(ODF_ENV *env, IDNHelloConnection *connection, ODF_TAXI_BUFFER *taxiBuffer);

    void destroyConnection(IDNHelloConnection *connection);
    void destroySession(ODFSession *session);

    IDNHelloConnection *findConnection(ODF_ENV *env, RECV_COOKIE *cookie, uint8_t clientGroup);
    int processRtConnection(ODF_ENV *env, RECV_COOKIE *cookie, IDNHDR_RT_ACKNOWLEDGE *ackRspHdr, int cmd, ODF_TAXI_BUFFER *taxiBuffer);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    protected:

    virtual IDNHelloConnection *createConnection(RECV_COOKIE *cookie, uint8_t clientGroup, char *logIdent) = 0;
    virtual ODFSession *createSession(char *logIdent, IDNServer *idnServer) = 0;

    virtual int checkExcluded(uint8_t flags);
    virtual void processCommand(ODF_ENV *env, RECV_COOKIE *cookie, ODF_TAXI_BUFFER *taxiBuffer);


    ////////////////////////////////////////////////////////////////////////////////////////////////
    public:

    IDNServer(LLNode<ServiceNode> *firstService);
    virtual ~IDNServer();

    virtual void setUnitID(uint8_t *fieldPtr, unsigned fieldSize);
    virtual void getUnitID(uint8_t *fieldPtr, unsigned fieldSize);

    virtual void setHostName(uint8_t *fieldPtr, unsigned fieldSize);
    virtual void getHostName(uint8_t *fieldPtr, unsigned fieldSize);

    virtual IDNService *getService(uint8_t serviceID);
    virtual IDNService *getDefaultService(uint8_t serviceMode);

    virtual void housekeeping(ODF_ENV *env, uint32_t envTimeUS);
};


#endif
