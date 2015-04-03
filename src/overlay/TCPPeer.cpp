// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "main/Application.h"
#include "generated/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/OverlayManager.h"
#include "database/Database.h"
#include "overlay/PeerRecord.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "main/Config.h"

#define MS_TO_WAIT_FOR_HELLO 2000
#define MAX_MESSAGE_SIZE 0x1000000

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<asio::ip::tcp::socket> socket)
    : Peer(app, role)
    , mSocket(socket)
    , mHelloTimer(app)
    , mMessageRead(
          app.getMetrics().NewMeter({"overlay", "message", "read"}, "message"))
    , mMessageWrite(
          app.getMetrics().NewMeter({"overlay", "message", "write"}, "message"))
    , mByteRead(app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte"))
    , mByteWrite(
          app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte"))
{
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, std::string const& ip, uint32_t port)
{
    CLOG(DEBUG, "Overlay") << "TCPPeer:initiate"
                           << "@" << app.getConfig().PEER_PORT << " to " << ip
                           << ":" << port;
    auto socket =
        make_shared<asio::ip::tcp::socket>(app.getClock().getIOService());
    auto result = make_shared<TCPPeer>(
        app, ACCEPTOR,
        socket); // We are initiating; new `newed` TCPPeer is accepting
    result->mIP = ip;
    result->mRemoteListeningPort = port;
    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(ip), port);
    socket->async_connect(endpoint, [result](asio::error_code const& error)
                          {
                              result->connectHandler(error);
                          });
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<asio::ip::tcp::socket> socket)
{
    CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
                           << "@" << app.getConfig().PEER_PORT;
    auto result = make_shared<TCPPeer>(
        app, INITIATOR,
        socket); // We are accepting; new `newed` TCPPeer initiated
    result->mIP = socket->remote_endpoint().address().to_string();
    result->mHelloTimer.expires_from_now(
        std::chrono::milliseconds(MS_TO_WAIT_FOR_HELLO));
    result->mHelloTimer.async_wait([result](asio::error_code const& error)
                                   {
                                       if (!error)
                                           result->timerExpired(error);
                                   });
    result->startRead();
    return result;
}

TCPPeer::~TCPPeer()
{
    //    cout << "TCPPeer::~TCPPeer @" << mApp.getConfig().PEER_PORT;
}

void
TCPPeer::timerExpired(asio::error_code const& error)
{
    drop();
}

std::string
TCPPeer::getIP()
{
    return mIP;
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    // Pass ownership of a serialized XDR message buffer, along with an
    // asio::buffer pointing into it, to the callback for async_write, so it
    // survives as long as the request is in flight in the io_service, and
    // is deallocated when the write completes.
    //
    // The capture of `buf` is required to keep the buffer alive long enough.

    CLOG(DEBUG, "Overlay") << "TCPPeer:sendMessage"
                           << "@" << mApp.getConfig().PEER_PORT << " to "
                           << mRemoteListeningPort;

    auto self = shared_from_this();
    auto buf = std::make_shared<xdr::msg_ptr>(std::move(xdrBytes));
    asio::async_write(
        *(mSocket.get()), asio::buffer((*buf)->raw_data(), (*buf)->raw_size()),
        [self, buf](asio::error_code const& ec, std::size_t length)
        {
            self->writeHandler(ec, length);
            (void)buf;
        });
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred)
{
    if (error)
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer::writeHandler error"
                               << "@" << mApp.getConfig().PEER_PORT << " to "
                               << mRemoteListeningPort;
        drop();
    }
    else
    {
        mMessageWrite.Mark();
        mByteWrite.Mark(bytes_transferred);
    }
}

void
TCPPeer::startRead()
{
    CLOG(DEBUG, "Overlay") << "TCPPeer::startRead"
                           << "@" << mApp.getConfig().PEER_PORT << " to "
                           << mSocket->remote_endpoint().port();

    auto self = shared_from_this();
    asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
                     [self](asio::error_code ec, std::size_t length)
                     {
                         CLOG(DEBUG, "Overlay")
                             << "TCPPeer::startRead calledback " << ec
                             << " length:" << length;
                         self->readHeaderHandler(ec, length);
                     });
}

