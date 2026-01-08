/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

#include "HappyEyeballsConnectionAttempt.h"

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

nsresult HappyEyeballsConnectionAttempt::ProcessHappyEyeballsEvents(
    HappyEyeballsInputKind aInputKind, const nsACString& aHost,
    const uint8_t* aAddrBytes, uint32_t aAddrLen) {
  LOG(("HappyEyeballsConnectionAttempt::ProcessHappyEyeballsEvents %p", this));

  nsresult rv = NS_OK;
  while (true) {
    HappyEyeballsEvent event{};
    nsTArray<uint8_t> heData;
    rv = happy_eyeballs_process(const_cast<HappyEyeballs*>(mHappyEyeballs),
                                aInputKind, &aHost, aAddrBytes, aAddrLen,
                                &event, &heData);
    if (NS_FAILED(rv)) {
      LOG(("process failed rv=%x", static_cast<uint32_t>(rv)));
      return rv;
    }

    LOG(("event.tag=%d", event.tag));
    switch (event.tag) {
      case HappyEyeballsEvent::Tag::SendDnsQuery: {
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
        // TODO: arm timer of type event.timer.timer_type
        //       with duration event.timer.duration_ms
        break;
      }

      case HappyEyeballsEvent::Tag::AttemptConnection: {
        // TODO: attempt connection using event.attempt_connection.protocol
        //       and event.attempt_connection.port
        break;
      }

      case HappyEyeballsEvent::Tag::None:
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
  if (addresses.IsEmpty()) {
    return ProcessHappyEyeballsEvents(HappyEyeballsInputKind::DnsResponseA,
                                      mHost, nullptr, 0);
  }

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
  if (addresses.IsEmpty()) {
    return ProcessHappyEyeballsEvents(HappyEyeballsInputKind::DnsResponseAaaa,
                                      mHost, nullptr, 0);
  }

  return NS_OK;
}

nsresult HappyEyeballsConnectionAttempt::OnHTTPSRecord(nsIDNSRecord* aRecord,
                                                       nsresult status) {
  // TODO: Implement this later
  return NS_OK;
}

NS_IMETHODIMP  // method for nsITimerCallback
HappyEyeballsConnectionAttempt::Notify(nsITimer* timer) {
  return NS_OK;
}

NS_IMETHODIMP  // method for nsINamed
HappyEyeballsConnectionAttempt::GetName(nsACString& aName) {
  aName.AssignLiteral("HappyEyeballsConnectionAttempt");
  return NS_OK;
}

}  // namespace mozilla::net
