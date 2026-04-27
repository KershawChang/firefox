# Happy Eyeballs v3 in necko

This describes the architecture of the HE driver and the 0-RTT
extension built on top of it.

## High-level idea

For a given origin, run DNS resolution and multiple TCP/UDP connection
attempts in parallel across IPv4 / IPv6 (and H2/H3 where applicable),
pick the first TLS handshake to finish, dispatch the real
`nsHttpTransaction` onto that winning connection, and tear down the
losers.

## Components

```
ConnectionEntry
  └── ConnectionAttemptPool
        └── HappyEyeballsConnectionAttempt  (one per origin race)
              │
              ├── HappyEyeballs              // Rust state machine
              │
              ├── mDnsRequestTable           // id -> DnsRequestInfo
              │     (A, AAAA, HTTPSSVC)
              │
              ├── mConnectionEstablisherTable
              │     // id -> TCPConnectionEstablisher
              │     //    or UDPConnectionEstablisher
              │
              │     each ConnectionEstablisher owns:
              │       - nsISocketTransport / UDP equivalent
              │       - ConnectionHandle (wraps the nascent conn)
              │       - HappyEyeballsTransaction  (speculative)
              │            └── inherits SpeculativeTransaction
              │                 drives TLS handshake without consuming
              │                 real request data
              │
              └── mTransaction                // the real nsAHttpTransaction
                    (starts as NullTransaction when speculative,
                     replaced by a real nsHttpTransaction via Claim())
```

- `HappyEyeballsConnectionAttempt` is a `ConnectionAttempt`. The
  connection manager can `Claim`, `Abandon`, or `OnTimeout` it the same
  way as a `DnsAndConnectSocket`.
- `HappyEyeballsTransaction` (HT) is a one-per-establisher speculative
  shim. It collects TCP and TLS timings, then forwards
  `OnTransportStatus` back to the owning `HappyEyeballsConnectionAttempt`
  via a caller-supplied `StatusForwarder`. HE deduplicates the events
  before relaying them to the real transaction.
- `ConnectionEstablisher` (with TCP and UDP subclasses) contains the
  low-level "open socket, drive handshake, surface the ready connection"
  machinery that used to live in `DnsAndConnectSocket`.

## Event loop

Everything is on the socket thread.

```
Init ──▶ ProcessHappyEyeballsOutput() ──▶ drain events from
                                          happy_eyeballs_process_output
                                           │
                                           ├─ SendDnsQuery(id,type)
                                           │   └─ DNSLookup(...)  →  nsIDNSService::AsyncResolve
                                           ├─ Timer(duration)
                                           │   └─ SetupTimer(...)
                                           ├─ AttemptConnection(id,ver,addr,port)
                                           │   ├─ H3  →  EstablishUDPConnection()
                                           │   └─ else →  EstablishTCPConnection()
                                           │         └─ builds a ConnectionEstablisher +
                                           │            HappyEyeballsTransaction, starts it
                                           ├─ CancelConnection(id)
                                           │   └─ CancelConnection(id)
                                           ├─ Succeeded   →  OnSucceeded()
                                           └─ Failed      →  CloseHttpTransaction(reason)
```

Inputs flowing back into the state machine:

- DNS: `OnARecord` / `OnAAAARecord` / `OnHTTPSRecord` call
  `happy_eyeballs_process_dns_response_{a,aaaa,https}` and then
  re-drain via `ProcessHappyEyeballsOutput()`.
- Connections: `HandleTCPConnectionResult` / `HandleUDPConnectionResult`
  capture the winning conn into `mOutputConn` (and its HT into
  `mOutputTrans`), then call `happy_eyeballs_process_connection_result`
  and re-drain.
- Timer: `Notify(nsITimer*)` re-drains.

## Declaring a winner

`OnSucceeded` is the single dispatch site:

