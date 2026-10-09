kcp-bench
=========

A single-process KCP benchmark harness for
[qtng](https://github.com/hgoldfish/qtng). Both KCP endpoints live in one
process and one event loop, so a run needs no root, no network namespace and no
second binary.

The point is attribution, not a single MB/s number: the output separates
protocol CPU, syscalls, copies/allocation, window/retransmission and queueing so
each optimization can be judged on evidence.

Datagram paths
--------------

- `--link sim` (default): an in-memory `SimLink` pair with a deterministic,
  seedable fault model (delay, jitter, loss, reorder, duplication, corruption
  and an optional serialization bottleneck). Reproducible from `--seed`.
- `--link real`: a real UDP pair on `127.0.0.1`, exercising kernel syscalls and
  socket buffers.

Modes
-----

- `throughput`: time-boxed bulk transfer (`--seconds`, `--warmup`); reports
  goodput and KCP statistics.
- `latency`: ping-pong round trips (`--count`, `--frame-bytes`); reports RTT
  percentiles.
- `probe`: bulk transfer with periodic probes (`--probe-interval`) on the same
  stream; reports load-time RTT. This is the bufferbloat / pacing signal.
- `fault`: integrity check under loss/reorder/dup (`--bytes`, `--iterations`);
  property-based, asserts the received bytes equal the sent bytes and prints the
  failing seed.

Build
-----

```sh
cd examples/kcp-bench
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target kcp-bench -j"$(nproc)"
```

The build pulls in qtng from the repository root (`../../`), exactly like
`examples/kcptun`. Requirements match qtng: C++11, CMake 3.14+, zlib,
OpenSSL or bundled LibreSSL.

Usage
-----

```sh
./kcp-bench --mode throughput --link sim --rtt 80 --loss 0.01 --seconds 10
./kcp-bench --mode latency --link sim --rtt 20 --count 2000
./kcp-bench --mode probe   --link sim --rtt 20 --rate 512K --seconds 5
./kcp-bench --mode fault   --link sim --iterations 20 --bytes 8M
./kcp-bench --mode throughput --link real --sndbuf 8M --seconds 10
./kcp-bench --help
```

Every run can append JSON Lines (`--json out.jsonl`): one object per sampled
interval plus a final `"type":"summary"` object.

Matrix runner
-------------

`run_matrix.py` runs a suite of configurations several times, reduces the
summary metrics to median + MAD and can diff against a stored baseline:

```sh
python3 run_matrix.py --suite quick --samples 3 --out baseline/baseline.json
python3 run_matrix.py --suite phase1 --samples 3 --out baseline/baseline.json
python3 run_matrix.py --suite ablation --samples 5 --base baseline/baseline.json
```

Suites: `quick` (fast smoke), `phase1` (throughput over RTT x loss x direction,
plus latency), `ablation` (flip one knob at a time at RTT 80ms + 1% loss).

Interpreting the stats
----------------------

| Observation | Reading |
| --- | --- |
| `waitsnd` large and `snd_wnd` pinned at the cap | window/budget limited (Tuner, pacing) |
| `waitsnd` small but `delivery_bps` low | application / single-thread / copy limited |
| `rto_resends` + `fast_resends` high | loss/retransmit limited (pacing, fastresend, FEC) |
| `rmt_wnd` small | peer receive window growth limited |

Notes and caveats
-----------------

- **Raw KCP has no checksum.** Corrupted datagrams cannot be detected by the
  protocol, so `--corrupt` is expected to fail the integrity check; real UDP
  relies on the UDP checksum. Corruption is therefore off by default and is an
  informational knob, not a pass/fail gate.
- **Direction matters.** `delivery_bps` in the stats only describes the send
  direction; a download-only stream is receive-window limited instead. Use
  `--direction down` / `bidi` to cover both.
- **Small frames make the harness itself the bottleneck.** At the default
  MTU 1400 each app frame is one KCP segment, so throughput is bounded by the
  per-segment cost (~50k segments/s on the reference machine below). Raise
  `--mtu` to see the protocol's real ceiling. This is a property of the
  measurement, not a bug.
- Every configuration should be run several times (`--samples`) and compared as
  median + MAD; a single run is not a measurement.

Baseline observations
---------------------

On a quiet machine, simulated link, single connection, version 2, Tuner on
(after the compact-ACK fix below; medians of 3 samples):

| Config | goodput | notes |
| --- | --- | --- |
| MTU 1400, RTT 0 | ~55 MiB/s | per-segment/CPU limited; `rto_resends` 0 |
| MTU 9000, RTT 0 | ~221 MiB/s | |
| MTU 60000, RTT 0 | ~418 MiB/s | CPU saturated |
| MTU 1400, RTT 80ms | ~8.7 MiB/s | window/BDP limited |
| MTU 1400, RTT 80ms, 1% loss | ~2.6 MiB/s | retransmit limited |
| probe, RTT 20ms, 512K bottleneck | p99 ~3s | whole-window flush creates large queueing delay |

Single-direction throughput is no longer starved: the Tuner grows the send
window towards the BDP (e.g. 256 -> 2048 -> ~810 segments settling on a
300ms-RTT path), so the ceiling is CPU, as intended.

The large probe RTT under a bottleneck is the direct argument for pacing
(P4): `ikcp_flush` emits a whole window per period, which a shallow-buffered
path turns into queueing delay rather than throughput.

Protocol fix: compact ACKs were holding the peer's window down
--------------------------------------------------------------

The first baseline run showed single-direction
throughput pinned at ~9 MiB/s at MTU 1400 with 19968 spurious RTOs, while
`--version 1` (which disables the version-2 compact-ACK path) reached 55 MiB/s
with zero RTOs.

Cause: `ikcp_flush_acks` kept the whole pending ACK batch until `interval*3` ms
elapsed, including ACKs for data received after the last send. The peer's send
window could not advance, so it RTO-retransmitted almost every segment and
goodput was pinned at `window/(interval*3)` no matter what the send-budget
Tuner computed.

Fix: drain the batch on every flush (like `ackn_mode == 0`); a lost ACKN frame
is recovered by data RTO. `rto_resends` drops from 19968 to 0 and
single-direction goodput rises 6x (9.1 -> 55.3 MiB/s at MTU 1400); the
version-1 vs version-2 gap closes (2.60 vs 1.73 -> 2.70 vs 2.47 MiB/s at RTT
80ms + 1% loss).

Optimization experiments
------------------------

Run against the baseline above.

### P1: socket buffers (real UDP link)

`--sndbuf` sets `SO_SNDBUF`/`SO_RCVBUF` before bind. On the real link
(`--link real --mode throughput`):

| `--sndbuf` | goodput |
| --- | --- |
| auto (kernel default) | ~3.7 MiB/s |
| 64K | ~1.3 MiB/s |
| 1M | ~6.0 MiB/s |
| 8M | ~6.9 MiB/s |

Larger buffers nearly double throughput on the real path; the kernel default is
undersized for a fast link. This is a deployment knob, not a protocol change.

### P2: data-plane work removal (`src/kcp.cpp`)

Three changes, all preserving the wire format:

1. Version-2 output goes straight from `kcp->buffer` to `rawSend(buf + 4, ...)`
   instead of copying into a `std::string` (`makeDataPacket`).
2. `recv()` hands the caller's buffer to `ikcp_recv` when the next message fits,
   skipping the temp string + append + memcpy.
3. `updateKcp()` and `startReceivingCoroutine()` remember whether their coroutine
   is alive instead of doing a string-keyed `CoroutineGroup::get()` scan on every
   send and every received datagram.

Instrumentation (`--profile-alloc`) shows ikcp heap allocations per delivered
MiB, the evidence for segment pooling:

| MTU | goodput | ikcp allocs / MiB |
| --- | --- | --- |
| 1400 | ~9 MiB/s | ~2340 |
| 9000 | ~62 MiB/s | ~357 |

Measured effect of the three P2 changes: **no significant throughput change**
(within run-to-run variance), i.e. the removed copies and lookups are not the
bottleneck at these rates. The changes are kept because they strictly remove
work on the hot path; the real limiter is per-segment protocol/event-loop cost,
which points at pooling and batching rather than copies.

### P3: per-KcpStream segment pool (`src/kcp.cpp`)

`KcpStream` owns a `KcpSegmentPool` and installs it with
`ikcp_set_segment_allocator()`, so ikcp's `ikcp_segment_new/delete` recycle
mss-sized segments instead of calling `malloc/free` twice per packet. The pool is
strictly per-kcp (no lock, no cross-stream sharing); every allocation carries a
size header so a freed block can be matched to the pooled class. `ikcp.c` gains
only a per-kcp allocator slot plus a setter; the pooling policy lives in
`KcpStream`.

`--profile-alloc` reports `kcpSegmentOsAllocationCount()` (real heap allocations
for segments, i.e. pool misses). At mtu 1400 this drops **~2360 → ~37 per
delivered MiB**; throughput is unchanged (the allocator was not the bottleneck,
but the per-packet malloc churn is gone).

### P4: batch receive (`src/kcp.cpp`)

`doReceive` / `doAccept` drain up to 32 datagrams per
`link->recvfrom(std::vector<Datagram>*)` (recvmmsg for UDP), each into a
reusable scratch slot. Batch receive keys on an address-shaped `DatagramPath`
(a `Datagram` only carries addr/port), which is why the benchmark `SimLink`
switched to `127.0.0.1:port`.

Batch *send* was tried and **removed**. A single flush reuses one `kcp->buffer`,
so batching the packets it produces requires copying each into a stable buffer
(`appendOutbound`), and that copy is inherent to deferring a flush's packets to
one `sendto(vector<Datagram>)`. Measured with a temporary switch that forced the
per-packet path on the same binary, batching gave **no throughput gain** (sim
identical, e.g. 143.5 vs 143.9 MiB/s at MTU 1400; real no faster and higher
variance), and `strace` showed `sendmmsg` cut send syscalls ~98% while throughput
was unchanged -- syscall count is not the bottleneck. So the zero-copy
single-packet `rawSend(buf + 4, ...)` was restored.

`Socket::sendto(vector<Datagram>)` / `recvfrom(vector<Datagram>*)` are unchanged;
only the `DatagramLink` batch *send* was dropped. The load generator still sends
in 256 KiB blocks (a receiver-buffer requirement), which is independent of this.

### P5: Tuner follows BBRv1 (`src/kcp.cpp`)

The send window used to be pinned at the 256-segment floor on fast, low-latency
paths: the window is `deliveryBps * srttMin`, but `rate = window / RTT` makes that
a fixed point it can never grow from, and `srttMin` cannot see the pipeline's
serialization delay. A capacity probe was added to fix that (raise a ceiling
while the pipe is full and clean, reset it to zero on congestion).

Both the EWMA rate and that probe are now gone. Bandwidth is the windowed
**maximum** of the delivery-rate samples, the propagation delay is the minimum
srtt over a 10s window, and a Startup/Drain/ProbeBW/ProbeRTT state machine turns
them into a window target. ProbeBW follows an **up-probe ceiling** that is
deliberately independent of the BDP, and the window is bounded by
`setSendBufferLimit`. Loss does not size the window at all -- no threshold, no
multiplicative shrink, no per-period deduction -- and the up-probe ignores it.

That last part is the point. The budget's own target is derived from the achieved
rate, and the achieved rate is set by the window, so anything that shrinks the
window on loss also shrinks the estimate that sizes it. On a high-bandwidth path
with random loss (a WiFi-shaped link) that feedback collapses the window to its
floor and takes throughput with it. The ceiling has no such loop: x5/4 per period
while the pipe is full and the queue empty, x9/10 when a queue stands, never
below the BDP. It is followed **directly** rather than through the stepper's 1/8
gap-closing, because two cascaded rate limiters mean the slower one decides --
the ceiling climbs at 25% per period while the budget trails at ~7%, so the
probe never reaches the path.

Measured on a WiFi-shaped simulated link: `--rtt 30 --rate 12M` (12.0 MiB/s),
`--seconds 10`, single connection, median of 5. `tuner=off` is the control: it
never enters the Tuner, so it is a fixed 256-segment window with fast retransmit
disabled -- no feedback loop at all.

| loss | before (BFB tuner) | after | tuner=off | after vs off | after vs before |
| --- | --- | --- | --- | --- | --- |
| 0 | 10.53 | 9.15 | 9.50 | no | 0.87x |
| 1% | 2.02 | 7.58 | 4.73 | **yes** | 3.75x |
| 5% | 0.83 | 6.06 | 3.19 | **yes** | 7.31x |
| 10% | 0.47 | 5.05 | 2.32 | **yes** | 10.73x |
| 20% | 0.26 | 3.51 | 1.28 | **yes** | 13.72x |

Sampled ranges: `loss 0` 8.75-9.62 vs 10.26-10.97; `1%` 7.07-7.61 vs 1.81-2.06;
`5%` 5.80-6.73 vs 0.78-0.90; `10%` 4.84-5.47 vs 0.38-0.54; `20%` 1.49-3.90 vs
0.24-0.57. The lossy columns do not overlap.

**The no-loss bucket is a regression, and it is the one failing criterion.** A
second, independent 5-sample run reproduced the same ratio: `10.65 -> 9.28`
(0.87x, ranges 8.83-10.94 vs 8.38-9.32). The target was "every lossy bucket beats
`tuner=off`, and no-loss does not regress"; only the first half is met. Note the
control is deterministic here -- `tuner=off` gives exactly 9.50 on every run --
so `9.28 < 9.50` is a real 2% shortfall against the control as well, not noise.
The cause is measured, not guessed: on a rate-limited link an oversized window
costs queueing delay and a sawtooth, while on an unthrottled link it is free,
which is why the same ceiling is a large win in the guard table below. Two
candidate fixes were tried and rejected on evidence:

- A "stop when the achieved rate stops improving" gate fixes `loss 0` (10.00) but
  wrecks the lossy buckets (5%: 6.13 -> 3.14, below `tuner=off`), because under
  loss the rate does not improve with a larger window even though a larger window
  is what the path needs.
- Tightening the queue threshold cannot separate the two cases: measured queueing
  delay is 39ms median at `loss 0` and 61ms at 5% (p90 85ms vs 308ms), so no
  threshold admits one and rejects the other.

Pacing is what actually resolves this (an oversized window would not waste the
link if it were not sent as a burst), and `--mode probe` still shows the queueing
that motivates it. The `--suite wifi` matrix reproduces the table above.

The same ceiling also removes the regression this section used to document. On
the unthrottled link (no `--rate`, so a large window is free) the guard configs,
`--seconds 10`, median of 3:

| guard | before | after | ratio |
| --- | --- | --- | --- |
| up, RTT 0 | 10.60 | 233.30 | **22.02x** |
| up, RTT 80ms | 85.18 | 133.56 | 1.57x |
| up, RTT 80ms + 1% loss | 3.62 | 42.72 | **11.79x** |
| down, RTT 80ms | 75.21 | 127.20 | 1.69x |
| bidi, RTT 80ms | 132.59 | 185.64 | 1.40x |

RTT 0 was the worst case of the BDP feedback loop described below and is now the
largest win, because ProbeBW no longer sizes its target from the rate-derived
BDP at all.

**The fixed point still governs Startup and Drain.** A windowed maximum only
stops a single low period from dragging the estimate down; `rate = window / RTT`
still couples the BDP to the window, and Startup's and Drain's targets are
`bdpSegs * gain`. ProbeBW is the state that escapes it, because its ceiling is
not derived from that estimate. Before the ceiling existed, all four states
derived from the BDP and a low-RTT path fell into a down-staircase: an RTT 0 run
sent the budget through 256 -> 81 -> 135 -> 96 -> 72 -> 56, still falling when the
run ended (each figure is one sampler tick, so read the shape, not the exact
values). The numbers below are from that build; the current one is measured in
the WiFi table above and in the guard table below.

Measured on the sim link, MTU 1400, single connection, `--seconds 6` (median of
3-5 samples), pre-ceiling build:

| Scenario | probe (before) | BBRv1 (after) |
| --- | --- | --- |
| up, RTT 0 | ~226 MiB/s | **~21 MiB/s** |
| up, RTT 80ms | ~51 MiB/s | ~78 MiB/s |
| up, RTT 80ms + 1% loss | ~3.7 MiB/s | ~2.6 MiB/s |
| down, RTT 80ms | ~51 MiB/s | ~80 MiB/s |
| bidi, RTT 80ms | ~77 MiB/s | ~97 MiB/s |
| probe, RTT 20ms, 512K | 0.14 MiB/s | 0.18 MiB/s |
| latency, RTT 20ms | p99 26ms | p99 26ms |

**Duration changes the verdict on two rows.** Re-measured at `--seconds 30`:

| Scenario | probe (before) | BBRv1 (after) |
| --- | --- | --- |
| up, RTT 0 | ~317 MiB/s | **~6.9 MiB/s** |
| up, RTT 80ms + 1% loss | ~2.9 MiB/s | **~7.1 MiB/s** |

- RTT 0 is the feedback staircase described above, and it gets *worse* with time
  (6s ~21 -> 30s ~6.9) while the probe build grows (226 -> 317). It is not a
  loss effect: `rto_resends` is 0 for the whole BBR run. This is a real
  regression.
- RTT 80ms + 1% loss is the opposite: the 6s row is a transient. BBR has to grow
  its window back after the loss knocks Startup into ProbeBW, which takes tens of
  seconds, and at 30s it is ~2.4x *faster* than the probe build. Reporting only
  the 6s row here would be misleading in both directions.

Decomposing the 1% loss penalty (medians over the sampled ticks): the
window is 10218 segments with no loss and 848 at 1% loss, while delivered
bytes/s is 73% of what the window could carry with no loss and 59% at 1% loss.
So the throughput drop is ~12x from the window and only ~1.24x from retransmit
efficiency. The window is small because the target is `gain * btlBw * srttMin`
and `btlBw` is the *achieved* rate, which loss holds down -- a feedback through
the rate estimate, not a loss term in the controller. Loss repair is
overwhelmingly RTO (see "Loss recovery is dominated by RTO retransmission"
below), so part of that efficiency loss is RTO stalls.

The RTT 80ms gains come with far more self-inflicted loss (the RTO-retired ratio
rises from ~0.07 to ~0.53, and ~1.3 on the downlink, with `rto_resends` up ~8x).
That is the missing pacing: the whole window still leaves in one flush. The
up-probe described above is what removed the RTT 0 regression, so pacing is now
the only one of the two left open.

The sim link has no bandwidth limit, so these numbers reward a large window and
should be read as an upper bound, not as a prediction for a real bottleneck.

### Loss recovery is dominated by RTO retransmission

`fast_resends` is 0 only when there is nothing to repair or when the packet rate
is too low to reach the threshold. When acks are batched into `IKCP_CMD_ACKN`
frames, `ikcp_ack_feed` folds every ack in a datagram into a single `maxack` and
`ikcp_parse_fastack` runs once per `ikcp_input()` call, so `fastack` advances at
most 1 per received datagram rather than 1 per acked segment. That does not make
fast retransmit impossible -- it makes it a function of the packet rate: the
threshold of 16 needs 16 datagrams to arrive within one RTO, which is a few
milliseconds at the rates this Tuner now reaches. Measured with `fastresend = 16`:

| config, 8s | goodput | rto_resends | fast_resends |
| --- | --- | --- | --- |
| throttled 12M, RTT 30ms, 1% loss | 7.1 MiB/s | 19912 | 364 |
| unthrottled, RTT 30ms, 1% loss | 67.2 MiB/s | 125578 | 1709 |
| unthrottled, RTT 30ms, 5% loss | 33.8 MiB/s | 77937 | 3661 |
| unthrottled, RTT 80ms, 1% loss | 40.5 MiB/s | 50862 | 645 |
| unthrottled, RTT 0, 1% loss | 80.7 MiB/s | 2720 | 6060 |
| unthrottled, RTT 0, no loss | 213.4 MiB/s | 15034 | 0 |

So the fast path is live whenever there is loss, but it is still a small share of
recovery: 1.3% at RTT 30ms + 1% loss. Loss repair here is overwhelmingly RTO, and
that is what a loss costs.

It also costs far more retransmits than the loss rate implies. Counting directly
from the counters rather than deriving them, per 8s run:

| config | sent_segs | link drops | rto_resends | RTO per drop |
| --- | --- | --- | --- | --- |
| unthrottled, RTT 30ms, 1% loss | 380268 | 5209 | 121408 | **23.3** |
| unthrottled, RTT 30ms, 5% loss | 194778 | 13855 | 68845 | **5.0** |
| unthrottled, RTT 80ms, 1% loss | 219365 | 2706 | 38863 | **14.4** |

A dropped segment should cost on the order of one retransmit. Two things about how
to read the ratio, both checked against the code and the counters rather than
assumed:

- `rto_resends` counts **segments** retransmitted by the RTO timer, not timer
  firings: `kcp->xmit++` sits inside `ikcp_flush`'s per-segment loop, guarded by
  that segment's `resendts` having passed. Since a window sent in one burst
  shares one `resendts`, a single expiry retransmits the whole outstanding
  window. `sent_segs`, by contrast, is first-sends only (`xmit == 0`).
- The denominator is not diluted by ack traffic. `packets_dropped` counts every
  datagram the simulated link drops in both directions, but at these rates it
  matches "1% of the data datagrams" within noise (6042 observed against 5878
  expected), so the ratio is effectively per dropped data segment.

So at RTT 30ms + 1% loss the timer retransmits about 23 segments for every one
that was actually lost, and the ratio is worse at 1% loss (19-23) than at 5%
(3.7-5.0). That is the headroom: not the fast-retransmit threshold, which
accounts for ~1% of repairs, but how much is re-sent per expiry and how often an
expiry happens, neither of which is established yet.

An earlier revision of this section claimed `fast_resends` was always 0 and
concluded that recovery was RTO-only by construction. That was measured on the
build before the up-probe, whose low throughput kept the packet rate under the
threshold; the conclusion held there and does not hold now.

Counting per acked segment was implemented, measured, and reverted. Keep the
result before trying again:

| 20s, RTT 80ms | fast_resends | goodput (median of 3) |
| --- | --- | --- |
| 1% loss, per-datagram (current) | 0 | 7.4 MiB/s |
| 1% loss, per-segment | ~1200 | 7.4 MiB/s |
| 10% loss, per-datagram (current) | 0 | 1.01 MiB/s |
| 10% loss, per-segment | ~550 | **0.58 MiB/s** |

At 1% loss the change is neutral; at 10% it costs ~43%, reproducibly (ranges
1.16/1.01/0.66 vs 0.59/0.49/0.58) even though total retransmits fall (RTO resends
~5100 -> ~2400). The window is what moves: the send budget collapses from
595-1024 to 117-126 segments, the same rate-derived-BDP feedback noted above --
enabling the fast path perturbs the achieved rate, so the window target shrinks.
Integrity held at 5/10/30% loss (`integrity_ok`), and no connection was torn
down early, but note that `segment->xmit` counts fast and RTO retransmits
together against `dead_link = 10`, so making the fast path live widens the set
of paths that can reach a dead-link teardown.

The attempted change is preserved as git tag `backup-fastack-attempt` and as hg
strip backup bundles under `.hg/strip-backup/`; the per-segment counting, the
duplicate-ack collapse and its whitebox test are all in that commit if this is
revisited (for instance together with pacing, which would change the retransmit
economics).

### Measurement caveat

Throughput at `mtu 1400` varies enough between runs (±20-90% on the lossy
configs) that 2-3 samples cannot reliably detect a change smaller than ~10%.
For regression gating, use more samples and a quiet machine; the
`--samples` median+MAD columns make the spread visible. The reproducible
configs (unloaded, `mtu 1400`, RTT 0/80) are the ones to watch.

### Not yet implemented

P4 (pacing) is designed and reproduced by `--mode probe` (p99 ~3s under a
512K bottleneck, the same figure as the baseline table above), but a correct
per-flush pacing budget inside `ikcp_flush` is a protocol change that needs more
validation than this harness alone can give; it is left as the next step rather
than shipped unvalidated.

