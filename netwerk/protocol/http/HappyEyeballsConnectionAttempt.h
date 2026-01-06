/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HappyEyeballsConnectionAttempt_h__
#define HappyEyeballsConnectionAttempt_h__

#include "ConnectionAttempt.h"
#include "nsAHttpConnection.h"
#include "nsIDNSListener.h"

namespace mozilla {
namespace net {

class HappyEyeballs;

class HappyEyeballsConnectionAttempt final : public ConnectionAttempt,
                                             public nsIDNSListener,
                                             public nsITimerCallback,
                                             public nsINamed {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIDNSLISTENER
  NS_DECL_NSITIMERCALLBACK
  NS_DECL_NSINAMED

  HappyEyeballsConnectionAttempt(nsHttpConnectionInfo* ci,
                                 nsAHttpTransaction* trans, uint32_t caps,
                                 bool speculative, bool urgentStart);

  nsresult Init(ConnectionEntry* ent);
  void Abandon() override;
  double Duration(TimeStamp epoch) override;
  void CloseTransports(nsresult error) override;

  void PrintDiagnostics(nsCString& log) override;

  // Checks whether the transaction can be dispatched using this
  // half-open's connection.  If this half-open is marked as urgent-start,
  // it only accepts urgent start transactions.  Call only before Claim().
  bool AcceptsTransaction(nsHttpTransaction* trans) override;
  bool Claim() override;
  void Unclaim();

 private:
  ~HappyEyeballsConnectionAttempt();

  nsresult ProcessHappyEyeballsEvents();

  const HappyEyeballs* mHappyEyeballs = nullptr;

  nsCString mHost;
  nsCOMPtr<nsICancelable> mARequest;
  nsCOMPtr<nsICancelable> mAAAARequest;
  nsCOMPtr<nsICancelable> mHTTPSRequest;
  nsCOMPtr<nsIDNSAddrRecord> mARecord;
  nsCOMPtr<nsIDNSAddrRecord> mAAAARecord;
  nsCOMPtr<nsIDNSHTTPSSVCRecord> mHTTPSRecord;
  nsIDNSService::DNSFlags mDnsFlags = nsIDNSService::RESOLVE_DEFAULT_FLAGS;
};

}  // namespace net
}  // namespace mozilla

#endif
