/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ConnectionAttempt.h"
#include "nsHttpTransaction.h"

namespace mozilla::net {

NS_IMPL_ISUPPORTS0(ConnectionAttempt)

bool ConnectionAttempt::AcceptsTransaction(nsHttpTransaction* trans) {
  // When marked as urgent start, only accept urgent start marked transactions.
  // Otherwise, accept any kind of transaction.
  return !mUrgentStart || (trans->Caps() & nsIClassOfService::UrgentStart);
}

void ConnectionAttempt::Unclaim() {
  MOZ_ASSERT(!mSpeculative && !mFreeToUse);
  // We will keep the backup-timer running. Most probably this halfOpen will
  // be used by a transaction from which this transaction took the halfOpen.
  // (this is happening because of the transaction priority.)
  mFreeToUse = true;
}

}  // namespace mozilla::net