```
OnSucceeded
 ├─ RecordIPFamilyPreference(mAddrFamily)   // next time, prefer this family
 ├─ mOutputConn->SetDnsBootstrapTimings()   // domainLookupStart/End
 ├─ Copy mOutputTrans->Timings() onto the real nsHttpTransaction
 │    (preserving transactionPending so DispatchTransaction records
 │     wait-time metrics correctly)
 ├─ if TCP conn: ProcessTCPConn()  // dispatches real txn via
 │                                  // FindTransactionHelper +
 │                                  // ConnMgr::DispatchTransaction
 └─ if UDP conn: ProcessUDPConn()  // analogous; plus
                                   // ConnMgr::ReportHttp3Connection
```

Losing ConnectionEstablishers are closed in `Abandon()`, called at the
tail of `OnSucceeded`. `Abandon` is also called independently from the
`Failed`, timeout, and LNA-denied paths. `Abandon`:

1. Cancels all outstanding DNS requests in `mDnsRequestTable`.
2. Snapshots `mConnectionEstablisherTable` into a local array, then
   closes each establisher. The snapshot prevents the
   `Close(NS_ERROR_ABORT)` re-entrancy below from invalidating the
   iterator.
3. Cancels the HE timer.
4. Calls `mZeroRttHandle->Cleanup()` on the socket thread. This drops
   the handle's `nsWeakPtr<HappyEyeballsConnectionAttempt>` while we are
   guaranteed to be on the right thread.
5. Drops `mEntry`.

## Speculative mode & Claim

HE can run with `mSpeculative = true` and a `NullHttpTransaction` in
`mTransaction`. When a real `nsHttpTransaction` is later directed to
this entry, the conn manager calls `Claim(realTxn)`:

- Clears `mSpeculative`, flips `mAllow1918`, and clears
  `ResetSpeculativeFlags()` on each in-flight establisher.
- If `mTransaction` is still the null txn, swaps in `realTxn` and
  replays the transport-status events that were already sent to the
  null txn (`RESOLVING_HOST`, `RESOLVED_HOST`, `CONNECTING_TO`,
  `CONNECTED_TO`) so the channel sees the sequence the user expects.

The real transaction is **not** removed from the pending queue at
`Claim` time. Removal happens later, at dispatch, when
`ProcessTCPConn` or `ProcessUDPConn` calls
`FindTransactionHelper(true, ...)`. This is why
`CloseAllConnectionAttempts` (e.g. triggered by `MakeAllDontReuseExcept`
during H2 coalescing) can safely abandon in-flight HE attempts: any
associated real transaction is still in the pending queue and will be
dispatched onto the winning coalesced connection.

## Failure paths

- `Failed` event from the Rust state machine → `CloseHttpTransaction`
  with the best-available reason (most recent connection error for
  connection-failure, most recent DNS error for DNS-failure), then
  `Abandon` + `RemoveConnectionAttempt(this, false)`.
- Local-network-access denial in `CheckLNA` → close the real txn with
  `NS_ERROR_LOCAL_NETWORK_ACCESS_DENIED` and short-circuit all pending
  attempts via `Abandon` + `RemoveConnectionAttempt`.
- `OnTimeout` → close the real txn with `NS_ERROR_NET_TIMEOUT` and
  `Abandon`.

## Timings plumbing

HE carries three time-keepers:

- `mDomainLookupStart` and `mDomainLookupEnd`: set on the first A/AAAA
  DNS event. They are bootstrapped onto the winning `HttpConnectionBase`
  via `SetDnsBootstrapTimings`.
- `mFirstConnectionStart`: set on the first `AttemptConnection` event.
  Used by `Duration()` for the connection manager's timeout tick.
- `HappyEyeballsTransaction::Timings()`: per-establisher TCP and TLS
  handshake times. They are transferred onto the real
  `nsHttpTransaction` in `OnSucceeded` before dispatch.

## TRR / HTTPS RR handling

