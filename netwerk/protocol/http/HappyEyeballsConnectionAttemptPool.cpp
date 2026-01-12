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

void HappyEyeballsConnectionAttemptPool::CloseAllConnectionAttempts() {}

uint32_t HappyEyeballsConnectionAttemptPool::UnconnectedConnectionAttempts()
    const {
  return 0;
}

bool HappyEyeballsConnectionAttemptPool::FindConnToClaim(
    PendingTransactionInfo* pendingTransInfo) {
  return false;
}

void HappyEyeballsConnectionAttemptPool::TimeoutTick() {}

}  // namespace mozilla::net
