/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ConnectionEstablisher_h__
#define ConnectionEstablisher_h__

#include <functional>
#include "ConnectionHandle.h"
#include "mozilla/Result.h"
#include "mozilla/net/DNS.h"
#include "nsAHttpConnection.h"
#include "nsHttpConnection.h"
#include "nsIAsyncOutputStream.h"

namespace mozilla {
namespace net {

class ConnectionEstablisher {
 public:
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

  using DoneCallback =
      std::function<void(Result<RefPtr<HttpConnectionBase>, nsresult>)>;

  ConnectionEstablisher(nsHttpConnectionInfo* aConnInfo, NetAddrKey aAddrKey,
                        uint32_t aCaps);

  virtual bool Start(DoneCallback&& aCallback) = 0;
  virtual void Close(nsresult aReason) = 0;
  const NetAddrKey& AddrKey() const { return mAddrKey; }

 protected:
  virtual ~ConnectionEstablisher();

  virtual void Finish(
      Result<RefPtr<HttpConnectionBase>, nsresult>&& aResult) = 0;
  void SetConnecting();
  void MaybeSetConnectingDone();

  RefPtr<nsHttpConnectionInfo> mConnInfo;
  NetAddrKey mAddrKey;
  nsCOMPtr<nsIDNSAddrRecord> mAddrRecord;
  uint32_t mCaps = 0;
  bool mFinished = false;
  bool mWaitingForConnect = false;
  bool mHasConnected = false;

  DoneCallback mCallback;
  RefPtr<ConnectionHandle> mHandle;
};

class TCPConnectionEstablisher : public ConnectionEstablisher,
                                 public nsIOutputStreamCallback,
                                 public nsITransportEventSink,
                                 public nsIInterfaceRequestor {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIOUTPUTSTREAMCALLBACK
  NS_DECL_NSITRANSPORTEVENTSINK
  NS_DECL_NSIINTERFACEREQUESTOR

  TCPConnectionEstablisher(nsHttpConnectionInfo* aConnInfo, NetAddrKey aAddrKey,
                           uint32_t aCaps, bool aSpeculative, bool aAllow1918);

  // Starts creating the socket transport + streams, and arms AsyncWait.
  // If it fails synchronously, the callback is invoked before returning.
  bool Start(DoneCallback&& aCallback) override;

  void Close(nsresult aReason) override;

 private:
  ~TCPConnectionEstablisher();

  nsresult CreateAndConfigureSocketTransport();
  void Finish(Result<RefPtr<HttpConnectionBase>, nsresult>&& aResult) override;

  TimeStamp mSynStarted;
  bool mSpeculative = false;
  bool mAllow1918 = false;
  bool mConnectedOK = false;

  nsCOMPtr<nsISocketTransport> mSocketTransport;
  nsCOMPtr<nsIAsyncOutputStream> mStreamOut;
  nsCOMPtr<nsIAsyncInputStream> mStreamIn;
};

}  // namespace net
}  // namespace mozilla

#endif