int
TCPPeer::getIncomingMsgLength()
{
    int length = mIncomingHeader[0];
    length &= 0x7f; // clear the XDR 'continuation' bit
    length <<= 8;
    length |= mIncomingHeader[1];
    length <<= 8;
    length |= mIncomingHeader[2];
    length <<= 8;
    length |= mIncomingHeader[3];
    if (length < 0 || length > MAX_MESSAGE_SIZE)
    {
        CLOG(WARNING, "Overlay")
            << "TCP::Peer::getIncomingMsgLength message size unacceptable: "
            << length;
        drop();
    }
    return (length);
}

void
TCPPeer::connected()
{
    startRead();
}

void
TCPPeer::readHeaderHandler(asio::error_code const& error,
                           std::size_t bytes_transferred)
{
    // LOG(DEBUG) << "TCPPeer::readHeaderHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        mByteRead.Mark(bytes_transferred);
        mIncomingBody.resize(getIncomingMsgLength());
        auto self = shared_from_this();
        asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                         [self](asio::error_code ec, std::size_t length)
                         {
                             self->readBodyHandler(ec, length);
                         });
    }
    else
    {
        CLOG(WARNING, "Overlay") << "readHeaderHandler error: " << error;
        drop();
    }
}

void
TCPPeer::readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred)
{
    // LOG(DEBUG) << "TCPPeer::readBodyHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        mByteRead.Mark(bytes_transferred);
        recvMessage();
        startRead();
    }
    else
    {
        CLOG(WARNING, "Overlay") << "readBodyHandler error: " << error;
        drop();
    }
}

void
TCPPeer::recvMessage()
{
    xdr::xdr_get g(mIncomingBody.data(),
                   mIncomingBody.data() + mIncomingBody.size());
    mMessageRead.Mark();
    StellarMessage sm;
    xdr::xdr_argpack_archive(g, sm);
    Peer::recvMessage(sm);
}

bool
TCPPeer::recvHello(StellarMessage const& msg)
{
    mHelloTimer.cancel();
    if (!Peer::recvHello(msg))
        return false;

    if (mRole == INITIATOR)
    {
        if (!PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(),
                                        getRemoteListeningPort()))
        {
            PeerRecord pr;
            PeerRecord::fromIPPort(getIP(), getRemoteListeningPort(),
                                   mApp.getClock(), pr);
            pr.storePeerRecord(mApp.getDatabase());
        }

        if (mApp.getOverlayManager().isPeerAccepted(shared_from_this()))
        {
            sendHello();
            sendPeers();
        }
        else
        { // we can't accept anymore peer connections
            sendPeers();
            drop();
        }
    }
    else
    { // we called this guy
        // only lower numFailures if we were successful connecting out to him
        auto pr = PeerRecord::loadPeerRecord(mApp.getDatabase(), getIP(),
                                             getRemoteListeningPort());
        if (!pr)
        {
            pr = make_optional<PeerRecord>();
            PeerRecord::fromIPPort(getIP(), getRemoteListeningPort(),
                                   mApp.getClock(), *pr);
        }
        pr->mNumFailures = 0;
        pr->mNextAttempt = mApp.getClock().now();
        pr->storePeerRecord(mApp.getDatabase());
    }
    return true;
}

void
TCPPeer::drop()
{
    if (mState == CLOSING)
    {
        return;
    }
    mState = CLOSING;

    CLOG(DEBUG, "Overlay") << "TCPPeer:drop"
                           << "@" << mApp.getConfig().PEER_PORT << " to "
                           << mRemoteListeningPort;

    auto self = shared_from_this();
    auto sock = mSocket;
    mApp.getClock().getIOService().post(
        [self, sock]()
        {
            self->getApp().getOverlayManager().dropPeer(self);
            try
            {
                sock->shutdown(asio::socket_base::shutdown_both);
            }
            catch (...)
            {
                CLOG(WARNING, "Overlay")
                    << "TCPPeer::drop failed to shutdown socket";
            }
            sock->close();
        });
}
}
