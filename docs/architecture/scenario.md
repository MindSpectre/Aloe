# Scenario Module

The scenario module (`fixtures/scenario/`) runs scenario scripts in packetdrill's syntax against the TCP brick:
`tcp::Stack` over `net::Ipv4<fabric::Port>` on the [fabric](fixtures.md), with no root, no hugepages and no
network card. A script injects the peer's segments at given times, asserts what the stack sends and when, and
drives the application side with socket calls the runner binds to the brick's verbs. Everything is reached
through the umbrella `#include <aloe/scenario>` (`export/aloe/scenario`), and targets link
`Aloe::Fixtures::Scenario`. It depends on [`tcp`](tcp.md), [`net`](net.md), [`loop`](loop.md), the `fabric` and
`frames` [fixtures](fixtures.md), [`wire`](wire.md) and [`core`](core.md), and contains no gtest: it reports
results as values.

The scripts are Aloe's own. Linux's packetdrill corpus is the reference for what Linux does, read and never
copied: packetdrill's scripts and the kernel's in-tree ones are GPL-2.0, and Aloe is BSD-3-Clause. A script is
written from the RFCs and from what Aloe does, and its opening comment says where Linux would differ.

## Key types

- **`aloe::scenario::Script`**, **`parse_script(text)`**, **`ParseError`** -- the parsed model: the events in
  order, each with its `TimeSpec`, its 1-based source `line` and either a `PacketLine` or a `CallLine`; the
  script's `tolerance` when it sets one; the `TcpConfig` fields its `--tcp_*` lines set (`ScriptTcpOptions`);
  and the policies it runs under (`CongestionChoice`: `NewReno`, `None` or `Both`). `parse_script` returns
  `std::expected<Script, ParseError>`; a `ParseError` carries the line, the column and what was wrong.
- **`aloe::scenario::Harness`**, **`HarnessConfig`** -- a fabric with the stack's port and the peer's port, and
  the bricks over queue 0 of the stack's port, `ShardQueue`, `Ipv4`, `TimerWheel` and `tcp::Stack`, with both
  MACs learned. The defaults put the stack at 10.0.0.2 with port 8080 for `bind` and the peer at 10.0.0.1 with
  port 40000 and the initial sequence number 1000000; they give the fabric no checksum offloads (`offloads`
  picks `EmulatedOffloads::None` or `Checksums`), the wheel a resolution of one millisecond, TCP its default
  `TcpConfig`, which `configured(config, script)` overrides with the script's `--tcp_*` lines, and ARP a
  lifetime of an hour (`arp_lifetime` is `arp_reachable`; `arp_expire` is twice it).
  `inject`, `step`, `flush` and `collect` are the parts the runner composes; a `Collected` is one frame the
  peer's port received, parsed, with a verdict on each checksum the stack wrote.
- **`aloe::scenario::Runner`**, **`RunnerConfig`**, **`Failure`**, **`format(failure, path)`** -- `Harness` and
  `Runner` run the brick under `tcp::NewReno`; `NoCongestionHarness` and `NoCongestionRunner` are the same
  classes over `tcp::NoCongestionControl` (`BasicHarness` and `BasicRunner` are the templates). The scripts
  binary registers each script once per policy its `--congestion` line allows and once per checksum mode, as
  `<path>/<NewReno|NoCongestion>/<NoOffloads|Checksums>`. `Runner{harness, script, config}.run()` executes the
  script once and returns `std::expected<void, Failure>`. `RunnerConfig` holds the `tolerance`, one millisecond
  unless the script's `--tolerance_usecs` says otherwise, and `any_bound`, how long a `*` line waits, 120
  seconds. A `Failure` is the script line and a message in the script's notation; `format` writes it as
  `path:line: message`.
