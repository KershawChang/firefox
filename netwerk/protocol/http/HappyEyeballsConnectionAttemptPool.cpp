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

#include "HappyEyeballsConnectionAttemptPool.h"
#include "mozilla/net/happy_eyeballs_glue.h"

namespace mozilla::net {

HappyEyeballsConnectionAttemptPool::HappyEyeballsConnectionAttemptPool(
    nsHttpConnectionInfo* info)
    : ConnectionAttemptPool(info) {}

HappyEyeballsConnectionAttemptPool::~HappyEyeballsConnectionAttemptPool() {}

nsresult HappyEyeballsConnectionAttemptPool::StartConnectionEstablishment(
    ConnectionEntry* entry, nsAHttpTransaction* trans, uint32_t caps,
    bool speculative, bool urgentStart, bool allow1918,
    PendingTransactionInfo* pendingTransInfo) {
  LOG(("Creating HappyEyeballs [this=%p trans=%p ent=%s key=%s]\n", this, trans,
       mConnInfo->Origin(), mConnInfo->HashKey().get()));

  RefPtr<HappyEyeballsConnectionAttempt> he =
      new HappyEyeballsConnectionAttempt(mConnInfo, trans, caps, speculative,
                                         urgentStart);

  if (speculative) {
    he->SetAllow1918(allow1918);
  }

  nsresult rv = he->Init(entry);
  if (NS_FAILED(rv)) {
    he->Abandon();
    return rv;
  }

  InsertIntoConnectionAttempts(he);

  if (pendingTransInfo) {
    bool claimed = he->Claim();
    if (!claimed) {
      // We should always be able to claim this.
      return NS_ERROR_UNEXPECTED;
    }
    pendingTransInfo->RememberConnectionAttempt(he);
  }
  return NS_OK;
}

size_t HappyEyeballsConnectionAttemptPool::Length() const { return 0; }

void HappyEyeballsConnectionAttemptPool::RemoveConnectionAttempt(
    ConnectionAttempt* attempt, bool abandon) {
  if (abandon) {
    attempt->Abandon();
  }

  if (mUnconnectedConns.RemoveElement(attempt)) {
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

void HappyEyeballsConnectionAttemptPool::CloseAllConnectionAttempts() {
  for (const auto& conn : mUnconnectedConns) {
    conn->Abandon();
    gHttpHandler->ConnMgr()->DecreaseNumDnsAndConnectSockets();
  }
  mUnconnectedConns.Clear();
  (void)gHttpHandler->ConnMgr()->ProcessPendingQ(mConnInfo);
}

uint32_t HappyEyeballsConnectionAttemptPool::UnconnectedConnectionAttempts()
    const {
  uint32_t unconnectedConns = 0;
  for (uint32_t i = 0; i < mUnconnectedConns.Length(); ++i) {
    if (!mUnconnectedConns[i]->HasConnected()) {
      ++unconnectedConns;
    }
  }
  return unconnectedConns;
}

bool HappyEyeballsConnectionAttemptPool::FindConnToClaim(
    PendingTransactionInfo* pendingTransInfo) {
  nsHttpTransaction* trans = pendingTransInfo->Transaction();
  for (const auto& sock : mUnconnectedConns) {
    if (sock->AcceptsTransaction(trans) && sock->Claim()) {
      pendingTransInfo->RememberConnectionAttempt(sock);
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

void HappyEyeballsConnectionAttemptPool::TimeoutTick() {
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

}  // namespace mozilla::net
