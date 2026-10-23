# Fixtures

The `fixtures/` tier holds what Aloe ships for testing code written over the bricks, kept apart from the
stack in `common/` so that nothing there reads as something a production program links. A fixture
depends on `common/`; nothing in `common/` depends on a fixture. Targets are named `Aloe::Fixtures::<Module>`.

| Module     | Include                    | Target                     | Holds                                                                                           |
|------------|----------------------------|----------------------------|-------------------------------------------------------------------------------------------------|
| `fabric`   | `#include <aloe/fabric>`   | `Aloe::Fixtures::Fabric`   | The in-memory device backend: a broadcast domain of ports. See the [device layer](device.md).   |
| `frames`   | `#include <aloe/frames>`   | `Aloe::Fixtures::Frames`   | Frame builders and a parser over [`wire`](wire.md)'s formats, and packet byte helpers.          |
| `scenario` | `#include <aloe/scenario>` | `Aloe::Fixtures::Scenario` | Packetdrill-style scripts run against the TCP brick on the fabric. See [scenario](scenario.md). |

Aloe's own tests use all three. Anyone testing a loop written over `tcp::Stack`, `Ipv4<Device>` or
`ShardQueue<Device>` needs the same two things: a device that runs with no root, no hugepages and no network
card, and a way to make frames and read the replies; for TCP, a scripted peer drives the brick one segment at
a time, and the [scenario runner](scenario.md) drives it from a script in packetdrill's syntax. Helpers that
only make sense inside Aloe's own test binaries, such as the EAL as a gtest environment, stay in
`tests/shared/`.

## Key types in `frames`

- **`aloe::frames::ethernet_frame`**, **`ipv4_frame`** with an **`Ipv4Spec`**, **`with_ipv4_options`** -- an
  Ethernet frame, or an IPv4 datagram with a UDP or TCP header, as bytes; `Checksums` picks correct, wrong or
  zero checksums. `pattern(length, seed)` is a recognisable payload.
- **`aloe::frames::arp_request`**, **`arp_reply`**, **`arp_frame`**, **`icmp_echo_frame`** with an **`EchoSpec`**
  -- the ARP and ICMP frames a peer would send.
- **`aloe::frames::parse_frame(bytes)`** -- a transmitted frame taken apart into a **`ParsedFrame`**: the
  Ethernet header, and the ARP packet, IPv4 header, ICMP header and L4 bytes where present. Nothing for a frame
  shorter than an Ethernet header. `l4_checksum_residue` checks a TCP or UDP checksum.
- **`aloe::frames::TcpSpec`**, **`tcp_frame(spec, payload)`**, **`tcp_segment(spec, payload)`**,
  **`tcp_payload(parsed)`**, **`tcp_options_of(parsed)`** -- a TCP segment over IPv4 over Ethernet as bytes,
  with the MSS option when `spec.mss` is set, or the raw option bytes of `spec.options` as they are when that
  is not empty (a whole number of 32-bit words, with `mss` left unset), and the checksum mode of `Checksums`
  (`Wrong` corrupts the TCP checksum only, so the IPv4 header stays right). `tcp_segment` gives the bytes after
  the IPv4 header; `tcp_payload` the bytes after the TCP header of a parsed frame, and `tcp_options_of` the
  option bytes between the fixed header and the data. `ParsedFrame::tcp` is the parsed TCP header, when the
  frame carries one.
- **`aloe::frames::TcpPeer`** -- the other end of a connection, scripted by the test. Built from a `TcpSpec`
  in the peer's outbound orientation and its initial sequence number. `syn()`, `syn_ack(seen)`, `ack()`,
  `ack(seen)`, `data(bytes)`, `fin()` and `rst()` each return one frame with the right numbers and advance the
  peer's sequence; `segment(flags, payload, sequence, acknowledgement, with_mss)` builds any segment at explicit
  numbers without changing state, for duplicates and gaps; `rewind(count)` moves the peer's sequence back to
  script a retransmission. `see(frame)` observes a frame the stack transmitted and never replies: it advances
  `rcv_nxt()`, `snd_una()`, `stack_window()` and `stack_mss()`, and on a SYN without ACK adopts its ports.
- **`aloe::frames::fill(packet, bytes)`**, **`bytes_of(packet)`** -- bytes into and out of a packet of any
  backend. `flow_of(spec)` gives the `device::FlowTuple` a card would hash.

## Usage

```cpp
#include <aloe/fabric>
#include <aloe/frames>

aloe::fabric::Fabric fabric;
aloe::fabric::Port& peer = fabric.add_port({.mac = {0x02, 0, 0, 0, 0, 2}});
// ... the code under test owns another port of the same fabric ...

auto packet = peer.allocate(0);
const auto request = aloe::frames::arp_frame(
    aloe::frames::arp_request({0x02, 0, 0, 0, 0, 2}, {10, 0, 0, 1}, {10, 0, 0, 2}),
    aloe::wire::MacAddress::broadcast());
std::ignore = aloe::frames::fill(*packet, request);
// transmit it, run the code under test, receive on `peer`, then:
// const auto reply = aloe::frames::parse_frame(aloe::frames::bytes_of(received));
```

A TCP test reads like a packetdrill script, one segment at a time. It feeds `peer.see` each frame the stack
transmits and injects what the peer sends: `peer.syn()`, then `see` the stack's SYN-ACK, then `peer.ack()`. The
peer never replies on its own, so every segment on the wire is one the test wrote down. A test that is a script
in all but name can be one: the [scenario](scenario.md) module runs packetdrill-style scripts over the same
fabric.