- **`aloe::scenario::Peer`**, **`ObservedSegment`** -- the other end's sequence bookkeeping. `observe` turns a
  frame the stack sent into an `ObservedSegment` in the script's relative numbers, fixing the stack's initial
  sequence number and its port from the first segment it sends with SYN set, its SYN or SYN-ACK; `sequence` and
  `acknowledgement` map a script's inbound relative numbers to absolute ones. `outbound_sack_base()` is the
  peer's initial sequence number, the base of sack blocks in segments the stack sends; `inbound_sack_base()` is
  the stack's, or zero until it is known, the base of sack blocks a script injects. `observe` also fixes the
  stack's timestamp base, `stack_timestamp_base()`, the value of the first segment the stack sends with the
  option, and writes every outbound `TS val` relative to it; `inbound_timestamp_base()` is that base, or zero
  until it is known, which an inbound line's `ecr` is relative to. `ObservedSegment` lives in `script.hpp`,
  beside the packet line it is matched against.
- **`aloe::scenario::match(line, seen, sack_base)`** -- nothing when the segment satisfies the line, else the
  first difference: `flags differ`, `sequence differs`, `ack differs`, `window differs`, `urgent differs` or
  `options differ`.
- **The notation** -- `describe(line, sack_base)` and `describe(segment, sack_base)` write a packet line or a
  segment the stack sent as a script would, `> S. 0:0(0) ack 1 win 65535 <mss 1460,nop,nop,TS val 0 ecr 100>`;
  `encode_options(options, sack_base, timestamp_echo_base)` and `describe_options` turn an option list into
  header bytes and back; `format_time` writes a stamp as seconds since the script's start, `+0.010000`.

## Usage

Aloe's scripts run through one test binary, but the module needs no gtest. A test of any framework parses a
script, builds a harness and runs it:

```cpp
#include <aloe/scenario>

const auto script = aloe::scenario::parse_script(text);
if (!script) {
    std::println("{}:{}: {}", path, script.error().line, script.error().message);
    return false;
}
aloe::scenario::Harness harness{aloe::scenario::HarnessConfig{.offloads = aloe::fabric::EmulatedOffloads::Checksums}};
aloe::scenario::Runner runner{harness, *script};
if (const auto result = runner.run(); !result) {
    std::println("{}", aloe::scenario::format(result.error(), path));
    return false;
}
return true;
```

A harness and a runner serve one script; the next script gets fresh ones.

## The script language

The runner accepts this subset of packetdrill's syntax and fails a script at parse time, naming the line and
the column, for anything outside it:

- **Comments and blank lines.** `//` to the end of the line, inside a quoted string too.
- **Option lines** before the first event, each at most once: `--tolerance_usecs=n`; `--tcp_rto_min_ms`,
  `--tcp_rto_max_ms`, `--tcp_rto_initial_ms`, `--tcp_user_timeout_ms` (milliseconds), `--tcp_retries`,
  `--tcp_data_retries`, `--tcp_retransmit_defers` (0 to 255), `--tcp_send_segments`, `--tcp_timestamps=0|1`,
  `--tcp_receive_segments`, `--tcp_receive_window`, `--tcp_sack=0|1`, `--tcp_window_scaling=0|1` and
  `--tcp_delayed_ack_ms`, which set the harness's `TcpConfig` for that script (`configured(config, script)` applies
  them); and
  `--congestion=newreno|none|both`, the policies the script runs under (`both` when absent). Any other name is a
  parse error, and so is an option line after the first event, so the dialect grows deliberately.
- **Backtick lines** are a parse error: the runner has no shell, and what a corpus script sets that way is the
  harness's configuration.
- **Event times.** `+t` after the previous event, `t` since the script's start, a window `t1~t2` or `+a~+b`, and
  `*` for any time, with `t` an integer or a decimal number of seconds.