`OnARecord` and `OnAAAARecord` populate a local `DnsMetadata`
(`mIsTRR`, `mEffectiveTRRMode`, `mTrrSkipReason`, etc.) from the
address record and hand that struct to each `ConnectionEstablisher`
via `SetDnsMetadata`. The establisher wraps each NetAddr in a
`SingleDNSAddrRecord` carrying the same metadata.
`nsHttpConnection` and `HttpConnectionUDP` read it during `Init` to
set `mResolvedByTRR`, `mEffectiveTRRMode`, and `mTRRSkipReason`, so the
channel ultimately reports `isResolvedByTRR` correctly.

When the HE state machine commits to an `AttemptConnection` purely
from an HTTPS RR that carries `ipv4hint` or `ipv6hint` (with no
A/AAAA response yet for this attempt), `OnHTTPSRecord` mirrors the
same population from `httpsRecord->IsTRR()` so the downstream
connection still reports TRR correctly. A `mTRRInfoForwarded` flag in
HE ensures `nsHttpTransaction::SetTRRInfo` is only called once:
whichever of `OnARecord`, `OnAAAARecord`, or `OnHTTPSRecord` fires
first forwards the info, and the others skip.

## 0-RTT (TLS 1.3 early data) extension

The HE race is run with a second layer: the first attempt whose TLS
stack can do 0-RTT resumption commits the real request as early data.
If the server accepts it, the handshake completes with the request
already on the wire (saving a round-trip). If it rejects, the request
is retransmitted after the Finished.

The complication specific to HE is that 0-RTT lets an attempt put
real request bytes on the wire *before* its TLS handshake completes.
With several racers all able to do 0-RTT at the same time, multiple
attempts would otherwise read from (and advance) the real
transaction's shared request stream in parallel.

### New components

```
HappyEyeballsConnectionAttempt
  ├── mZeroRttHandle                    // one per race
  └── mTRRInfoForwarded                 // guards SetTRRInfo dedup

ZeroRttHandle                           // shared across all racer HTs
  ├── mHet          nsWeakPtr → HE      // cleared by Cleanup() on socket thread
  ├── mWinner / mRejected               // race resolution
  └── mAny0RttStarted                   // at least one HT entered 0-RTT

HappyEyeballsTransaction (HT)            // per establisher
  ├── mZeroRttHandle                    // shared ref
  ├── m0RttRequestStreamOffset          // this attempt's byte offset
  │                                       into real txn's request stream
  └── mRealTxn                          // set by Adopt() on the winner
```

- The first time `CreateAttemptTransaction` runs, it lazily allocates
  one `ZeroRttHandle` and hands it to every HT in the race.
- `HappyEyeballsTransaction` carries two pieces of 0-RTT state: the
  shared handle, and its own `m0RttRequestStreamOffset`. The offset
  prevents racer HTs reading the same request stream from clobbering
  each other's position.
- `IsAdopted() == (mRealTxn != nullptr)` is the lifecycle flag. An HT
  is in the RACING phase until `Adopt`, and ADOPTED afterwards.

### Start: `Do0RTT`

NSS signals that 0-RTT can proceed inside
`TlsHandshaker::Check0RttEnabled` on a specific connection. The
connection calls `trans->Do0RTT()` on its `nsAHttpTransaction`, and HT
delegates to `ZeroRttHandle::Do0RTT(caller=this)`:

1. If the caller is already in the 0-RTT flow
   (`m0RttRequestStreamOffset` is set), return true. The call is
   idempotent.
2. Otherwise, require that the real transaction is resolvable, still
   open, and the request method is safe. Set the caller's offset to
   `0`, mark `mAny0RttStarted = true`, and return true.

### Reading early data: `ZeroRttHandle::ReadSegments`

`nsHttpConnection::ReadSegments` calls HT's `ReadSegments`. If HT has
`m0RttRequestStreamOffset.isSome()`, the call routes to
`ZeroRttHandle::ReadSegments(caller, reader, count, countRead)`:

