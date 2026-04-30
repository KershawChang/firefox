/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Smoke coverage for the multi-channel HE path that originally surfaced
// the "Unclaim strands sibling transaction" bug fixed in
// HappyEyeballsConnectionAttempt::Unclaim() (override to no-op).
//
// The deterministic reproducer for the original bug is the WPT
// `html/anonymous-iframe/embedding.tentative.https.window.js` shard
// suite: it relies on cross-shard state (a warmed-up entry plus a
// burst of concurrent fetches) that we can't easily synthesize in
// xpcshell. This test exercises the same general territory — a
// pre-warmed conn entry plus a concurrent burst with one Unblocked
// channel — and asserts every channel completes. It will not always
// catch a regression of the specific Unclaim bug; the WPT shards in
// CI are the canonical regression check. It does provide a fast
// smoke test for related future regressions in the HE-multi-channel
// dispatch path.

"use strict";

var { setTimeout } = ChromeUtils.importESModule(
  "resource://gre/modules/Timer.sys.mjs"
);

const { NodeHTTP2Server } = ChromeUtils.importESModule(
  "resource://testing-common/NodeServer.sys.mjs"
);

let trrServer;

add_setup(async function () {
  Services.prefs.setBoolPref("network.http.happy_eyeballs_enabled", true);
  Services.prefs.setIntPref("network.http.speculative-parallel-limit", 6);

  trrServer = new TRRServer();
  await trrServer.start();
  trr_test_setup();
  Services.prefs.setIntPref("network.trr.mode", 3);
  Services.prefs.setCharPref(
    "network.trr.uri",
    `https://foo.example.com:${trrServer.port()}/dns-query`
  );

  registerCleanupFunction(async () => {
    Services.prefs.clearUserPref("network.http.happy_eyeballs_enabled");
    Services.prefs.clearUserPref("network.http.speculative-parallel-limit");
    trr_clear_prefs();
    if (trrServer) {
      await trrServer.stop();
    }
  });
});

// Pause the first TCP connection at the socket level for delayMs so
// later HE attempts race past it and create the conditions where a
// previous claimer's pending-trans-info gets destroyed (Unclaim) while
// other attempts are still in flight.
async function pauseFirstConnection(server, delayMs) {
  await server.execute(`
    global.firstConnPaused = false;
    global.server.on("connection", (socket) => {
      if (!global.firstConnPaused) {
        global.firstConnPaused = true;
        socket.pause();
        setTimeout(() => { try { socket.resume(); } catch(e) {} }, ${delayMs});
      }
    });
  `);
}

function makeChannel(host, port, unblocked = false) {
  let chan = NetUtil.newChannel({
    uri: `https://${host}:${port}/test`,
    loadUsingSystemPrincipal: true,
  }).QueryInterface(Ci.nsIHttpChannel);
  chan.loadFlags = Ci.nsIChannel.LOAD_INITIAL_DOCUMENT_URI;
  if (unblocked) {
    let cos = chan.QueryInterface(Ci.nsIClassOfService);
    cos.addClassFlags(Ci.nsIClassOfService.Unblocked);
  }
  return chan;
}

function fetchOnce(chan) {
  return new Promise(resolve => {
    chan.asyncOpen({
      onStartRequest() {},
      onDataAvailable(req, stream, offset, count) {
        read_stream(stream, count);
      },
      onStopRequest(req) {
        let status = 0;
        try {
          status = req.QueryInterface(Ci.nsIHttpChannel).responseStatus;
        } catch (e) {}
        resolve({ status });
      },
    });
  });
}

add_task(async function test_unclaim_does_not_strand_sibling_txn() {
  Services.dns.clearCache(true);
  Services.obs.notifyObservers(null, "net:cancel-all-connections");
  // eslint-disable-next-line mozilla/no-arbitrary-setTimeout
  await new Promise(resolve => setTimeout(resolve, 500));

  let server = new NodeHTTP2Server();
  await server.start();
  await server.registerPathHandler("/test", (_req, resp) => {
    resp.writeHead(200, { "Content-Type": "text/plain" });
    resp.end("ok");
  });

  let serverPort = server.port();
  let host = "alt1.example.com";

  await trrServer.registerDoHAnswers(host, "A", {
    answers: [
      { name: host, ttl: 55, type: "A", flush: false, data: "127.0.0.1" },
    ],
  });

  // Step 1: warm-up. Run a single fetch first to seed the entry with
  // an idle conn. The bug needs an idle conn to appear at the right
  // moment so a queued creator txn can be dispatched via step 2 (idle)
  // — that step-2 dispatch is what triggers the
  // ~PendingTransactionInfo -> Unclaim that flips mFreeToUse back to
  // true on the creator's HE attempt.
  {
    let chan = makeChannel(host, serverPort);
    let r = await fetchOnce(chan);
    Assert.equal(r.status, 200, "warm-up fetch succeeds");
  }

  // Step 2: pause the next TCP connection. With an idle conn already
  // in the pool, the entry can dispatch via step 2 immediately when a
  // new txn arrives — exactly the path that destroys the creator's
  // pending-trans-info while subsequent claimers are still racing.
  await pauseFirstConnection(server, 2000);

  // Step 3: burst of concurrent channels. 12 with one Unblocked
  // matches the WPT pattern that originally hit the bug. With the
  // override of HE::Unclaim, Claim() never spuriously succeeds for a
  // sibling, so all complete.
  const numChannels = 12;
  const unblockedIndex = 6;
  let promises = [];
  for (let i = 0; i < numChannels; i++) {
    let chan = makeChannel(host, serverPort, /*unblocked=*/ i === unblockedIndex);
    promises.push(fetchOnce(chan));
  }

  // Cap the wait so the test fails with a clear assertion rather than
  // timing out at the harness level. 30s is well over any legitimate
  // completion path; the bug's mode is "hangs forever".
  let timeoutPromise = new Promise(resolve =>
    // eslint-disable-next-line mozilla/no-arbitrary-setTimeout
    setTimeout(() => resolve("TIMEOUT"), 30000)
  );
  let results = await Promise.race([Promise.all(promises), timeoutPromise]);

  Assert.notEqual(
    results,
    "TIMEOUT",
    "All channels should complete (no Unclaim-induced strand)"
  );
  let successCount = 0;
  for (let r of results) {
    if (r.status === 200) {
      successCount++;
    }
  }
  Assert.equal(
    successCount,
    numChannels,
    `All ${numChannels} channels should succeed (got ${successCount})`
  );

  await server.stop();
});