- **Packet lines.** `<` for what the peer injects, `>` for what the stack must send. Flags from the letters `S`,
  `F`, `R`, `P` and `U`, with `.` for ACK alone or as a suffix (`S.`, `P.`, `F.`, `R.`) and `-` for none; then
  `first:last(length)`, where `last - first` must equal `length`; then `ack n`, `win n` and `urg n`, each at most
  once; then an option list in angle brackets, comma-separated, of `mss n`, `nop`, `eol`, `sackOK`, `wscale n`,
  `sack a:b c:d` and `TS val v ecr e` in any order, or `<...>` for any options. A sack option's blocks are
  separated by spaces, as in packetdrill, since a comma starts the next option: `<sack 1001:2001 3001:4001,nop,nop>`.
- **Call lines.** `name(arguments) = result`, with an errno word after a `-1` and an optional `(text)` after
  that, as `= -1 EINPROGRESS (Operation now in progress)`. Arguments are integers, `...`, bare words
  (`SOCK_STREAM`, `IPPROTO_TCP`, `SO_REUSEADDR`, `O_RDWR|O_NONBLOCK`), a bracketed value (`[1]`) or a quoted
  string. The parser keeps calls generic; the runner decides which names it binds.

IPv4 header information in parentheses or brackets, `udp`, `icmp`, `gre` and `mpls` lines, unknown flag letters
and unknown TCP options are parse errors too, and so is anything left on a line after what it should hold.

## The binding to the brick

A script is written from the stack's side, as packetdrill's are: `>` is what the stack sends, `<` what the peer
injects, and the calls are the application. The runner keeps its own table of descriptors, numbered as the script
numbers them, and binds the calls to the brick's verbs. A call it does not bind fails the script with its name
and line, never silently:

| Call                                                                     | Binding                                                                                                                                                                                      |
|--------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `socket(..., SOCK_STREAM, IPPROTO_TCP) = n`                              | Opens descriptor `n`, bound to nothing yet. A negative `n` or one in use fails.                                                                                                              |
| `setsockopt(n, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0`, and `TCP_NODELAY` | Accepted and ignored: Aloe reuses ports and has no Nagle. Any other option fails as `unbound call setsockopt(<option>)`.                                                                     |
| `fcntl(n, F_SETFL, O_RDWR\|O_NONBLOCK) = 0`                              | Accepted and ignored: every verb is non-blocking.                                                                                                                                            |
| `bind(n, ..., ...) = 0`                                                  | Binds `n` to the harness's `stack_port`.                                                                                                                                                     |
| `listen(n, backlog) = 0`                                                 | `tcp.listen(port)` on a bound socket, which becomes a listener. A `ListenError` fails with its name.                                                                                         |
| `accept(n, ..., ...) = m`                                                | Takes the oldest connection the stack raised `Accepted` for and makes it descriptor `m`. With none pending the call is unsatisfied.                                                          |
| `connect(n, ..., ...) = 0`, or `= -1 EINPROGRESS (...)`                  | `tcp.connect` to the harness's peer at the call's stamp; `n` becomes the connection. A `ConnectError` fails with its name.                                                                   |
| `write(n, ..., count) = m`, `send(n, ..., count, 0) = m`                 | `prepare` and `commit` while `prepare` allows, up to `count` bytes filled with `frames::pattern`. Passes when the bytes accepted equal `m`, which may be fewer than `count`.                 |
| `read(n, ..., count) = m`, `recv(n, ..., count, 0) = m`                  | `m > 0` needs `min(unread, count) == m` and consumes `m` bytes; `m == 0` needs the peer's FIN with nothing unread; `-1` needs its errno word's condition. Nothing unread yet is unsatisfied. |
| `close(n) = 0`                                                           | On a listener, `unlisten`. On a connection, `close()`, a FIN after what was committed; the slot is released at the connection's terminal event.                                              |
| `shutdown(n, SHUT_WR) = 0`                                               | `close()` on the connection, which stays readable.                                                                                                                                           |
| anything else                                                            | Fails as `unbound call <name>`: `getsockopt`, `poll`, `epoll_*`, `ioctl`, `sendfile` and the rest.                                                                                           |

