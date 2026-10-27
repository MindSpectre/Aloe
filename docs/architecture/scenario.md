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
  order, each with its `TimeSpec`, its 1-based source `line` and either a `PacketLine` or a `CallLine`, and the
  script's `tolerance` when it sets one. `parse_script` returns `std::expected<Script, ParseError>`; a
  `ParseError` carries the line, the column and what was wrong.
- **`aloe::scenario::Harness`**, **`HarnessConfig`** -- a fabric with the stack's port and the peer's port, and
  the bricks over queue 0 of the stack's port, `ShardQueue`, `Ipv4`, `TimerWheel` and `tcp::Stack`, with both
  MACs learned. The defaults put the stack at 10.0.0.2 with port 8080 for `bind` and the peer at 10.0.0.1 with
  port 40000 and the initial sequence number 1000000; they give the fabric no checksum offloads (`offloads`
  picks `EmulatedOffloads::None` or `Checksums`), the wheel a resolution of one millisecond, TCP its default
  `TcpConfig`, and ARP a lifetime of an hour (`arp_lifetime` is `arp_reachable`; `arp_expire` is twice it).
  `inject`, `step`, `flush` and `collect` are the parts the runner composes; a `Collected` is one frame the
  peer's port received, parsed, with a verdict on each checksum the stack wrote.
- **`aloe::scenario::Runner`**, **`RunnerConfig`**, **`Failure`**, **`format(failure, path)`** --
  `Runner{harness, script, config}.run()` executes the script once and returns `std::expected<void, Failure>`.
  `RunnerConfig` holds the `tolerance`, one millisecond unless the script's `--tolerance_usecs` says otherwise,
  and `any_bound`, how long a `*` line waits, 120 seconds. A `Failure` is the script line and a message in the
  script's notation; `format` writes it as `path:line: message`.
- **`aloe::scenario::Peer`**, **`ObservedSegment`** -- the other end's sequence bookkeeping. `observe` turns a
  frame the stack sent into an `ObservedSegment` in the script's relative numbers, fixing the stack's initial
  sequence number and its port from the first segment it sends with SYN set, its SYN or SYN-ACK; `sequence` and
  `acknowledgement` map a script's inbound relative numbers to absolute ones. `outbound_sack_base()` is the peer's initial sequence
  number, the base of sack blocks in segments the stack sends; `inbound_sack_base()` is the stack's, or zero
  until it is known, the base of sack blocks a script injects. `ObservedSegment` lives in `script.hpp`, beside
  the packet line it is matched against.
- **`aloe::scenario::match(line, seen, sack_base)`** -- nothing when the segment satisfies the line, else the
  first difference: `flags differ`, `sequence differs`, `ack differs`, `window differs`, `urgent differs` or
  `options differ`.
- **The notation** -- `describe(line, sack_base)` and `describe(segment, sack_base)` write a packet line or a
  segment the stack sent as a script would, `> S. 0:0(0) ack 1 win 46720 <mss 1460>`; `encode_options` and
  `describe_options` turn an option list into header bytes and back; `format_time` writes a stamp as seconds
  since the script's start, `+0.010000`.

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
- **Option lines** before the first event: `--tolerance_usecs=n`. Any other name is a parse error, and so is an
  option line after the first event, so the dialect grows deliberately.
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
| `write(n, ..., count) = count`, `send(n, ..., count, 0) = count`         | `prepare` and `commit` until `count` bytes are committed, each segment as large as `prepare` allows, filled with `frames::pattern`. Fewer fails with the count accepted.                     |
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
to the stack's initial sequence number, read from its SYN or SYN-ACK. Outbound numbers are checked the other
way round. Before the stack's number is known, an inbound acknowledgement and an outbound sequence number are
taken as written, which is why a script that sees no handshake writes absolute numbers. An inbound line must write
its options rather than `<...>`, as whole 32-bit words (`pad with nop`), and cannot carry `urg`, which the
frame builder has no field for. ARP never appears in a script: both MACs are learned before it starts, with
lifetimes of an hour, beyond the `*` bound and the longest retry schedule.

## Time and matching

**The clock is virtual.** It starts at the epoch and moves only as the script asks, one wheel resolution at a
time. One step is: `now += resolution`, an empty `tcp.process` at the new stamp, `wheel.advance`,
`fabric.advance`, a drain of the stack's events (which binds them to the descriptors and releases a connection
whose terminal event arrived), `tcp.flush`, `queue.flush`, and then every frame the peer's port received joins
the runner's queue of sent segments, stamped with `now`.

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
tests/scenarios/tcp/close/remote-fin-then-local-close.pkt:13: expected "> F. 1:1(0) ack 2 win 46720" at +0.000000; the stack sent "> . 1:1(0) ack 2 win 46720" at +0.000000: flags differ
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
  (RFC 2525, 2.17) is a question for the lifecycle step of phase 2, and no script in the first set closes with
  unread data.
