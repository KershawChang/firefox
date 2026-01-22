/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HappyEyeballsConnectionAttempt_h__
#define HappyEyeballsConnectionAttempt_h__

#include "ConnectionAttempt.h"
#include "nsAHttpConnection.h"
#include "nsIDNSListener.h"
#include "mozilla/Result.h"
#include "mozilla/net/happy_eyeballs_glue.h"
#include "ConnectionEstablisher.h"

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
  bool Claim() override;

 private:
  ~HappyEyeballsConnectionAttempt();

  nsresult ProcessDnsResponseA(const nsACString& aHost,
                               const NetAddr* aAddresses, uint32_t aAddrLen);
  nsresult ProcessDnsResponseAAAA(const nsACString& aHost,
                                  const NetAddr* aAddresses, uint32_t aAddrLen);
  nsresult ProcessDnsResponseHTTPS(const nsACString& aHost,
                                   const ServiceInfoFFI* aServiceInfos,
                                   uint32_t aServiceInfosLen);
  nsresult ProcessConnectionResult(const NetAddr& aAddr, nsresult aStatus);
  nsresult ProcessHappyEyeballsOutput();
  // DNS lookups
  Result<nsIDNSService::DNSFlags, nsresult> SetupDnsFlags(DnsRecordType aType);
  nsresult DNSLookup(DnsRecordType aType, nsIDNSService::DNSFlags aFlags);

  // DNS answers
  nsresult OnARecord(nsIDNSRecord* aRecord, nsresult status);
  nsresult OnAAAARecord(nsIDNSRecord* aRecord, nsresult status);
  nsresult OnHTTPSRecord(nsIDNSRecord* aRecord, nsresult status);

  // Connection Attempt
  nsresult EstablishTCPConnection(NetAddr aAddr, uint16_t aPort);
  void HandleTCPConnectionResult(
      Result<RefPtr<HttpConnectionBase>, nsresult> aResult,
      TCPConnectionEstablisher* aEstablisher);
  void CancelConnection(NetAddr aAddr);
  nsresult EstablishUDPConnection(NetAddr aAddr, uint16_t aPort);
  void HandleUDPConnectionResult(
      Result<RefPtr<HttpConnectionBase>, nsresult> aResult,
      UDPConnectionEstablisher* aEstablisher);

  // Timer
  void SetupTimer(uint64_t aTimeout);

  void OnSucceeded();
  void ProcessTCPConn(nsHttpConnection* aConn, ConnectionEntry* aEntry);
  void ProcessUDPConn(HttpConnectionUDP* aConn, ConnectionEntry* aEntry);

  const HappyEyeballs* mHappyEyeballs = nullptr;

  nsCString mHost;
  nsCOMPtr<nsICancelable> mARequest;
  nsCOMPtr<nsICancelable> mAAAARequest;
  nsCOMPtr<nsICancelable> mHTTPSRequest;
  nsCOMPtr<nsIDNSAddrRecord> mARecord;
  nsCOMPtr<nsIDNSAddrRecord> mAAAARecord;
  nsCOMPtr<nsIDNSHTTPSSVCRecord> mHTTPSRecord;

  nsRefPtrHashtable<NetAddrKey, ConnectionEstablisher>
      mConnectionEstablisherTable;
  RefPtr<HttpConnectionBase> mOutputConn;

  nsCOMPtr<nsITimer> mTimer;
  WeakPtr<ConnectionEntry> mEntry;
  bool mDone = false;
};

}  // namespace net
}  // namespace mozilla

#endif