The errno words a `read` or `recv` of `-1` may expect: `EAGAIN` (a live connection, nothing unread, the peer
open), `ETIMEDOUT` (the connection ended `TimedOut`), `ECONNRESET` (`Reset` after it was established),
`ECONNREFUSED` (`Reset` before it was, a RST answering our SYN) and `EHOSTUNREACH` (`Unresolved`); any other
word fails as `unbound errno <word>`. A `read` that asks for data of a connection that has ended fails with the
event that ended it. An unsatisfied `accept`, `read` or `recv` fails as `<name>: not satisfied:` and what it found
at the descriptor: the unread count, whether the peer closed and the state, or the event that ended it.

The peer's side follows from the harness. Inbound lines become frames from the peer's MAC, address and port to
the stack's `stack_port`, or to the stack's ephemeral port once its SYN has been seen, with a window of 65535
when the line writes none and a payload of `frames::pattern`. The peer's initial sequence number is not zero, so
a stack that assumed zero would fail. Inbound sequence numbers are relative to it, and inbound acknowledgements
to the stack's initial sequence number, read from its SYN or SYN-ACK. Outbound numbers are checked the other way
round. Before the stack's number is known, an inbound acknowledgement and an outbound sequence number are taken
as written, which is why a script that sees no handshake writes absolute numbers. An inbound line must write its
options rather than `<...>`, as whole 32-bit words (`pad with nop`), and cannot carry `urg`, which the frame
builder has no field for. ARP never appears in a script: both MACs are learned before it starts, with lifetimes
of an hour, beyond the `*` bound and the longest retry schedule.

Timestamps are relative too, to the value of the first segment the stack sent with the option, its SYN or
SYN-ACK. An outbound `TS val` is matched less that base, so the SYN shows `TS val 0` and a segment sent 200 ms
later `TS val 200`, exact on the virtual clock; an outbound `ecr` is matched as written, the value of the inbound
line it echoes. An inbound `TS val` is injected as written, and an inbound `ecr` is written relative to the same
base, zero until the stack has sent one, which the runner adds back, so a script echoes the stack's `TS val` as
its outbound lines show it.

## Time and matching

**The clock is virtual.** It starts at the epoch and moves only as the script asks, one wheel resolution at a
time. One step is: `now += resolution`, `fabric.advance`, a receive pass through IP and TCP at the new stamp over
whatever the fabric delivered to the stack's port (usually nothing), `wheel.advance`, a drain of the stack's
events (which binds them to the descriptors and releases a connection whose terminal event arrived),
`tcp.flush`, `queue.flush`, and then every frame the peer's port received joins the runner's queue of sent
segments, stamped with `now`.

**Inbound lines and calls act at their nominal time,** with no tolerance: the runner steps up to that stamp,
injects or calls, then drains, flushes and collects at the same stamp, so a `+0 >` line right after one is
checked against what that action produced. A call runs after the step's process, wheel advance and drain, so a
call written at a timer's nominal time finds the timer's event. An `accept`, `read` or `recv` written with a
window is retried once per step until its result holds or the window closes, which is how a script waits for an
event without knowing its exact tick; every other call runs once, at the window's start.

**An outbound line takes the head of the queue.** The runner steps until the queue is not empty or the line's
window has closed, then matches the head against the line and checks that its stamp lies in the window, widened
by the tolerance on both sides. A relative time counts from the previous event's actual time: for an outbound
line, the stamp of the segment it matched; for the others, the stamp of the action. A `*` line's window runs
from the script's start to `now` plus `any_bound`, so a segment that was already waiting is never early for it.

**Segments wait in order, as in packetdrill.** The queue keeps what the stack sent with its stamps, and outbound
lines take segments in order across the inbound lines and calls between them, so the three segments of one
`write` can be acknowledged one by one. A segment the script did not expect is caught in one of three places:
at the next outbound line, whose head it is and does not match; by its stamp, outside the window of the line it
would have matched; or at the end of the script, where a queue that is not empty fails it.

