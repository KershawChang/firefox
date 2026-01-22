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

class ConnectionAttemptPool final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(ConnectionAttemptPool)

  explicit ConnectionAttemptPool(nsHttpConnectionInfo* info);

  nsresult StartConnectionEstablishment(
      ConnectionEntry* entry, nsAHttpTransaction* trans, uint32_t caps,
      bool speculative, bool urgentStart, bool allow1918,
      PendingTransactionInfo* pendingTransInfo);
  size_t Length() const { return mUnconnectedConns.Length(); }
  void RemoveConnectionAttempt(ConnectionAttempt* attempt, bool abandon);
  void CloseAllConnectionAttempts();
  // calculate the number of half open sockets that have not had at least 1
  // connection complete
  uint32_t UnconnectedConnectionAttempts() const;

  bool FindConnToClaim(PendingTransactionInfo* pendingTransInfo);

  void TimeoutTick();

  void PrintDiagnostics(nsCString& log);

  void GetConnectionData(HttpRetParams& data);

  uint32_t UnconnectedUDPConnsLength() const;

 protected:
  ~ConnectionAttemptPool();

  void InsertIntoConnectionAttempts(ConnectionAttempt* sock);

  RefPtr<nsHttpConnectionInfo> mConnInfo;
  nsTArray<RefPtr<ConnectionAttempt>> mUnconnectedConns;
};

}  // namespace net
}  // namespace mozilla

#endif