## Impairment and the clock

A port's egress can be impaired, so two stacks on one fabric can be made to lose, repeat, reorder and delay
segments from each other in a unit test. `Port::set_impairment(impairment)` replaces a port's impairment and
reseeds its engine, from any thread; a default `Impairment` is no impairment, and a port with none delivers as
it always has.

- **`aloe::fabric::Impairment`** -- `loss`, `duplicate` and `reorder` are probabilities per frame; `delay` is
  added to a delayed frame and `jitter` adds a uniform extra in `[0, jitter]`, drawn per frame; `seed` seeds the
  engine; `script` is called first for every frame.
- **`aloe::fabric::Egress`**, **`Verdict`**, **`Action`** -- what the script sees and answers. `Egress::index`
  counts the frames offered to the port's impairment from zero, since the port's first frame: `set_impairment`
  does not reset it. `Egress::frame` is the whole frame, for `frames::parse_frame`. A `Verdict` is one `Action`,
  `Deliver`, `Drop`, `Duplicate`, `Reorder` or `Delay`, and for `Delay` its own duration; a script that returns
  nothing leaves the frame to the model.
- **`aloe::fabric::ImpairmentCounters`** -- `dropped`, `duplicated`, `reordered` and `delayed`, from
  `Port::impairment_counters()`. A dropped frame still counts as transmitted on its queue: it left the port.

Each frame gets one action. The script answers first. With no answer the model draws for loss, then
duplication, then reordering, and the first that hits decides; a frame none of them takes is delayed when
`delay` or `jitter` is set and delivered otherwise. A drop is gone. A duplicate is delivered twice in a row. A
reorder is held until the port's next frame has been delivered, or until the next `Fabric::advance` when none
follows. A delay waits on the fabric with a due stamp. The script runs under the port's impairment lock and
inside `transmit`, which is `noexcept`: it must not call back into the port, and it must not throw.

**Determinism.** The model draws from a `std::mt19937_64` seeded with `seed` and turns the engine's raw bits
into a double itself, a 53-bit fraction, rather than through `std::uniform_real_distribution`, whose numbers
differ between standard libraries. The same seed gives the same run under clang and GCC, and a script fixes one
frame's fate exactly whatever the seed.

**The clock.** `Fabric::advance(now)` is the fabric's step. It delivers every delayed frame due at or before
`now`, in order of due stamp and then of transmission; releases every reorder hold; and marks every loan an
`OnStep` port transmitted before the call as finished. `transmit` takes no stamp, so a delay is measured from
the fabric's current stamp, `Fabric::now()`, which is the stamp of the last `advance`, the epoch before the
first; a stamp earlier than the last changes nothing. A test loop with delay or loans therefore advances the
fabric at every tick, as `TcpHarness::advance` in `tests/shared/` and the two-stack test's tick do. `advance` is
safe to call from any thread.

**Loans.** `PortConfig::loans` picks a port's `LoanReturn`. On an `OnStep` port, the default, a transmitted
loan is finished by the next `advance` and released by the queue's next `transmit`, `receive` or `reclaim`, the
calls that already take the queue's lock, so `lent()` flips on the queue's own thread, as a DPDK driver frees a
transmitted mbuf on a later burst; `reclaim(queue)` releases every loan of its queue at once, finished or not,
and returns the count. On a `Held` port nothing finishes on its own and `reclaim` returns zero;
`Port::return_loans(queue)`, on the queue's thread, releases them. `Port::loans_outstanding(queue)` counts the
handles not yet released. A test that needs a packet still lent long after its frame was delivered uses `Held`:
no CI driver does that.

```cpp
aloe::fabric::Fabric fabric;
aloe::fabric::Port& client = fabric.add_port({.mac = {0x02, 0, 0, 0, 0, 1}});
aloe::fabric::Port& server = fabric.add_port({.mac = {0x02, 0, 0, 0, 0, 2}});

client.set_impairment({
    .loss   = 0.01,
    .delay  = std::chrono::milliseconds{5},
    .seed   = 42,
    .script = [](const aloe::fabric::Egress& egress) -> std::optional<aloe::fabric::Verdict> {
        if (egress.index == 0) {
            return aloe::fabric::Verdict{.action = aloe::fabric::Action::Drop};  // the client's first frame
        }
        return std::nullopt;  // the model decides
    },
});

for (auto now = aloe::core::TimePoint{}; running; now += std::chrono::milliseconds{1}) {
    fabric.advance(now);  // what is due arrives; finished loans go back at each queue's next call
    // ... each side receives, processes, flushes ...
}
std::println("{} dropped", client.impairment_counters().dropped);
```

## Design notes

The frames module has no gtest in it: `parse_frame` reports a short frame as `std::nullopt` instead of
failing an assertion, so a test of any framework can use it. Aloe's tests call `.value()`, which fails the
test with an exception when the frame is short.

**The loan stays on the queue's thread.** The fabric's pool and the lent and orphaned marks on its blocks are
the queue thread's, unsynchronised, as the pool always was. `advance` touches only each queue's count of
finished loans, under the queue's lock, and never the pool: the handles themselves are released by the queue's
own next call. A lender dropped while lent orphans its block, and the handle gives the block back when it is
released. The `Pool` destructor's assertion, that every packet is destroyed before its pool, is the check that
every loan came back exactly once; a loan never finished or reclaimed stays out until the port is destroyed,
and the assertion reports it.