**Matching.** An outbound line matches a segment when every field it states agrees: the flags exactly; the
relative range and the length; `ack`, `win` and `urg` when written; the option list byte for byte when written,
any options for `<...>`, and no options when none is written. Ports and addresses are not matched, as
packetdrill does not match them. The IPv4 and TCP checksums of every segment the stack sends are verified
whatever the line says, and every script runs in both checksum modes of the fabric, so a script that passes has
checked the software path and the offload path.

A failure names the line, what was expected and what the stack sent, both in the script's notation, with stamps
in seconds since the start and the window as written, before the tolerance:

```
tests/scenarios/tcp/close/remote-fin-then-local-close.pkt:13: expected "> F. 1:1(0) ack 2 win 65534 <nop,nop,TS val 0 ecr 100>" at +0.000000; the stack sent "> . 1:1(0) ack 2 win 65534 <nop,nop,TS val 0 ecr 100>" at +0.000000: flags differ
```

A mismatch says so first: `early:`, `late:`, `missing:`, `unexpected:` or `bad checksum:`. A call failure (`unbound call ...`, `read: not satisfied: ...`) names the call instead.

## Limits

What the runner does not model yet, which a script author needs to know:

- The runner releases a connection's slot at its first terminal event, `Closed`, `Reset`, `TimedOut` or
  `Unresolved`, so whatever was unread is gone after it. After a `shutdown`, the peer's data followed by its FIN
  takes the connection to `Closed`, and a later `read` fails where Linux would return the data.
- A closed connection's descriptor keeps its number until its terminal event, which frees it (closing a listener or a
  socket frees it at once): `accept(3, ..., ...) = 4` after `close(4)` fails with descriptor 4 in use while the FIN
  exchange is still open. Number the next connection 5.
- `close` in SynSent does nothing, since the brick's `close()` acts in Established and CloseWait only; closing a
  listener leaves the connections it had accepted and `accept` had not taken where they are.
- `close` on an established connection sends a FIN even with unread data. The reset Linux sends in that case
  (RFC 2525, 2.17) is a question for the lifecycle step of phase 2, and no script closes with unread data.
- The runner does not advance the clock after the script's last line, so a script that asserts the stack stays
  silent for a while needs a later line to take the clock there.
- `send` and `recv` flags must be `0`, `shutdown` takes only `SHUT_WR`, and `setsockopt` binds only
  `SO_REUSEADDR` and `TCP_NODELAY`.
- An inbound option list must fit TCP's 40 bytes; the runner does not check that yet, and the frame builder's
  assertion is what stops a longer one.
- A script drives one connection: the peer takes the stack's initial sequence number, its port and its timestamp
  base from the first SYN or SYN-ACK the stack sends.
- A `write` cannot wait. The brick has no send buffer, so a `write` accepts what the windows and the send cap
  allow at its stamp, the script says how much with its result, and it writes the rest after the ACK it injects;
  a `write` that waited for a later line's ACK could never finish in a runner that runs every line in order.
- The busy rule's deferral and copy cannot be scripted: they need a port that holds its loans and a
  `return_loans` between lines. They are unit tests over a fabric port with `LoanReturn::Held`.

## Writing and running a script