1. If `mWinner` is already set, return `NS_BASE_STREAM_CLOSED`. The
   race is decided, and further reads would move the shared stream out
   from under the winner. Losing attempts get marked done via the
   normal speculative-transaction shutdown path.
2. Seek the real transaction's request stream to `caller->offset`.
3. Forward the read via `stream->ReadSegments(...)` into the provided
   segment reader. This is what actually puts bytes on the TLS wire
   as early data.
4. Advance `caller->offset` by `countRead`.
5. On the first successful byte flow, call
   `realTxn->MarkEarlyDataSent()`. This flips
   `mEarlyDataDisposition` from `EARLY_NONE` to `EARLY_SENT` on the
   real transaction, mirroring the non-HE
   `nsHttpTransaction::ReadSegments` transition.

`HT::RequestHead()` returns the real transaction's head (looked up
via the shared handle) during the RACING phase. Without that,
`Http3Stream::TryActivating` and `Http2Stream::GenerateHeaders` would
encode an empty `:path` and a bogus `:authority`.

### Resolving the 0-RTT race: `Finish0RTT`

`Finish0RTT` is invoked from `nsHttpConnection::HandshakeDone` once
NSS reports whether early data was accepted. The call goes
`trans->Finish0RTT(aRestart, aAlpnChanged)`, then through HT to
`ZeroRttHandle::Finish0RTT(caller, aRestart, aAlpnChanged)`:

1. **First caller wins.** If `mWinner` is already set, this is a late
   `Finish0RTT` on a losing attempt. Leave the stream alone and
   return.
2. Commit `mWinner = caller` and `mRejected = aRestart`.
3. Backfill the real transaction's 0-RTT flags via
   `FinishAdopted0RTT`:
   - Both accept and reject set `mEarlyDataWasAvailable = true`, so a
     later 0-RTT TLS alert in `Close()` can drive
     `ShouldRestartOn0RttError`.
   - Accept promotes `EARLY_SENT` to `EARLY_ACCEPTED`, so
     `HandleContentStart` tags the response and a server `425` is
     mapped to `EARLY_425` for retry.
   - Reject sets `mDoNotTryEarlyData` and seeks the real
     transaction's request stream back to 0. A subsequent send must
     replay the full request over the connection that resumed as a
     full handshake.

   Fields like `mConnected`, `mSecurityInfo`, and the fallback timers
   are intentionally left alone. The real transaction's own
   `ReadSegments` init path sets them when dispatch kicks in.
4. **Adoption.** `het->AdoptWinner(caller)` removes the real
   transaction from the pending queue (via
   `FindTransactionHelper(true, …)`) and calls
   `caller->Adopt(realTxn)` on the winning HT. See the next section.
5. Run `Cleanup()` on the handle's `mHet` weak ref while we are on
   the socket thread.
6. On **accept**, seek the real transaction's request stream to
   `caller->offset`. That is the byte index already delivered as
   early data. The real transaction reads 0 more bytes from the
   (EOF-by-now) stream, and `nsHttpConnection` transitions to
   "request sent, awaiting response" without sending a duplicate on
   the wire. The **reject** path was already seeked to 0 inside
   `FinishAdopted0RTT(restart=true)` in step 3, so nothing to do
   here.
7. `caller->InvokeCallback()` fires `SpeculativeTransaction`'s
   `mCloseCallback(NS_OK)`, which is the establisher's connected
   callback. That resumes the establisher's `Finish(NS_OK)` path,
   producing a successful `HandleTCPConnectionResult` into HE, which
   feeds `NS_OK` into the Rust state machine. The state machine
   ultimately emits `Succeeded`, which lands in `OnSucceeded`.

### Adoption: `HappyEyeballsTransaction::Adopt` and `SwapTransaction`

`Adopt(realTxn)` hands the real transaction to the live connection
**and re-keys the session or connection so it drives the real
transaction directly, bypassing HT**. This is the key change from the
previous "HT forwards every virtual to the real txn" design.
Post-Adopt, HT is completely out of the I/O path.

