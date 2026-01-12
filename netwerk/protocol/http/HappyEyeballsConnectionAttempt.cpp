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
    (void)happy_eyeballs_new(&mHappyEyeballs, &mHost,
                             static_cast<uint16_t>(mConnInfo->OriginPort()));
  } else {
    mHost = mConnInfo->GetRoutedHost();
    (void)happy_eyeballs_new(&mHappyEyeballs, &mHost,
                             static_cast<uint16_t>(mConnInfo->RoutedPort()));
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
  return ProcessHappyEyeballsEvents(HappyEyeballsInputKind::None, mHost,
                                    nullptr, 0);
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

nsresult HappyEyeballsConnectionAttempt::ProcessHappyEyeballsEvents(
    HappyEyeballsInputKind aInputKind, const nsACString& aHost,
    const NetAddr* aAddresses, uint32_t aAddrLen) {
  LOG(("HappyEyeballsConnectionAttempt::ProcessHappyEyeballsEvents %p", this));

  nsresult rv = NS_OK;
  while (true) {
    HappyEyeballsEvent event{};
    nsTArray<uint8_t> heData;
    rv = happy_eyeballs_process(const_cast<HappyEyeballs*>(mHappyEyeballs),
                                aInputKind, &aHost, aAddresses, aAddrLen,
                                &event, &heData);
    if (NS_FAILED(rv)) {
      LOG(("process failed rv=%x", static_cast<uint32_t>(rv)));
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
        LOG(("HappyEyeballsEvent::Tag::AttemptConnection protocol=%d port=%d",
             event.attempt_connection.protocol, event.attempt_connection.port));
        auto res = ToNetAddr(heData, event.attempt_connection.port);
        if (res.isErr()) {
          LOG(("Failed to convert to NetAddr"));
          // TODO: how to handle this error?
          return res.unwrapErr();
        }

        LOG(("connect to:[%s]", res.unwrap().ToString().get()));
        EstablishTCPConnection(res.unwrap());
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
      // TODO: deal with HTTPS RR later
      return Err(NS_ERROR_NOT_AVAILABLE);
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
    case DnsRecordType::Https:
      rv = dns->AsyncResolveNative(
          mHost, nsIDNSService::RESOLVE_TYPE_HTTPSSVC,
          aFlags | nsIDNSService::RESOLVE_WANT_RECORD_ON_ERROR, nullptr, this,
          gSocketTransportService, mConnInfo->GetOriginAttributes(),
          getter_AddRefs(mHTTPSRequest));
      break;
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

  return rv;
}

void HappyEyeballsConnectionAttempt::HandleTCPConnectionResult(
    Result<RefPtr<HttpConnectionBase>, nsresult> aResult,
    TCPConnectionEstablisher* aEstablisher) {
  LOG(("HappyEyeballsConnectionAttempt::HandleTCPConnectionResult %p", this));

  RefPtr<TCPConnectionEstablisher> establisher = aEstablisher;
  mConnectionEstablisherTable.Remove(establisher->AddrKey());

  if (aResult.isErr()) {
    // TODO: notify the state machine result
    establisher->Close(aResult.unwrapErr());
    return;
  }

  if (mDone) {
    // Should we use another error code?
    establisher->Close(NS_BASE_STREAM_CLOSED);
    return;
  }

  RefPtr<HttpConnectionBase> conn = aResult.unwrap();
  RefPtr<nsHttpConnection> connTCP = do_QueryObject(conn);
  LOG(("Got connTCP:%p", connTCP.get()));

  RefPtr<PendingTransactionInfo> pendingTransInfo =
      gHttpHandler->ConnMgr()->FindTransactionHelper(true, mEntry,
                                                     mTransaction);
  if (pendingTransInfo) {
    MOZ_ASSERT(!mSpeculative, "Speculative Half Open found mTransaction");
    mEntry->InsertIntoActiveConns(connTCP);
    nsresult rv = gHttpHandler->ConnMgr()->DispatchTransaction(
        mEntry, pendingTransInfo->Transaction(), connTCP);
    if (NS_FAILED(rv)) {
      mTransaction->Close(rv);
    }
  } else {
    // After about 1 second allow for the possibility of restarting a
    // transaction due to server close. Keep at sub 1 second as that is the
    // minimum granularity we can expect a server to be timing out with.
    connTCP->SetIsReusedAfter(950);

    LOG(
        ("HandleTCPConnectionResult no transaction match "
         "returning conn %p to pool\n",
         connTCP.get()));
    gHttpHandler->ConnMgr()->OnMsgReclaimConnection(connTCP);
  }

  Done();
}

nsresult HappyEyeballsConnectionAttempt::EstablishTCPConnection(NetAddr aAddr) {
  NetAddrKey key(aAddr);
  RefPtr<TCPConnectionEstablisher> establisher = new TCPConnectionEstablisher(
      mConnInfo, key, mCaps, mSpeculative, mAllow1918);
  auto callback = [self = RefPtr{this}, establisher](
                      Result<RefPtr<HttpConnectionBase>, nsresult> aResult) {
    self->HandleTCPConnectionResult(std::move(aResult), establisher);
  };

  if (establisher->Start(std::move(callback))) {
    mConnectionEstablisherTable.InsertOrUpdate(key, std::move(establisher));
  }

  return NS_OK;
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
  auto cancelAndClear = [](nsCOMPtr<nsICancelable>&& aRequest) {
    if (aRequest) {
      aRequest->Cancel(NS_ERROR_ABORT);
      aRequest = nullptr;
    }
  };

  cancelAndClear(std::move(mARequest));
  cancelAndClear(std::move(mAAAARequest));
  cancelAndClear(std::move(mHTTPSRequest));

  for (auto iter = mConnectionEstablisherTable.Iter(); !iter.Done();
       iter.Next()) {
    RefPtr<ConnectionEstablisher> conn = iter.Data();
    conn->Close(NS_ERROR_ABORT);
  }
  mConnectionEstablisherTable.Clear();

  if (mTimer) {
    mTimer->Cancel();
  }
  mTimer = nullptr;
}

void HappyEyeballsConnectionAttempt::Done() {
  LOG(("HappyEyeballsConnectionAttempt::Done %p", this));

  MOZ_ASSERT(!mDone);
  mDone = true;

  RefPtr<HappyEyeballsConnectionAttempt> self(this);
  mEntry->RemoveConnectionAttempt(this, false);
}

double HappyEyeballsConnectionAttempt::Duration(TimeStamp epoch) { return 0; }

void HappyEyeballsConnectionAttempt::CloseTransports(nsresult error) {}

void HappyEyeballsConnectionAttempt::PrintDiagnostics(nsCString& log) {}

bool HappyEyeballsConnectionAttempt::AcceptsTransaction(
    nsHttpTransaction* trans) {
  return false;
}

bool HappyEyeballsConnectionAttempt::Claim() { return false; }

void HappyEyeballsConnectionAttempt::Unclaim() {}

NS_IMETHODIMP
HappyEyeballsConnectionAttempt::OnLookupComplete(nsICancelable* request,
                                                 nsIDNSRecord* rec,
                                                 nsresult status) {
  LOG(("HappyEyeballsConnectionAttempt::OnLookupComplete"));
  if (request == mARequest) {
    mARequest = nullptr;
    return OnARecord(rec, status);
  }

  if (request == mAAAARequest) {
    mAAAARequest = nullptr;
    return OnAAAARecord(rec, status);
  }

  if (request == mHTTPSRequest) {
    mHTTPSRequest = nullptr;
    return OnHTTPSRecord(rec, status);
  }

  MOZ_ASSERT_UNREACHABLE("Unexpected DNS type");
  return NS_OK;
}

static UniquePtr<NetAddr[]> ToRawArray(const nsTArray<NetAddr>& aArray,
                                       size_t& aLength) {
  aLength = aArray.Length();
  if (aArray.IsEmpty()) {
    return nullptr;
  }

  auto result = mozilla::MakeUnique<NetAddr[]>(aArray.Length());
  for (size_t i = 0; i < aArray.Length(); ++i) {
    result[i] = aArray[i];
  }

  return result;
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
  if (NS_FAILED(status) || !mARecord) {
    return ProcessHappyEyeballsEvents(HappyEyeballsInputKind::DnsResponseA,
                                      mHost, nullptr, 0);
  }

  nsTArray<NetAddr> addresses;
  mARecord->GetAddresses(addresses);
  size_t len = 0;
  UniquePtr<NetAddr[]> rawArray = ToRawArray(addresses, len);
  (void)ProcessHappyEyeballsEvents(HappyEyeballsInputKind::DnsResponseA, mHost,
                                   rawArray.get(), len);
  return NS_OK;
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
  if (NS_FAILED(status) || !mAAAARecord) {
    return ProcessHappyEyeballsEvents(HappyEyeballsInputKind::DnsResponseAaaa,
                                      mHost, nullptr, 0);
  }

  nsTArray<NetAddr> addresses;
  mAAAARecord->GetAddresses(addresses);
  size_t len = 0;
  UniquePtr<NetAddr[]> rawArray = ToRawArray(addresses, len);
  (void)ProcessHappyEyeballsEvents(HappyEyeballsInputKind::DnsResponseAaaa,
                                   mHost, rawArray.get(), len);
  return NS_OK;
}

nsresult HappyEyeballsConnectionAttempt::OnHTTPSRecord(nsIDNSRecord* aRecord,
                                                       nsresult status) {
  // TODO: Implement this later
  return NS_OK;
}

NS_IMETHODIMP  // method for nsITimerCallback
HappyEyeballsConnectionAttempt::Notify(nsITimer* timer) {
  (void)ProcessHappyEyeballsEvents(HappyEyeballsInputKind::None, mHost, nullptr,
                                   0);
  return NS_OK;
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