A script lives under `tests/scenarios/tcp/<area>/<name>.pkt`. It starts with a comment saying what it asserts
and, where Linux would behave differently today, how. It writes `win` on outbound lines where the value is known,
since a field a line leaves out is not checked. Nothing else registers it: the scripts binary finds every `.pkt`
under `tests/scenarios/` at run time and registers one test per policy its `--congestion` line allows and per
checksum mode of the fabric, named `Scenario.tcp/<area>/<name>/<NewReno|NoCongestion>/<NoOffloads|Checksums>`:
four for a script that names no policy, two for one that names one. CTest lists one entry, the binary, as it does
for every test binary; a gtest filter selects an area, a script or a policy:

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Scenarios
./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/*'
./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/rto/fin?retransmitted*'
./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.*/NoCongestion/*'
```

In a filter, `-` starts the patterns to exclude, so a name with a dash in it is matched with `?` in its place. A
failing script prints `path:line: message` with both segments in the script's notation, as above.

## The scripts

Each script asserts the TCP of today: `<mss 1460,nop,nop,TS val 0 ecr ...>` on SYNs and SYN-ACKs and
`<nop,nop,TS ...>` on every later segment of a connection that negotiated timestamps; the SYN's schedule of 1, 3,
7, 15 and 31 seconds after the first send with `TimedOut` at 63; data and the FIN resent on the measured RTO,
from 200 ms after a handshake with no round trip; NewReno's windows where a script names it; and no delayed ACK.
A script that names no policy runs under both. Its opening comment says what it pins, with the arithmetic
behind every number.

| Script                                              | What it pins                                                                                                                                  |
|-----------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| `handshake/passive-open`                            | Listen, SYN in, SYN-ACK out with `<mss 1460,nop,nop,TS val 0 ecr 100>` and the whole byte window, ACK in, `accept`; then the listener closes. |
| `handshake/active-open`                             | `connect` returns `EINPROGRESS`, SYN out with the MSS and the timestamps, SYN-ACK in, ACK out at the flush echoing the peer's value.          |
| `handshake/syn-retransmitted-on-schedule`           | No answer: the SYN again 1, 3, 7, 15 and 31 seconds after the first, each with the clock's value then, and `read` reports `ETIMEDOUT` at 63.  |
| `handshake/syn-ack-retransmitted-on-schedule`       | The passive side's backoff: the SYN-ACK again 1, 2 and 4 seconds apart, then the late ACK, echoing the last, completes the handshake.         |
| `handshake/syn-to-closed-port-is-reset`             | No listener: `R.` with sequence 0, acknowledging the SYN.                                                                                     |
| `handshake/stray-ack-in-listen-is-reset`            | An ACK to a listening port with no connection: `R` at its acknowledgement number.                                                             |
| `data/inbound-data-is-acked-at-flush`               | Data in, a pure ACK out at `+0` with the window less what is unread; `read` reopens it and the update leaves at the flush.                    |
| `data/write-segments-by-peer-mss`                   | The peer's SYN carried `mss 1000`: a `write` of 2500 leaves as three `P.` of 988, 988 and 524, the MSS less the timestamp option.             |
| `data/duplicate-is-acked-at-once`                   | A segment that arrives twice is answered at once with the same acknowledgement, and `read` sees its bytes once.                               |
| `reassembly/gap-filled-delivers-the-run`            | A segment above a gap is held and answered at once with the window restated; the one that fills the gap delivers both, acked at once.         |
| `reassembly/overlap-is-trimmed`                     | Two held segments overlap in part: the held bytes win, the second is kept from the first one's end, and the run reads 2500 bytes.             |
| `reassembly/covering-segment-replaces-a-held-one`   | A segment above a gap that covers a held one from its first byte replaces it; the gap filled, the run reads 2000 bytes.                       |
| `reassembly/fin-above-a-gap`                        | A FIN above a gap is remembered: the duplicate ACK stays at the gap, and the FIN is acknowledged only once the gap fills.                     |
| `reassembly/cap-drops-the-highest`                  | `--tcp_receive_segments=4`: the lower sequence wins at the cap; a new highest is dropped, a lower one or in-order data takes its node.        |
| `close/local-close-then-remote-fin`                 | `close`, FIN out, ACK in, the peer's FIN in, ACK out at once; no TIME_WAIT.                                                                   |
| `close/remote-fin-then-local-close`                 | The peer's FIN in, ACK out at `+0`, `read` returns 0, `close`, FIN out, ACK in; a later segment is reset.                                     |
| `close/close-at-zero-window-holds-fin`              | `close` at a zero window: no FIN, a probe at +0.2, the answer opens the window and the FIN leaves at once; the peer's ACK and FIN end it.     |
| `close/fin-waits-behind-a-full-window`              | `close` with data filling the window: the FIN waits, unsent, until the ACK of the data, 100 ms later, makes room, then leaves at once.        |
| `reset/rst-in-established`                          | `R.` in, without timestamps: `read` fails with `ECONNRESET`, the slot is released, and a later segment is answered with `R`.                  |
| `reset/rst-to-syn-is-refused`                       | `connect`, SYN out, `R.` in: `read` reports `ECONNREFUSED`.                                                                                   |
| `rto/data-retransmitted-after-rto`                  | One segment never acknowledged: resent at the RTO, 200 ms after a zero round trip, then at twice that; the ACK of the resend ends it.         |
| `rto/backoff-ends-in-timeout`                       | `--tcp_data_retries=3`: resends at +0.2, +0.4 and +0.8, and the fourth expiry, 1.6 s later, ends it; `read` reports `ETIMEDOUT`.              |
| `rto/karn-skips-a-retransmitted-sample`             | `--tcp_timestamps=0`: the ACK of a resend gives no sample, so the next resend waits the backed-off 400 ms; a new ACK resets the RTO.          |
| `rto/echo-sample-ends-the-backoff`                  | The Karn script with timestamps: the ACK of a resend echoes it and gives a sample, so the backoff ends and the next resend comes at +0.2.     |
| `rto/partial-ack-resends-the-tail`                  | A partial ACK, then the timeout: the resend carries only the unacknowledged bytes.                                                            |
| `rto/fin-retransmitted-on-rto`                      | The FIN unacknowledged: again at +0.2 and +0.4 on the measured RTO, then the peer's ACK leaves the connection in FIN-WAIT-2, silent.          |
| `rto/user-timeout-aborts-at-the-deadline`           | `--tcp_user_timeout_ms=500`: a resend at +0.2, the next expiry clamped from +0.6 to +0.5, where the connection ends without a resend.         |
| `timestamp/timestamps-off-without-the-option`       | A SYN without the option: the SYN-ACK offers the MSS alone, no segment carries the option, and a segment holds the whole MSS.                 |
| `timestamp/timestamps-off-by-config`                | `--tcp_timestamps=0`: a SYN that offers timestamps is answered with the MSS alone, and no segment carries the option.                         |
| `timestamp/paws-drops-an-old-timestamp`             | A segment with an older value is acknowledged at once and its bytes are never read; the same bytes with a fresh value are taken.              |
| `timestamp/segment-without-timestamp-is-accepted`   | A segment without the option on a timestamped connection is taken, acknowledged and read, as Linux takes it.                                  |
| `window/zero-window-is-probed`                      | The window closes with nothing in flight: probes, pure ACKs at `snd_una - 1`, at +0.2, then +0.4 and +0.8 after the one before.               |
| `window/window-opens-ends-probing`                  | The answer to the first probe opens the window: no probe for two seconds, and the next `write` leaves at once.                                |
| `window/probes-unanswered-time-out`                 | `--tcp_data_retries=3`: three probes without an answer, and the fourth expiry ends the connection; `read` reports `ETIMEDOUT`.                |
| `window/send-cap-blocks-until-acked`                | `--tcp_send_segments=2`: `write(4, ..., 3000) = 1976`, two segments; their ACK frees both entries and the next `write` leaves.                |
| `recovery/fast-retransmit-on-three-dupacks`         | `--congestion=newreno`: ten segments, the first lost; three duplicate ACKs resend it at once, before any timer.                               |
| `recovery/limited-transmit-sends-two`               | `--congestion=newreno`: the first and second duplicates each let one more segment out beyond the window; the third resends the head.          |
| `recovery/newreno-partial-ack`                      | Five segments, the first two lost: the ACK of the resent head is short of `recover`, so the next head goes at once; both policies.            |
| `recovery/newreno-full-ack-exits`                   | `--congestion=newreno`: the full ACK sets the window to `min(ssthresh, max(flight, mss) + mss)`, two segments; slow start resumes.            |
| `recovery/no-second-fast-retransmit-after-rto`      | After a timeout, three duplicates that do not cover more than `recover` resend nothing; the loss episode resends the next head.               |
| `congestion/initial-window-is-ten`                  | `--congestion=newreno`: `write(4, ..., 20000) = 9880`, ten segments of 988; after the first ACK the next `write` accepts two.                 |
| `congestion/slow-start-doubles-per-round`           | `--congestion=newreno`: ten segments acknowledged one by one double the window, and the next `write` accepts twenty.                          |
| `congestion/congestion-avoidance-adds-one`          | `--congestion=newreno`: past the threshold, a window of acknowledged bytes adds one segment: two, then three.                                 |
| `congestion/timeout-restarts-at-one`                | `--congestion=newreno`: after a timeout one segment, then slow start to the threshold while each ACK resends the next lost head.              |
| `congestion/idle-restarts-the-window`               | `--congestion=newreno`: a `write` 300 ms after the last ACK, past the RTO with nothing in flight, restarts at ten segments.                   |
| `congestion/no-congestion-control-sends-the-window` | `--congestion=none`, `--tcp_send_segments=64`: a `write` of forty segments leaves at once.                                                    |

Where Linux differs today, the script's comment says how: Linux delays the ACK of in-order data and of a FIN by up to
40 ms, offers `sackOK` and `wscale` beside the MSS and timestamps on its SYNs, retries a SYN six times, runs PRR in fast
recovery, and probes a zero window only while it holds unsent data. The later steps of phase 2 change these scripts as
SACK, window scaling and delayed ACKs land; the grammar already carries every option they need.

## Design notes

**A fixture module, not a test helper.** The runner is for anyone testing a loop over the TCP brick, not only
Aloe's own test binaries, so it lives in `fixtures/` beside the fabric and frames, under its own namespace, and
contains no gtest: it returns values, and a `Failure` a person reads without a debugger. Aloe's scripts binary
is the thin gtest wrapper. `tests/shared/` keeps what only Aloe's binaries need.

**The runner's clock is virtual and steps at the wheel's resolution.** Nothing in a run reads a real clock, so a
script is deterministic, its 63 seconds of retries take milliseconds, and it runs the same under every sanitizer.
The clock steps by the wheel's resolution because the stack's timers fire at that resolution and no finer: a
script that needs finer timing asks for timing the stack does not have. The default tolerance is 1 ms, the wheel's default resolution;
a script's `--tolerance_usecs` overrides it.

**Unbound calls fail loudly.** A call the runner does not bind, an option or errno word it does not know, flags
on a `send`, an inbound field the frame builder cannot write, or syntax outside the subset fails the script with
its line. A script that passes has therefore run every line it contains; nothing is skipped, and nothing is
taken as close enough.

**Strict matching.** An outbound line checks every field it writes and the checksums it does not, an unexpected
segment fails the script wherever it lands, and every script runs in both checksum modes. A script cannot pass
by asserting nothing.

**The policy is an axis, as the checksum mode is.** A script that asserts nothing about the congestion window
runs under `NewReno` and `NoCongestionControl` alike, so what the stack keeps under every policy, the RTO,
timestamps, probes, fast retransmit and the `recover` guard, is pinned under both; a script that asserts a
window names its policy. The harness and the runner are templates over the policy, so both runs drive the same
code but the policy's.

**A partial write is the brick's answer.** Packetdrill's scripts lean on the kernel's send buffer, which takes a
whole `write` and sends it as the window opens. The brick has none: `prepare` offers what the windows and the
send cap allow and nothing beyond, and `Writable` says when there is more, so a script that writes again after
the ACK shows what a caller of `prepare` and `commit` sees.
