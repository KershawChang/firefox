/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

// Log on level :5, instead of default :4.
#undef LOG
#define LOG(args) LOG5(args)
#undef LOG_ENABLED
#define LOG_ENABLED() LOG5_ENABLED()

#include "ConnectionAttemptPool.h"
#include "ConnectionEntry.h"
#include "DnsAndConnectSocket.h"
#include "nsHttpHandler.h"

namespace mozilla::net {

ConnectionAttemptPool::ConnectionAttemptPool(nsHttpConnectionInfo* info)
    : mConnInfo(info) {
  LOG(("ConnectionAttemptPool ctor %p", this));
}

ConnectionAttemptPool::~ConnectionAttemptPool() {
  LOG(("ConnectionAttemptPool dtor %p", this));
  MOZ_DIAGNOSTIC_ASSERT(mUnconnectedConns.IsEmpty());
}

nsresult ConnectionAttemptPool::StartConnectionEstablishment(
    ConnectionEntry* entry, nsAHttpTransaction* trans, uint32_t caps,
    bool speculative, bool urgentStart, bool allow1918,
    PendingTransactionInfo* pendingTransInfo) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  MOZ_ASSERT((speculative && !pendingTransInfo) ||
             (!speculative && pendingTransInfo));

  RefPtr<DnsAndConnectSocket> sock =
      new DnsAndConnectSocket(mConnInfo, trans, caps, speculative, urgentStart);

  if (speculative) {
    sock->SetAllow1918(allow1918);
  }

  nsresult rv = sock->Init(entry);
  if (NS_FAILED(rv)) {
    sock->Abandon();
    return rv;
  }

  InsertIntoConnectionAttempts(sock);

  if (pendingTransInfo && sock->Claim()) {
    pendingTransInfo->RememberConnectionAttempt(sock);
  }

  return NS_OK;
}

void ConnectionAttemptPool::InsertIntoConnectionAttempts(
    ConnectionAttempt* sock) {
  mUnconnectedConns.AppendElement(sock);
  gHttpHandler->ConnMgr()->IncreaseNumDnsAndConnectSockets();
}

void ConnectionAttemptPool::RemoveConnectionAttempt(ConnectionAttempt* sock,
                                                    bool abandon) {
  if (abandon) {
    sock->Abandon();
  }

  if (mUnconnectedConns.RemoveElement(sock)) {
    gHttpHandler->ConnMgr()->DecreaseNumDnsAndConnectSockets();
  }

  if (!UnconnectedConnectionAttempts()) {
    // perhaps this reverted RestrictConnections()
    // use the PostEvent version of processpendingq to avoid
    // altering the pending q vector from an arbitrary stack
    nsresult rv = gHttpHandler->ConnMgr()->ProcessPendingQ(mConnInfo);
    if (NS_FAILED(rv)) {
      LOG(
          ("ConnectionAttemptPool::RemoveConnectionAttempt\n"
           "    failed to process pending queue\n"));
    }
  }
}

uint32_t ConnectionAttemptPool::UnconnectedConnectionAttempts() const {
  uint32_t unconnectedConns = 0;
  for (uint32_t i = 0; i < mUnconnectedConns.Length(); ++i) {
    if (!mUnconnectedConns[i]->HasConnected()) {
      ++unconnectedConns;
    }
  }
  return unconnectedConns;
}

void ConnectionAttemptPool::CloseAllConnectionAttempts() {
  for (const auto& sock : mUnconnectedConns) {
    sock->Abandon();
    gHttpHandler->ConnMgr()->DecreaseNumDnsAndConnectSockets();
  }

  mUnconnectedConns.Clear();

  nsresult rv = gHttpHandler->ConnMgr()->ProcessPendingQ(mConnInfo);
  if (NS_FAILED(rv)) {
    LOG(
        ("ConnectionAttemptPool::CloseAllConnectionAttempts\n"
         "    failed to process pending queue\n"));
  }
}

bool ConnectionAttemptPool::FindConnToClaim(
    PendingTransactionInfo* pendingTransInfo) {
  nsHttpTransaction* trans = pendingTransInfo->Transaction();
  for (const auto& sock : mUnconnectedConns) {
    if (sock->AcceptsTransaction(trans) && sock->Claim()) {
      // TODO: hack for now. Do we need remember ConnectionAttempt in
      // pendingTransInfo?
      DnsAndConnectSocket* dnsAndSock = sock->ToDnsAndConnectSocket();
      if (!dnsAndSock) {
        continue;
      }
      pendingTransInfo->RememberConnectionAttempt(dnsAndSock);
      // We've found a speculative connection or a connection that
      // is free to be used in the DnsAndConnectSockets list.
      // A free to be used connection is a connection that was
      // open for a concrete transaction, but that trunsaction
      // ended up using another connection.
      LOG(
          ("ConnectionAttemptPool::FindConnToClaim [ci = %s]\n"
           "Found a speculative or a free-to-use DnsAndConnectSocket\n",
           trans->ConnectionInfo()->HashKey().get()));

      // return OK because we have essentially opened a new connection
      // by converting a speculative DnsAndConnectSockets to general use
      return true;
    }
  }
  return false;
}

void ConnectionAttemptPool::TimeoutTick() {
  if (mUnconnectedConns.IsEmpty()) {
    return;
  }

  TimeStamp currentTime = TimeStamp::Now();
  double maxConnectTime_ms = gHttpHandler->ConnectTimeout();
  for (const auto& sock : Reversed(mUnconnectedConns)) {
    double delta = sock->Duration(currentTime);
    // If the socket has timed out, close it so the waiting
    // transaction will get the proper signal.
    if (delta > maxConnectTime_ms) {
      LOG(("Force timeout of DnsAndConnectSocket to %p after %.2fms.\n",
           sock.get(), delta));
      sock->CloseTransports(NS_ERROR_NET_TIMEOUT);
    }

    // If this DnsAndConnectSocket hangs around for 5 seconds after we've
    // closed() it then just abandon the socket.
    if (delta > maxConnectTime_ms + 5000) {
      LOG(("Abandon DnsAndConnectSocket to %p after %.2fms.\n", sock.get(),
           delta));
      RemoveConnectionAttempt(sock, true);
    }
  }
}

void ConnectionAttemptPool::PrintDiagnostics(nsCString& log) {
  if (mUnconnectedConns.IsEmpty()) {
    return;
  }

  uint32_t count = 0;
  for (const auto& sock : Reversed(mUnconnectedConns)) {
    log.AppendPrintf("   :: Half Open #%u\n", count);
    sock->PrintDiagnostics(log);
    count++;
  }
}

void ConnectionAttemptPool::GetConnectionData(HttpRetParams& data) {
  for (uint32_t i = 0; i < mUnconnectedConns.Length(); i++) {
    DnsAndConnectSockets dnsAndSock{};
    dnsAndSock.speculative = mUnconnectedConns[i]->IsSpeculative();
    data.dnsAndSocks.AppendElement(dnsAndSock);
  }
}

uint32_t ConnectionAttemptPool::UnconnectedUDPConnsLength() const {
  if (!mConnInfo->IsHttp3()) {
    return 0;
  }

  return mUnconnectedConns.Length();
}

}  // namespace mozilla::net
