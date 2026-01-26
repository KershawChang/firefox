/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef HappyEyeballs_h__
#define HappyEyeballs_h__

#include <cstdint>
#include "nsError.h"
#include "nsTArray.h"
#include "mozilla/net/happy_eyeballs_glue.h"

namespace mozilla {
namespace net {

class HappyEyeballsAPI final {
 public:
  static nsresult Init(HappyEyeballs** aHappyEyeballs,
                       const nsACString& aOrigin, uint16_t aPort,
                       const nsTArray<AltSvc>* aAltSvc = nullptr) {
    nsTArray<AltSvc> emptyAltSvc;
    return happy_eyeballs_new((const HappyEyeballs**)aHappyEyeballs, &aOrigin,
                              aPort, aAltSvc ? aAltSvc : &emptyAltSvc);
  }
};


}  // namespace net
}  // namespace mozilla

#endif
