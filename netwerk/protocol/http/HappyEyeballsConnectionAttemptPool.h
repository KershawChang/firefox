/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HappyEyeballsConnectionAttemptPool_h__
#define HappyEyeballsConnectionAttemptPool_h__

#include "ConnectionAttemptPool.h"

namespace mozilla {
namespace net {

class HappyEyeballsConnectionAttemptPool : public ConnectionAttemptPool {
 public:
  explicit HappyEyeballsConnectionAttemptPool(nsHttpConnectionInfo* info);

  nsresult StartConnectionEstablishment(
      ConnectionEntry* entry, nsAHttpTransaction* trans, uint32_t caps,
      bool speculative, bool urgentStart, bool allow1918,
      PendingTransactionInfo* pendingTransInfo) override;
  size_t Length() const override;
  void RemoveConnectionAttempt(ConnectionAttempt* attempt,
                               bool abandon) override;
  void CloseAllConnectionAttempts() override;
  uint32_t UnconnectedConnectionAttempts() const override;
  bool FindConnToClaim(PendingTransactionInfo* pendingTransInfo) override;
  void TimeoutTick() override;
  void PrintDiagnostics(nsCString& log) override {}
  void GetConnectionData(HttpRetParams& data) override {}
  uint32_t UnconnectedUDPConnsLength() const override;

 private:
  ~HappyEyeballsConnectionAttemptPool();
};

}  // namespace net
}  // namespace mozilla

#endif