- The runner does not advance the clock after the script's last line, so a script that asserts the stack stays
  silent for a while needs a later line to take the clock there.
- `send` and `recv` flags must be `0`, `shutdown` takes only `SHUT_WR`, and `setsockopt` binds only
  `SO_REUSEADDR` and `TCP_NODELAY`.
- An inbound option list must fit TCP's 40 bytes; the runner does not check that yet, and the frame builder's
  assertion is what stops a longer one.

## Writing and running a script

A script lives under `tests/scenarios/tcp/<area>/<name>.pkt`. It starts with a comment saying what it asserts
and, where Linux would behave differently today, how. It writes `win` on outbound lines where the value is known,
since a field a line leaves out is not checked. Nothing else registers it: the scripts binary finds every `.pkt`
under `tests/scenarios/` at run time and registers two tests per script, one per checksum mode of the fabric,
named `Scenario.tcp/<area>/<name>/NoOffloads` and `.../Checksums`. CTest lists one entry, the binary, as it does
for every test binary; a gtest filter selects an area or a script:

```bash
cmake --build --preset debug --target Aloe.Tests.Unit.Scenarios
./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/*'
./build/debug/tests/unit_tests/scenarios/Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/fin?retransmitted*'
```

In a filter, `-` starts the patterns to exclude, so a name with a dash in it is matched with `?` in its place. A
failing script prints `path:line: message` with both segments in the script's notation, as above.

## The first set of scripts

Each script asserts the TCP of today: `<mss 1460>` on SYNs and no options elsewhere, the retry schedule of 1, 3,
7, 15 and 31 seconds after the first send with `TimedOut` at 63, and no delayed ACK.

| Script                                        | What it pins                                                                                                                |
|-----------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------|
| `handshake/passive-open`                      | Listen, SYN in, SYN-ACK out with `<mss 1460>` and the whole byte window, ACK in, `accept`; then the listener closes.        |
| `handshake/active-open`                       | `connect` returns `EINPROGRESS`, SYN out, SYN-ACK in, ACK out at the flush.                                                 |
| `handshake/syn-retransmitted-on-schedule`     | No answer: the SYN again 1, 3, 7, 15 and 31 seconds after the first, and `read` reports `ETIMEDOUT` at 63.                  |
| `handshake/syn-ack-retransmitted-on-schedule` | The passive side's schedule: the SYN-ACK again at 1, 3 and 7 seconds, then the late ACK completes the handshake.            |
| `handshake/syn-to-closed-port-is-reset`       | No listener: `R.` with sequence 0, acknowledging the SYN.                                                                   |
| `handshake/stray-ack-in-listen-is-reset`      | An ACK to a listening port with no connection: `R` at its acknowledgement number.                                           |
| `data/inbound-data-is-acked-at-flush`         | Data in, a pure ACK out at `+0` with the window less what is unread; `read` reopens it and the update leaves at the flush.  |
| `data/write-segments-by-peer-mss`             | The peer's SYN carried `mss 1000`: a `write` of 2500 leaves as three `P.` of 1000, 1000 and 500, acknowledged one by one.   |
| `data/duplicate-is-acked-at-once`             | A segment that arrives twice is answered at once with the same acknowledgement, and `read` sees its bytes once.             |
| `data/above-a-gap-is-acked-not-stored`        | A segment above a gap is answered at once with the acknowledgement at the gap and not kept; `read` sees what came in order. |
| `close/local-close-then-remote-fin`           | `close`, FIN out, ACK in, the peer's FIN in, ACK out at once; no TIME_WAIT.                                                 |
| `close/remote-fin-then-local-close`           | The peer's FIN in, ACK out at `+0`, `read` returns 0, `close`, FIN out, ACK in; a later segment is reset.                   |
| `close/fin-retransmitted-on-schedule`         | The FIN unacknowledged: again at 1 and 3 seconds, then the peer's ACK.                                                      |
| `reset/rst-in-established`                    | `R.` in: `read` fails with `ECONNRESET`, the slot is released, and a later segment from the peer is answered with `R`.      |
| `reset/rst-to-syn-is-refused`                 | `connect`, SYN out, `R.` in: `read` reports `ECONNREFUSED`.                                                                 |

Where Linux differs today, the script's comment says how: Linux delays the ACK of in-order data and of a FIN by up
to 40 ms, offers `sackOK`, `wscale` and `TS` on its SYNs, holds a segment above a gap and answers it with a SACK
block, and retries a SYN six times. The later steps of phase 2 change these scripts as SACK, window scaling,
timestamps and delayed ACKs land; the grammar already carries every option they need.

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
