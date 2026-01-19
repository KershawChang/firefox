/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ConnectionAttempt_h__
#define ConnectionAttempt_h__

#include "mozilla/TimeStamp.h"
#include "nsWeakReference.h"

namespace mozilla {
namespace net {

class DnsAndConnectSocket;
class nsAHttpTransaction;
class nsHttpConnectionInfo;
class nsHttpTransaction;

class ConnectionAttempt : public nsSupportsWeakReference {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS

  ConnectionAttempt(nsHttpConnectionInfo* ci, nsAHttpTransaction* trans,
                    uint32_t caps, bool speculative, bool urgentStart)
      : mConnInfo(ci),
        mTransaction(trans),
        mCaps(caps),
        mSpeculative(speculative),
        mUrgentStart(urgentStart) {}

  virtual void Abandon() = 0;
  virtual double Duration(TimeStamp epoch) = 0;
  bool AcceptsTransaction(nsHttpTransaction* trans);
  virtual bool Claim() = 0;
  void Unclaim();
  virtual void CloseTransports(nsresult error) = 0;
  virtual void PrintDiagnostics(nsCString& log) = 0;
  virtual DnsAndConnectSocket* ToDnsAndConnectSocket() { return nullptr; }

  bool IsSpeculative() { return mSpeculative; }
  bool Allow1918() { return mAllow1918; }
  void SetAllow1918(bool val) { mAllow1918 = val; }
  bool HasConnected() { return mHasConnected; }

 protected:
  virtual ~ConnectionAttempt() = default;

  RefPtr<nsHttpConnectionInfo> mConnInfo;
  RefPtr<nsAHttpTransaction> mTransaction;

  uint32_t mCaps = 0;
  // mSpeculative is set if the socket was created from
  // SpeculativeConnect(). It is cleared when a transaction would normally
  // start a new connection from scratch but instead finds this one in
  // the half open list and claims it for its own use. (which due to
  // the vagaries of scheduling from the pending queue might not actually
  // match up - but it prevents a speculative connection from opening
  // more connections that are needed.)
  bool mSpeculative = false;
  // If created with a non-null urgent transaction, remember it, so we can
  // mark the connection as urgent rightaway it's created.
  bool mUrgentStart = false;
  bool mAllow1918 = true;
  // mHasConnected tracks whether one of the sockets has completed the
  // connection process. It may have completed unsuccessfully.
  bool mHasConnected = false;

  // A ConnectionAttempt can be made for a concrete non-null transaction,
  // but the transaction can be dispatch to another connection. In that
  // case we can free this transaction to be claimed by other
  // transactions.
  bool mFreeToUse = true;
};

}  // namespace net
}  // namespace mozilla

#endif
