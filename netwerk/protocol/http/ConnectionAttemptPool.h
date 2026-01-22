/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ConnectionAttemptPool_h__
#define ConnectionAttemptPool_h__

#include "ConnectionAttempt.h"
#include "DashboardTypes.h"
#include "nsHashKeys.h"
#include "nsTHashMap.h"
#include "PendingTransactionInfo.h"

namespace mozilla {
namespace net {

class ConnectionEntry;

class ConnectionAttemptPool {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(ConnectionAttemptPool)

  explicit ConnectionAttemptPool(nsHttpConnectionInfo* info);

  virtual nsresult StartConnectionEstablishment(
      ConnectionEntry* entry, nsAHttpTransaction* trans, uint32_t caps,
      bool speculative, bool urgentStart, bool allow1918,
      PendingTransactionInfo* pendingTransInfo);
  virtual size_t Length() const { return mUnconnectedConns.Length(); }
  virtual void RemoveConnectionAttempt(ConnectionAttempt* attempt,
                                       bool abandon);
  virtual void CloseAllConnectionAttempts();
  // calculate the number of half open sockets that have not had at least 1
  // connection complete
  virtual uint32_t UnconnectedConnectionAttempts() const;

  virtual bool FindConnToClaim(PendingTransactionInfo* pendingTransInfo);

  virtual void TimeoutTick();

  virtual void PrintDiagnostics(nsCString& log);

  virtual void GetConnectionData(HttpRetParams& data);

  virtual uint32_t UnconnectedUDPConnsLength() const;

 protected:
  virtual ~ConnectionAttemptPool();

  void InsertIntoConnectionAttempts(ConnectionAttempt* sock);

  RefPtr<nsHttpConnectionInfo> mConnInfo;
  nsTArray<RefPtr<ConnectionAttempt>> mUnconnectedConns;
};

}  // namespace net
}  // namespace mozilla

#endif