The re-keying is done by a `SwapTransaction(aOld, aNew)` method
declared on each of `Http3Session`, `Http2Session`, and
`nsHttpConnection`. The shape is the same in each case:

- Update the `mTransaction` slot that the session or connection calls
  `WriteSegments`, `ReadSegments`, and `Close` on. The slot lives on
  `Http{2,3}Stream::mTransaction` for H2/H3, and on
  `nsHttpConnection::mTransaction` for H1. It moves from `aOld` to
  `aNew`.
- For H2 and H3 only, re-key `mStreamTransactionHash` from `aOld` to
  `aNew` so `CloseTransaction(realTxn, ...)` finds the stream.

Per protocol:

- **HTTP/3** (`Http3Session::SwapTransaction`) removes the stream
  from `mStreamTransactionHash` under `aOld`, re-inserts under `aNew`,
  calls `stream->SetTransaction(aNew)`, and re-points
  `mFirstHttpTransaction` if it pointed at HT. Post-swap,
  `Http3Stream::mTransaction` is the real transaction, and
  `Http3Session::CloseTransaction(realTxn, ...)` finds the stream in
  the hash.
- **HTTP/2** (`Http2Session::SwapTransaction`) follows the same
  pattern against `mStreamTransactionHash` and
  `Http2Stream::SetTransaction`. The call is gated on
  `GetHttp2Stream()` so tunnel and push streams can't be swapped.
- **HTTP/1** (`nsHttpConnection::SwapTransaction`) has no session and
  no hash. It just sets `mTransaction = aNew` on the connection.
  After this, `nsHttpConnection::OnSocketReadable` calls
  `mTransaction->WriteSegments()` directly on the real transaction.

`Adopt` picks the right `SwapTransaction` by inspecting
`conn->UsingHttp3()` and `conn->UsingSpdy()`, then calls it with
`(this, mRealTxn)`. Before the swap, it also points the real
transaction at the same live connection via `SetConnection`:

```cpp
nsAHttpConnection* ourHandle = Connection();
RefPtr<HttpConnectionBase> conn = ourHandle->HttpConnection();
if (conn->UsingHttp3()) {
  mRealTxn->SetConnection(ourHandle);          // the Http3Session
  Http3Session::SwapTransaction(this, mRealTxn);
} else if (conn->UsingSpdy()) {
  mRealTxn->SetConnection(ourHandle);          // the Http2Session
  Http2Session::SwapTransaction(this, mRealTxn);
} else {
  // H1: establisher's ConnectionHandle will be Reset() shortly after
  // this returns, so mint a fresh handle wrapping the live conn.
  mRealTxn->SetConnection(new ConnectionHandle(conn));
  nsHttpConnection::SwapTransaction(this, mRealTxn);
}
```

The H1 branch mints a fresh `ConnectionHandle` because
`ConnectionEstablisher::FinishInternal` calls `mHandle->Reset()` on H1
shortly after `Adopt` returns. Sharing the establisher's handle would
leave the real transaction's connection ref nulled out. The H2 and H3
branches don't need this. `AddStream` already replaced HT's original
`ConnectionHandle` with the session itself, and the establisher does
not `Reset()` sessions.

### Post-adopt behavior

Post-Adopt, HT is dormant:

- For H2 and H3, the session's `Http{2,3}Stream::mTransaction` now
  points at the real transaction. For H1, the connection's
  `nsHttpConnection::mTransaction` does. Either way,
  `ReadSegments`, `WriteSegments`, `Close`, and `OnTransportStatus`
  go straight to the real transaction.
- HT's `ReadSegments` is only reachable during the RACING phase. It
  routes through `ZeroRttHandle::ReadSegments` when the caller is in
  the 0-RTT flow, and otherwise falls back to
  `SpeculativeTransaction`'s speculative path.
