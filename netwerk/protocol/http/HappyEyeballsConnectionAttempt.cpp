/* vim:set ts=4 sw=2 sts=2 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

#include "HappyEyeballsConnectionAttempt.h"
#include "mozilla/net/happy_eyeballs_glue.h"

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
  (void)happy_eyeballs_new(&mHappyEyeballs, &mConnInfo->GetOrigin(),
                           static_cast<uint16_t>(mConnInfo->OriginPort()));
}

HappyEyeballsConnectionAttempt::~HappyEyeballsConnectionAttempt() {
  if (mHappyEyeballs) {
    happy_eyeballs_release(mHappyEyeballs);
    mHappyEyeballs = nullptr;
  }
}

nsresult HappyEyeballsConnectionAttempt::Init(ConnectionEntry* ent) {
  return ProcessHappyEyeballsEvents();
}

nsresult HappyEyeballsConnectionAttempt::ProcessHappyEyeballsEvents() {
  LOG(("HappyEyeballsConnectionAttempt::ProcessHappyEyeballsEvents %p", this));

  nsresult rv = NS_OK;
  HappyEyeballsInputKind inputKind = HappyEyeballsInputKind::None;
  while (true) {
    HappyEyeballsEvent event{};
    nsTArray<uint8_t> heData;
    rv =
        happy_eyeballs_process(const_cast<HappyEyeballs*>(mHappyEyeballs),
                               inputKind, nullptr, nullptr, 0, &event, &heData);
    if (NS_FAILED(rv)) {
      LOG(("process failed rv=%x", static_cast<uint32_t>(rv)));
      return rv;
    }

    LOG(("event.tag=%d", event.tag));
    switch (event.tag) {
      case HappyEyeballsEvent::Tag::SendDnsQuery: {
        LOG(("SendDnsQuery type=%d", event.send_dns_query.record_type));
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

    // After the first iteration, further calls are typically driven
    // by timers / DNS / socket events rather than the original input.
    inputKind = HappyEyeballsInputKind::None;
  }

  return rv;
}

void HappyEyeballsConnectionAttempt::Abandon() {}

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
