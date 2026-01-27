/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

#include "HappyEyeballsConnectionAttempt.h"
#include "mozilla/UniquePtr.h"

// Log on level :5, instead of default :4.
#undef LOG
#define LOG(args) LOG5(args)
#undef LOG_ENABLED
#define LOG_ENABLED() LOG5_ENABLED()

namespace mozilla::net {

NS_IMPL_ADDREF_INHERITED(HappyEyeballsConnectionAttempt, ConnectionAttempt)
NS_IMPL_RELEASE_INHERITED(HappyEyeballsConnectionAttempt, ConnectionAttempt)

NS_INTERFACE_MAP_BEGIN(HappyEyeballsConnectionAttempt)
  NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
  NS_INTERFACE_MAP_ENTRY(nsITimerCallback)
  NS_INTERFACE_MAP_ENTRY(nsINamed)
  NS_INTERFACE_MAP_ENTRY(nsIDNSListener)
NS_INTERFACE_MAP_END

HappyEyeballsConnectionAttempt::HappyEyeballsConnectionAttempt(
    nsHttpConnectionInfo* ci, nsAHttpTransaction* trans, uint32_t caps,
    bool speculative, bool urgentStart)
    : ConnectionAttempt(ci, trans, caps, speculative, urgentStart) {
  LOG(("HappyEyeballsConnectionAttempt ctor %p", this));
  if (mConnInfo->GetRoutedHost().IsEmpty()) {
    mHost = mConnInfo->GetOrigin();
    nsTArray<AltSvc> emptyAltSvc;
    (void)happy_eyeballs_new(&mHappyEyeballs, &mHost,
                             static_cast<uint16_t>(mConnInfo->OriginPort()),
                             &emptyAltSvc);
  } else {
    mHost = mConnInfo->GetRoutedHost();
    if (mConnInfo->IsHttp3()) {
      LOG(("HappyEyeballsConnectionAttempt for HTTP/3"));
      nsTArray<AltSvc> altSvcArray;
      AltSvc altsvc{};
      altsvc.protocol = Protocol::H3;
      altSvcArray.AppendElement(altsvc);
      (void)happy_eyeballs_new(&mHappyEyeballs, &mHost,
                               static_cast<uint16_t>(mConnInfo->RoutedPort()),
                               &altSvcArray);
    } else {
      nsTArray<AltSvc> emptyAltSvc;
      (void)happy_eyeballs_new(&mHappyEyeballs, &mHost,
                               static_cast<uint16_t>(mConnInfo->RoutedPort()),
                               &emptyAltSvc);
    }
  }
}

HappyEyeballsConnectionAttempt::~HappyEyeballsConnectionAttempt() {
  LOG(("HappyEyeballsConnectionAttempt dtor %p", this));
  if (mHappyEyeballs) {
    happy_eyeballs_release(mHappyEyeballs);
    mHappyEyeballs = nullptr;
  }
}

nsresult HappyEyeballsConnectionAttempt::Init(ConnectionEntry* ent) {
  mEntry = ent;
  return ProcessHappyEyeballsOutput();
}

static Result<NetAddr, nsresult> ToNetAddr(const nsTArray<uint8_t>& aData,
                                           uint16_t aPort) {
  NetAddr addr;
  if (NS_FAILED(addr.InitFromString(nsDependentCSubstring(
          reinterpret_cast<const char*>(aData.Elements()), aData.Length())))) {
    return Err(NS_ERROR_UNEXPECTED);
  }

  uint16_t port = htons(aPort);
  if (addr.raw.family == AF_INET) {
    addr.inet.port = port;
  } else if (addr.raw.family == AF_INET6) {
    addr.inet6.port = port;
  }

  return addr;
}

nsresult HappyEyeballsConnectionAttempt::ProcessDnsResponseA(
    const nsACString& aHost, const nsTArray<NetAddr>& aAddresses) {
  LOG(("HappyEyeballsConnectionAttempt::ProcessDnsResponseA %p", this));

  nsresult rv = happy_eyeballs_process_dns_response_a(
      const_cast<HappyEyeballs*>(mHappyEyeballs), &aHost, &aAddresses);
  if (NS_FAILED(rv)) {
    LOG(("process_dns_response_a failed rv=%x", static_cast<uint32_t>(rv)));
  }
  return rv;
}

nsresult HappyEyeballsConnectionAttempt::ProcessDnsResponseAAAA(
    const nsACString& aHost, const nsTArray<NetAddr>& aAddresses) {
  LOG(("HappyEyeballsConnectionAttempt::ProcessDnsResponseAAAA %p", this));

  nsresult rv = happy_eyeballs_process_dns_response_aaaa(
      const_cast<HappyEyeballs*>(mHappyEyeballs), &aHost, &aAddresses);
  if (NS_FAILED(rv)) {
    LOG(("process_dns_response_aaaa failed rv=%x", static_cast<uint32_t>(rv)));
  }
  return rv;
}

nsresult HappyEyeballsConnectionAttempt::ProcessDnsResponseHTTPS(
    const nsACString& aHost, const nsTArray<ServiceInfoFFI>& aServiceInfos) {
  LOG(("HappyEyeballsConnectionAttempt::ProcessDnsResponseHTTPS %p", this));

  nsresult rv = happy_eyeballs_process_dns_response_https(
      const_cast<HappyEyeballs*>(mHappyEyeballs), &aHost, &aServiceInfos);
  if (NS_FAILED(rv)) {
    LOG(("process_dns_response_https failed rv=%x", static_cast<uint32_t>(rv)));
  }
  return rv;
}

nsresult HappyEyeballsConnectionAttempt::ProcessConnectionResult(
    const NetAddr& aAddr, nsresult aStatus) {
  LOG(("HappyEyeballsConnectionAttempt::ProcessConnectionResult %p", this));

  nsresult rv = happy_eyeballs_process_connection_result(
      const_cast<HappyEyeballs*>(mHappyEyeballs), &aAddr, aStatus);
  if (NS_FAILED(rv)) {
    LOG(("process_connection_result failed rv=%x", static_cast<uint32_t>(rv)));
  }
  return ProcessHappyEyeballsOutput();
}

nsresult HappyEyeballsConnectionAttempt::ProcessHappyEyeballsOutput() {
  LOG(("HappyEyeballsConnectionAttempt::ProcessHappyEyeballsOutput %p", this));

  if (mDone) {
    return NS_OK;
  }

  nsresult rv = NS_OK;

  while (true) {
    HappyEyeballsEvent event{};
    nsTArray<uint8_t> heData;
    rv = happy_eyeballs_process_output(
        const_cast<HappyEyeballs*>(mHappyEyeballs), &event, &heData);
    if (NS_FAILED(rv)) {
      LOG(("process_output failed rv=%x", static_cast<uint32_t>(rv)));
      return rv;
    }

    LOG(("event.tag=%d", event.tag));
    switch (event.tag) {
      case HappyEyeballsEvent::Tag::SendDnsQuery: {
        LOG(("HappyEyeballsEvent::Tag::SendDnsQuery"));
        auto dnsFlags = SetupDnsFlags(event.send_dns_query.record_type);
        if (dnsFlags.isOk()) {
          rv = DNSLookup(event.send_dns_query.record_type, dnsFlags.unwrap());
          if (NS_FAILED(rv)) {
            Abandon();
            return rv;
          }
        }
        break;
      }

      case HappyEyeballsEvent::Tag::Timer: {
        SetupTimer(event.timer.duration_ms);
        return NS_OK;
      }

      case HappyEyeballsEvent::Tag::AttemptConnection: {
        LOG(
            ("HappyEyeballsEvent::Tag::AttemptConnection protocol=%d port=%d "
             "addr_len=%u ech_config_len=%u",
             event.attempt_connection.protocol, event.attempt_connection.port,
             event.attempt_connection.addr_len,
             event.attempt_connection.ech_config_len));

        nsTArray<uint8_t> addrData;
        addrData.AppendElements(heData.Elements(),
                                event.attempt_connection.addr_len);
        auto res = ToNetAddr(addrData, event.attempt_connection.port);
        if (res.isErr()) {
          LOG(("Failed to convert to NetAddr"));
          // TODO: how to handle this error?
          return res.unwrapErr();
        }

        nsTArray<uint8_t> echConfig;
        if (event.attempt_connection.ech_config_len > 0) {
          echConfig.AppendElements(
              heData.Elements() + event.attempt_connection.addr_len,
              event.attempt_connection.ech_config_len);
        }

        LOG(("connect to:[%s] ech_config_len=%zu",
             res.unwrap().ToString().get(), echConfig.Length()));
        if (event.attempt_connection.protocol == ProtocolCombination::H3) {
          EstablishUDPConnection(res.unwrap(), event.attempt_connection.port,
                                 std::move(echConfig));
        } else {
          EstablishTCPConnection(res.unwrap(), event.attempt_connection.port,
                                 std::move(echConfig));
        }
        break;
      }

      case HappyEyeballsEvent::Tag::CancelConnection: {
        auto res = ToNetAddr(heData, event.attempt_connection.port);
        if (res.isErr()) {
          LOG(("Failed to convert to NetAddr"));
          // TODO: how to handle this error?
          return res.unwrapErr();
        }

        LOG(("CancelConnection:[%s]", res.unwrap().ToString().get()));
        CancelConnection(res.unwrap());
        break;
      }

      case HappyEyeballsEvent::Tag::Succeeded:
        LOG(("HappyEyeballsEvent::Tag::Succeeded"));
        OnSucceeded();
        return NS_OK;

      case HappyEyeballsEvent::Tag::Failed:
        LOG(("HappyEyeballsEvent::Tag::Failed"));
        Abandon();
        return NS_ERROR_CONNECTION_REFUSED;

      case HappyEyeballsEvent::Tag::None:
        LOG(("HappyEyeballsEvent::Tag::None"));
        // No more events to process
        return NS_OK;
    }
  }

  return rv;
}

Result<nsIDNSService::DNSFlags, nsresult>
HappyEyeballsConnectionAttempt::SetupDnsFlags(DnsRecordType aType) {
  LOG(("HappyEyeballsConnectionAttempt::SetupDnsFlags [this=%p aType=%d] ",
       this, aType));

  nsIDNSService::DNSFlags dnsFlags = nsIDNSService::RESOLVE_DEFAULT_FLAGS;

  if (mCaps & NS_HTTP_REFRESH_DNS) {
    dnsFlags = nsIDNSService::RESOLVE_BYPASS_CACHE;
  }

  switch (aType) {
    case DnsRecordType::Https:
      dnsFlags |= nsIDNSService::GetFlagsFromTRRMode(mConnInfo->GetTRRMode());
      return dnsFlags;
    case DnsRecordType::Aaaa:
      if (mCaps & NS_HTTP_DISABLE_IPV6) {
        return Err(NS_ERROR_NOT_AVAILABLE);
      }
      dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV4;
      break;
    case DnsRecordType::A:
      if (mCaps & NS_HTTP_DISABLE_IPV4) {
        return Err(NS_ERROR_NOT_AVAILABLE);
      }
      dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV6;
      break;
  }

  // TODO: let the state machine know the preference
  /*if (ent->PreferenceKnown()) {
    if (ent->mPreferIPv6) {
      dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV4;
    } else if (ent->mPreferIPv4) {
      dnsFlags |= nsIDNSService::RESOLVE_DISABLE_IPV6;
    }
  }*/

  // Deal with IP hints later
  /*if (ent->mConnInfo->HasIPHintAddress()) {
    nsresult rv;
    nsCOMPtr<nsIDNSService> dns;
    dns = mozilla::components::DNS::Service(&rv);
    if (NS_FAILED(rv)) {
      return rv;
    }

    // The spec says: "If A and AAAA records for TargetName are locally
    // available, the client SHOULD ignore these hints.", so we check if the DNS
    // record is in cache before setting USE_IP_HINT_ADDRESS.
    nsCOMPtr<nsIDNSRecord> record;
    rv = dns->ResolveNative(
        mPrimaryTransport.mHost, nsIDNSService::RESOLVE_OFFLINE,
        mConnInfo->GetOriginAttributes(), getter_AddRefs(record));
    if (NS_FAILED(rv) || !record) {
      LOG(("Setting Socket to use IP hint address"));
      dnsFlags |= nsIDNSService::RESOLVE_IP_HINT;
    }
  }*/

  dnsFlags |=
      nsIDNSService::GetFlagsFromTRRMode(NS_HTTP_TRR_MODE_FROM_FLAGS(mCaps));

  // When we get here, we are not resolving using any configured proxy likely
  // because of individual proxy setting on the request or because the host is
  // excluded from proxying.  Hence, force resolution despite global proxy-DNS
  // configuration.
  dnsFlags |= nsIDNSService::RESOLVE_IGNORE_SOCKS_DNS;

  NS_ASSERTION(!(dnsFlags & nsIDNSService::RESOLVE_DISABLE_IPV6) ||
                   !(dnsFlags & nsIDNSService::RESOLVE_DISABLE_IPV4),
               "Setting both RESOLVE_DISABLE_IPV6 and RESOLVE_DISABLE_IPV4");

  LOG(("dnsFlags=%u", dnsFlags));
  return dnsFlags;
}

nsresult HappyEyeballsConnectionAttempt::DNSLookup(
    DnsRecordType aType, nsIDNSService::DNSFlags aFlags) {
  nsCOMPtr<nsIDNSService> dns = GetOrInitDNSService();
  if (!dns) {
    return NS_ERROR_UNEXPECTED;
  }

  nsresult rv = NS_OK;
  switch (aType) {
    case DnsRecordType::Https: {
      if (mCaps & NS_HTTP_DISALLOW_HTTPS_RR) {
        rv = NS_ERROR_NOT_AVAILABLE;
      } else {
        nsCOMPtr<nsIDNSAdditionalInfo> info;
        if (mConnInfo->OriginPort() != NS_HTTPS_DEFAULT_PORT) {
          dns->NewAdditionalInfo(""_ns, mConnInfo->OriginPort(),
                                 getter_AddRefs(info));
        }
        rv = dns->AsyncResolveNative(
            mHost, nsIDNSService::RESOLVE_TYPE_HTTPSSVC,
            aFlags | nsIDNSService::RESOLVE_WANT_RECORD_ON_ERROR, info, this,
            gSocketTransportService, mConnInfo->GetOriginAttributes(),
            getter_AddRefs(mHTTPSRequest));
      }
      break;
    }
    case DnsRecordType::Aaaa:
      rv = dns->AsyncResolveNative(
          mHost, nsIDNSService::RESOLVE_TYPE_DEFAULT,
          aFlags | nsIDNSService::RESOLVE_WANT_RECORD_ON_ERROR, nullptr, this,
          gSocketTransportService, mConnInfo->GetOriginAttributes(),
          getter_AddRefs(mAAAARequest));
      break;
    case DnsRecordType::A:
      rv = dns->AsyncResolveNative(
          mHost, nsIDNSService::RESOLVE_TYPE_DEFAULT,
          aFlags | nsIDNSService::RESOLVE_WANT_RECORD_ON_ERROR, nullptr, this,
          gSocketTransportService, mConnInfo->GetOriginAttributes(),
          getter_AddRefs(mARequest));
      break;
  }

  if (NS_FAILED(rv)) {
    // TODO: we should notify the DNS response synchronously.
    NS_DispatchToCurrentThread(NS_NewRunnableFunction(
        "HappyEyeballsConnectionAttempt::DNSLookup",
        [self = RefPtr{this}, aType]() {
          switch (aType) {
            case DnsRecordType::Https:
              (void)self->OnHTTPSRecord(nullptr, NS_ERROR_UNKNOWN_HOST);
              break;
            case DnsRecordType::Aaaa:
              (void)self->OnAAAARecord(nullptr, NS_ERROR_UNKNOWN_HOST);
              break;
            case DnsRecordType::A:
              (void)self->OnARecord(nullptr, NS_ERROR_UNKNOWN_HOST);
              break;
          }
        }));
  }

  return NS_OK;
}

void HappyEyeballsConnectionAttempt::HandleTCPConnectionResult(
    Result<RefPtr<HttpConnectionBase>, nsresult> aResult,
    TCPConnectionEstablisher* aEstablisher) {
  RefPtr<TCPConnectionEstablisher> establisher = aEstablisher;
  mConnectionEstablisherTable.Remove(establisher->AddrKey());
  NetAddr addr = establisher->AddrKey().mAddr;

  LOG(
      ("HappyEyeballsConnectionAttempt::HandleTCPConnectionResult %p addr=[%s] "
       "family=[%d]",
       this, addr.ToString().get(), addr.raw.family));

  if (aResult.isErr()) {
    // TODO: notify the state machine result
    establisher->Close(aResult.unwrapErr());
    ProcessConnectionResult(addr, aResult.unwrapErr());
    return;
  }

  if (mDone) {
    // Should we use another error code?
    // How should we notify the state machine?
    establisher->Close(NS_BASE_STREAM_CLOSED);
    ProcessConnectionResult(addr, NS_BASE_STREAM_CLOSED);
    return;
  }

  mOutputConn = aResult.unwrap();
  // The ownership of connection is moved to HappyEyeballsConnectionAttempt now.
  establisher->ClearResultConnection();

  ProcessConnectionResult(addr, NS_OK);
}

nsresult HappyEyeballsConnectionAttempt::EstablishTCPConnection(
    NetAddr aAddr, uint16_t aPort, nsTArray<uint8_t>&& aEchConfig) {
  NetAddrKey key(aAddr);
  // TODO: we always use ProtocolCombination::H2OrH1 for now. Do we really want
  // to race H2 and H1?
  RefPtr<nsHttpConnectionInfo> info =
      mConnInfo->CloneAndAdoptPortAndAlpn(aPort, ProtocolCombination::H2OrH1);
  if (!aEchConfig.IsEmpty()) {
    info->SetEchConfig(
        nsCString((const char*)aEchConfig.Elements(), aEchConfig.Length()));
  }
  RefPtr<TCPConnectionEstablisher> establisher =
      new TCPConnectionEstablisher(info, key, mCaps, mSpeculative, mAllow1918);
  auto callback = [self = RefPtr{this}, establisher](
                      Result<RefPtr<HttpConnectionBase>, nsresult> aResult) {
    self->HandleTCPConnectionResult(std::move(aResult), establisher);
  };

  if (establisher->Start(std::move(callback))) {
    mConnectionEstablisherTable.InsertOrUpdate(key, std::move(establisher));
  } else {
    ProcessConnectionResult(aAddr, NS_ERROR_FAILURE);
  }

  return NS_OK;
}

nsresult HappyEyeballsConnectionAttempt::EstablishUDPConnection(
    NetAddr aAddr, uint16_t aPort, nsTArray<uint8_t>&& aEchConfig) {
  NetAddrKey key(aAddr);
  RefPtr<nsHttpConnectionInfo> info =
      mConnInfo->CloneAndAdoptPortAndAlpn(aPort, ProtocolCombination::H3);
  if (!aEchConfig.IsEmpty()) {
    info->SetEchConfig(
        nsCString((const char*)aEchConfig.Elements(), aEchConfig.Length()));
  }
  RefPtr<UDPConnectionEstablisher> establisher =
      new UDPConnectionEstablisher(info, key, mCaps);
  auto callback = [self = RefPtr{this}, establisher](
                      Result<RefPtr<HttpConnectionBase>, nsresult> aResult) {
    self->HandleUDPConnectionResult(std::move(aResult), establisher);
  };

  if (establisher->Start(std::move(callback))) {
    mConnectionEstablisherTable.InsertOrUpdate(key, std::move(establisher));
  } else {
    ProcessConnectionResult(aAddr, NS_ERROR_FAILURE);
  }

  return NS_OK;
}

void HappyEyeballsConnectionAttempt::HandleUDPConnectionResult(
    Result<RefPtr<HttpConnectionBase>, nsresult> aResult,
    UDPConnectionEstablisher* aEstablisher) {
  RefPtr<UDPConnectionEstablisher> establisher = aEstablisher;
  mConnectionEstablisherTable.Remove(establisher->AddrKey());
  NetAddr addr = establisher->AddrKey().mAddr;

  LOG(
      ("HappyEyeballsConnectionAttempt::HandleUDPConnectionResult %p addr=[%s] "
       "family=[%d]",
       this, addr.ToString().get(), addr.raw.family));

  if (aResult.isErr()) {
    establisher->Close(aResult.unwrapErr());
    ProcessConnectionResult(addr, aResult.unwrapErr());
    return;
  }

  if (mDone) {
    // Should we use another error code?
    // How should we notify the state machine?
    establisher->Close(NS_BASE_STREAM_CLOSED);
    ProcessConnectionResult(addr, NS_BASE_STREAM_CLOSED);
    return;
  }

  mOutputConn = aResult.unwrap();
  // The ownership of connection is moved to HappyEyeballsConnectionAttempt now.
  establisher->ClearResultConnection();

  ProcessConnectionResult(addr, NS_OK);
}

void HappyEyeballsConnectionAttempt::CancelConnection(NetAddr aAddr) {
  NetAddrKey key(aAddr);
  ConnectionEstablisher* conn = mConnectionEstablisherTable.GetWeak(key);
  if (conn) {
    conn->Close(NS_ERROR_ABORT);
    mConnectionEstablisherTable.Remove(key);
  }
}

void HappyEyeballsConnectionAttempt::Abandon() {
  LOG(("HappyEyeballsConnectionAttempt::Abandon %p", this));
  mDone = true;

  auto cancelAndClear = [](nsCOMPtr<nsICancelable>&& aRequest) {
    if (aRequest) {
      aRequest->Cancel(NS_ERROR_ABORT);
      aRequest = nullptr;
    }
  };

  cancelAndClear(std::move(mARequest));
  cancelAndClear(std::move(mAAAARequest));
  cancelAndClear(std::move(mHTTPSRequest));

  // Collect all connection establishers into a temporary array to avoid
  // iterator invalidation when Close() triggers callbacks that modify the table
  nsTArray<RefPtr<ConnectionEstablisher>> establishers;
  for (auto iter = mConnectionEstablisherTable.Iter(); !iter.Done();
       iter.Next()) {
    establishers.AppendElement(iter.Data());
  }
  mConnectionEstablisherTable.Clear();

  // Now close all the connections without worrying about iterator invalidation
  for (auto& conn : establishers) {
    conn->Close(NS_ERROR_ABORT);
  }

  if (mTimer) {
    mTimer->Cancel();
  }
  mTimer = nullptr;

  mEntry = nullptr;
}

void HappyEyeballsConnectionAttempt::ProcessTCPConn(nsHttpConnection* aConn,
                                                    ConnectionEntry* aEntry) {
  RefPtr<ConnectionEntry> entry(mEntry);
  if (!entry) {
    return;
  }

  RefPtr<nsHttpConnection> connTCP = aConn;
  LOG(("Got connTCP:%p", connTCP.get()));

  entry->InsertIntoActiveConns(connTCP);

  RefPtr<PendingTransactionInfo> pendingTransInfo =
      gHttpHandler->ConnMgr()->FindTransactionHelper(true, entry, mTransaction);
  bool isHttp2 = connTCP->UsingSpdy();
  if (pendingTransInfo) {
    MOZ_ASSERT(!mSpeculative, "Speculative Half Open found mTransaction");
    nsresult rv = gHttpHandler->ConnMgr()->DispatchTransaction(
        entry, pendingTransInfo->Transaction(), connTCP);
    if (NS_FAILED(rv)) {
      mTransaction->Close(rv);
    }
  } else if (!isHttp2) {
    // After about 1 second allow for the possibility of restarting a
    // transaction due to server close. Keep at sub 1 second as that is the
    // minimum granularity we can expect a server to be timing out with.
    connTCP->SetIsReusedAfter(950);

    LOG(
        ("ProcessTCPConn no transaction match "
         "returning conn %p to pool\n",
         connTCP.get()));
    gHttpHandler->ConnMgr()->OnMsgReclaimConnection(connTCP);
  }

  connTCP->SetIsRacing(false);
  if (isHttp2) {
    gHttpHandler->ConnMgr()->ReportSpdyConnection(
        connTCP, true, (mCaps & NS_HTTP_DISALLOW_HTTP3));
  } else {
    gHttpHandler->ConnMgr()->ReportSpdyConnection(connTCP, false, false);
  }
}

void HappyEyeballsConnectionAttempt::ProcessUDPConn(HttpConnectionUDP* aConn,
                                                    ConnectionEntry* aEntry) {
  RefPtr<ConnectionEntry> entry(mEntry);
  if (!entry) {
    return;
  }

  LOG(("Got connUDP:%p", aConn));

  entry->InsertIntoActiveConns(aConn);

  RefPtr<PendingTransactionInfo> pendingTransInfo =
      gHttpHandler->ConnMgr()->FindTransactionHelper(true, entry, mTransaction);
  nsresult rv = NS_OK;
  if (pendingTransInfo) {
    MOZ_ASSERT(!mSpeculative, "Speculative Half Open found mTransaction");
    rv = gHttpHandler->ConnMgr()->DispatchTransaction(
        entry, pendingTransInfo->Transaction(), aConn);
    if (NS_FAILED(rv)) {
      mTransaction->Close(rv);
    }
  } else {
    rv = aConn->Activate(mTransaction, mCaps, 0);
  }

  aConn->SetIsRacing(false);
  gHttpHandler->ConnMgr()->ReportHttp3Connection(aConn, entry);
}

void HappyEyeballsConnectionAttempt::OnSucceeded() {
  LOG(("HappyEyeballsConnectionAttempt::OnSucceeded %p", this));

  MOZ_ASSERT(!mDone);
  mDone = true;

  RefPtr<HappyEyeballsConnectionAttempt> self(this);
  RefPtr<ConnectionEntry> entry(mEntry);
  MOZ_ASSERT(entry);

  RefPtr<nsHttpConnection> connTCP = do_QueryObject(mOutputConn);
  if (connTCP) {
    ProcessTCPConn(connTCP, entry);
  } else {
    RefPtr<HttpConnectionUDP> connUDP = do_QueryObject(mOutputConn);
    ProcessUDPConn(connUDP, entry);
  }

  mOutputConn = nullptr;

  // Make sure everything is released.
  Abandon();

  entry->RemoveConnectionAttempt(this, false);
}

double HappyEyeballsConnectionAttempt::Duration(TimeStamp epoch) { return 0; }

void HappyEyeballsConnectionAttempt::CloseTransports(nsresult error) {}

void HappyEyeballsConnectionAttempt::PrintDiagnostics(nsCString& log) {}

uint32_t HappyEyeballsConnectionAttempt::UnconnectedUDPConnsLength() const {
  uint32_t len = 0;
  for (auto iter = mConnectionEstablisherTable.ConstIter(); !iter.Done();
       iter.Next()) {
    if (iter.Data()->IsUDP()) {
      len++;
    }
  }
  return len;
}

bool HappyEyeballsConnectionAttempt::Claim() {
  if (mSpeculative) {
    mSpeculative = false;
    mAllow1918 = true;
    for (auto iter = mConnectionEstablisherTable.Iter(); !iter.Done();
         iter.Next()) {
      RefPtr<ConnectionEstablisher> conn = iter.Data();
      conn->ResetSpeculativeFlags();
    }
  }

  if (mFreeToUse) {
    mFreeToUse = false;
    // TODO: we should claim the socket transport
    return true;
  }

  return false;
}

NS_IMETHODIMP
HappyEyeballsConnectionAttempt::OnLookupComplete(nsICancelable* request,
                                                 nsIDNSRecord* rec,
                                                 nsresult status) {
  LOG(("HappyEyeballsConnectionAttempt::OnLookupComplete"));
  if (request && request == mARequest) {
    mARequest = nullptr;
    return OnARecord(rec, status);
  }

  if (request && request == mAAAARequest) {
    mAAAARequest = nullptr;
    return OnAAAARecord(rec, status);
  }

  if (request && request == mHTTPSRequest) {
    mHTTPSRequest = nullptr;
    return OnHTTPSRecord(rec, status);
  }

  return NS_OK;
}

nsresult HappyEyeballsConnectionAttempt::OnARecord(nsIDNSRecord* aRecord,
                                                   nsresult status) {
  LOG(("HappyEyeballsConnectionAttempt::OnARecord: this=%p status %" PRIx32,
       this, static_cast<uint32_t>(status)));
  // TODO: we should report this only once
  if (NS_SUCCEEDED(status)) {
    mTransaction->OnTransportStatus(nullptr, NS_NET_STATUS_RESOLVED_HOST, 0);
  }

  // TODO: use NS_ERROR_UNKNOWN_PROXY_HOST if stasus is failed and proxy is used

  mARecord = do_QueryInterface(aRecord);
  nsresult rv;
  if (NS_FAILED(status) || !mARecord) {
    nsTArray<NetAddr> emptyArray;
    rv = ProcessDnsResponseA(mHost, emptyArray);
    if (NS_FAILED(rv)) {
      return rv;
    }
    return ProcessHappyEyeballsOutput();
  }

  nsTArray<NetAddr> addresses;
  mARecord->GetAddresses(addresses);

  // Filter to only IPv4 addresses
  nsTArray<NetAddr> ipv4Addresses;
  for (const auto& addr : addresses) {
    if (addr.raw.family == AF_INET) {
      LOG(("Addr=[%s]", addr.ToString().get()));
      ipv4Addresses.AppendElement(addr);
    }
  }

  rv = ProcessDnsResponseA(mHost, ipv4Addresses);
  if (NS_FAILED(rv)) {
    return rv;
  }
  return ProcessHappyEyeballsOutput();
}

nsresult HappyEyeballsConnectionAttempt::OnAAAARecord(nsIDNSRecord* aRecord,
                                                      nsresult status) {
  LOG(("HappyEyeballsConnectionAttempt::OnAAAARecord: this=%p status %" PRIx32,
       this, static_cast<uint32_t>(status)));
  // TODO: we should report this only once
  if (NS_SUCCEEDED(status)) {
    mTransaction->OnTransportStatus(nullptr, NS_NET_STATUS_RESOLVED_HOST, 0);
  }

  // TODO: use NS_ERROR_UNKNOWN_PROXY_HOST if stasus is failed and proxy is used

  mAAAARecord = do_QueryInterface(aRecord);
  nsresult rv;
  if (NS_FAILED(status) || !mAAAARecord) {
    nsTArray<NetAddr> emptyArray;
    rv = ProcessDnsResponseAAAA(mHost, emptyArray);
    if (NS_FAILED(rv)) {
      return rv;
    }
    return ProcessHappyEyeballsOutput();
  }

  nsTArray<NetAddr> addresses;
  mAAAARecord->GetAddresses(addresses);

  // Filter to only IPv6 addresses
  nsTArray<NetAddr> ipv6Addresses;
  for (const auto& addr : addresses) {
    if (addr.raw.family == AF_INET6) {
      LOG(("Addr=[%s]", addr.ToString().get()));
      ipv6Addresses.AppendElement(addr);
    }
  }

  rv = ProcessDnsResponseAAAA(mHost, ipv6Addresses);
  if (NS_FAILED(rv)) {
    return rv;
  }
  return ProcessHappyEyeballsOutput();
}

// Helper function to convert ALPN string to Protocol enum
static Maybe<Protocol> AlpnStringToProtocol(const nsACString& aAlpn) {
  if (aAlpn.EqualsLiteral("h3")) {
    return Some(Protocol::H3);
  }
  if (aAlpn.EqualsLiteral("h2")) {
    return Some(Protocol::H2);
  }
  if (aAlpn.EqualsLiteral("http/1.1")) {
    return Some(Protocol::H1);
  }
  // Unknown ALPN protocol
  return Nothing();
}

nsresult HappyEyeballsConnectionAttempt::OnHTTPSRecord(nsIDNSRecord* aRecord,
                                                       nsresult status) {
  LOG(("HappyEyeballsConnectionAttempt::OnHTTPSRecord %p status=%x", this,
       static_cast<uint32_t>(status)));
  nsCOMPtr<nsIDNSHTTPSSVCRecord> record = do_QueryInterface(aRecord);
  if (!record || NS_FAILED(status)) {
    nsTArray<ServiceInfoFFI> emptyArray;
    (void)ProcessDnsResponseHTTPS(mHost, emptyArray);
    return ProcessHappyEyeballsOutput();
  }

  nsTArray<RefPtr<nsISVCBRecord>> svcbRecords;
  // TODO: Handle aNoHttp2, aNoHttp3, and aCname.
  (void)record->GetRecords(svcbRecords);
  if (svcbRecords.IsEmpty()) {
    nsTArray<ServiceInfoFFI> emptyArray;
    (void)ProcessDnsResponseHTTPS(mHost, emptyArray);
    return ProcessHappyEyeballsOutput();
  }

  nsTArray<ServiceInfoFFI> serviceInfos;

  for (const auto& svcbRecord : svcbRecords) {
    ServiceInfoFFI svcInfo;
    (void)svcbRecord->GetPriority(&svcInfo.priority);
    (void)svcbRecord->GetName(svcInfo.target_name);

    nsTArray<RefPtr<nsISVCParam>> values;
    (void)svcbRecord->GetValues(values);

    nsTArray<nsCString> alpn;
    uint16_t port = 0;
    nsTArray<RefPtr<nsINetAddr>> ipv4Hint;
    nsTArray<RefPtr<nsINetAddr>> ipv6Hint;

    for (const auto& value : values) {
      uint16_t type;
      (void)value->GetType(&type);
      switch (type) {
        case SvcParamKeyAlpn: {
          nsCOMPtr<nsISVCParamAlpn> alpnParam = do_QueryInterface(value);
          (void)alpnParam->GetAlpn(alpn);
          break;
        }
        case SvcParamKeyNoDefaultAlpn:
          break;
        case SvcParamKeyPort: {
          nsCOMPtr<nsISVCParamPort> portParam = do_QueryInterface(value);
          (void)portParam->GetPort(&port);
          break;
        }
        case SvcParamKeyIpv4Hint: {
          nsCOMPtr<nsISVCParamIPv4Hint> ipv4Param = do_QueryInterface(value);
          (void)ipv4Param->GetIpv4Hint(ipv4Hint);
          break;
        }
        case SvcParamKeyIpv6Hint: {
          nsCOMPtr<nsISVCParamIPv6Hint> ipv6Param = do_QueryInterface(value);
          (void)ipv6Param->GetIpv6Hint(ipv6Hint);
          break;
        }
        case SvcParamKeyEchConfig: {
          nsCOMPtr<nsISVCParamEchConfig> echConfigParam =
              do_QueryInterface(value);
          nsCString echConfig;
          (void)echConfigParam->GetEchconfig(echConfig);
          svcInfo.ech_config.AppendElements(
              reinterpret_cast<const uint8_t*>(echConfig.BeginReading()),
              echConfig.Length());
          break;
        }
        default:
          break;
      }
    }

    for (const auto& alpnStr : alpn) {
      auto protocol = AlpnStringToProtocol(alpnStr);
      if (protocol) {
        svcInfo.alpn_protocols.AppendElement(protocol.ref());
      }
    }

    for (const auto& addr : ipv4Hint) {
      NetAddr netAddr;
      addr->GetNetAddr(&netAddr);
      svcInfo.ipv4_hints.AppendElement(netAddr);
    }

    for (const auto& addr : ipv6Hint) {
      NetAddr netAddr;
      addr->GetNetAddr(&netAddr);
      svcInfo.ipv6_hints.AppendElement(netAddr);
    }

    serviceInfos.AppendElement(std::move(svcInfo));
  }

  (void)ProcessDnsResponseHTTPS(mHost, serviceInfos);
  return ProcessHappyEyeballsOutput();
}

NS_IMETHODIMP  // method for nsITimerCallback
HappyEyeballsConnectionAttempt::Notify(nsITimer* timer) {
  return ProcessHappyEyeballsOutput();
}

NS_IMETHODIMP  // method for nsINamed
HappyEyeballsConnectionAttempt::GetName(nsACString& aName) {
  aName.AssignLiteral("HappyEyeballsConnectionAttempt");
  return NS_OK;
}

void HappyEyeballsConnectionAttempt::SetupTimer(uint64_t aTimeout) {
  if (!aTimeout) {
    return;
  }

  LOG3(("HappyEyeballsConnectionAttempt::SetupTimer to %" PRIu64
        "ms [this=%p].",
        aTimeout, this));

  if (!mTimer) {
    // This can only fail on OOM and we'd crash.
    mTimer = NS_NewTimer();
  }

  DebugOnly<nsresult> rv =
      mTimer->InitWithCallback(this, aTimeout, nsITimer::TYPE_ONE_SHOT);
  // There is no meaningful error handling we can do here. But an error here
  // should only be possible if the timer thread did already shut down.
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

}  // namespace mozilla::net