- HT's `WriteSegments` is effectively unreachable post-swap, because
  the session or connection drives `real_txn` directly. It is kept as
  a hard stop. `ZeroRttHandle::WriteSegments` always returns
  `NS_BASE_STREAM_CLOSED`, so any stray call drops bytes rather than
  landing them on HT.
- HT's `Close` keeps its RACING-phase "disqualify non-0-RTT racer"
  logic (see next section). Nothing calls `Close` on the winning HT
  after swap, because the session or connection closes the real
  transaction directly.
- `HT::QueryHttpTransaction()` returns `mRealTxn` so HE's own
  bookkeeping (which still holds HT via `mOutputTrans`) can reach the
  real transaction.

### Disqualifying non-0-RTT racers: `HT::Close`

If a racer attempt started 0-RTT (`mAny0RttStarted` is true) but
*this* HT completed its handshake without entering the 0-RTT flow
(`Request0RttStreamOffset().isNothing()`), then a "successful" Close
of this HT would let it win the normal TCP race. HE would dispatch
the real transaction onto this connection, wasting the single-use PSK
the racer already spent. `HappyEyeballsTransaction::Close` guards
against that:

```cpp
if (NS_SUCCEEDED(aReason) && mZeroRttHandle &&
    mZeroRttHandle->ShouldDisqualify(this)) {
  aReason = NS_ERROR_FAILURE;   // turn success into failure
}
SpeculativeTransaction::Close(aReason);
```

HE's state machine sees the attempt as failed and continues waiting
for a 0-RTT racer.

This guard only fires on `NS_SUCCEEDED(aReason)` closes. Some paths
close an HT with a non-`NS_OK` reason even though the handshake
succeeded. The notable one is `Http2Session::CleanupStream` handing
down `NS_BASE_STREAM_WOULD_BLOCK` on a non-adopted HT. In those
cases, a non-0-RTT racer can reach `OnSucceeded` even though
`mAny0RttStarted` is true. The `OnSucceeded` rewind described below
is the fallback.

### `OnSucceeded` interaction with adoption

When the Rust state machine emits `Succeeded`, `OnSucceeded` sees one
of three variants:

- **Winner was adopted in `Finish0RTT`** (the most common 0-RTT
  path). `mZeroRttHandle->Winner()->IsAdopted()` is true, so
  `ProcessTCPConn` or `ProcessUDPConn` is called with
  `aTransactionAlreadyOnConn = true`. That skips
  `FindTransactionHelper` plus `DispatchTransaction` / `Activate`,
  because the real transaction is already on the connection via
  `SwapTransaction`. The only per-protocol side of dispatch that
  still runs is `InsertIntoActiveConns` plus `ReportSpdyConnection`
  or `ReportHttp3Connection`.

- **Winner won the TCP race but not 0-RTT.** A different racer
  entered 0-RTT (so `mAny0RttStarted` is true and the request stream
  is at that racer's offset), but the eventual winner never called
  `Finish0RTT` (no `mWinner`). This is the fallback for the paths
  where `HT::Close`'s `ShouldDisqualify` guard can't fire, such as
  `Http2Session::CleanupStream` closing a non-adopted HT with
  `NS_BASE_STREAM_WOULD_BLOCK` (not `NS_OK`). Treat it like an
  adopt-reject for the real transaction:

  ```cpp
  if (mZeroRttHandle && mZeroRttHandle->AnyStarted() &&
      (!mZeroRttHandle->Winner() || !mZeroRttHandle->Winner()->IsAdopted())) {
    if (nsHttpTransaction* realTxn = mTransaction->QueryHttpTransaction()) {
      realTxn->FinishAdopted0RTT(/*aRestart=*/true);
    }
  }
  ```

  `FinishAdopted0RTT(true)` rewinds the request stream to 0 and sets
  `mDoNotTryEarlyData` and `mEarlyDataWasAvailable`. The normal
  `aTransactionAlreadyOnConn = false` dispatch then runs, and the
  real transaction sends fresh over the winning connection.

- **No 0-RTT at all.** Same as the non-0-RTT path.
