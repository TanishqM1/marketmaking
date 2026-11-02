# Market Structure

### Q: why do we need multicast and A/B feeds?

**Multicast is forced by fanout**, not by packet loss: the exchange sends each packet once and
hundreds of subscribers pick it up off the wire — unicast would mean N copies of every message.
**A/B feeds are the fix for the loss that multicast then causes**, since multicast only exists
for UDP (TCP is inherently 1:1), and UDP drops silently.

**The chain, in order:**
```
fanout requirement -> multicast -> UDP -> no reliability -> need seq numbers + A/B + recovery
```
The unreliability isn't a speed tradeoff someone chose. It falls out of the delivery model.

**A/B is redundancy layered on top, not the same thing as multicast.** The exchange publishes the
*same* data twice, on two multicast groups, usually over separate physical paths. We subscribe to
both and take whichever sequence number arrives **first**, dropping the duplicate.

- Latency is the **minimum** of two paths, not the average — so A/B is *faster*, not slower.
- Cost is 2x packets to process plus a de-dup compare against "highest seq seen". Throughput cost,
  not latency cost.
- Bonus: a drop on path A is covered by B, so we skip the recovery path — and recovery is the
  genuinely slow thing, because a gap means we stop quoting.

**Gotcha:** A/B does NOT guarantee delivery. Both paths can drop the same packet (shared upstream
segment, or exchange-side), and **if we are too slow both sockets drop** — because that drop is in
our own kernel buffer, not the network. A/B protects against network loss, not against us being
slow. Recovery is still required.

**Gotcha 2:** we never compare the two payloads against each other. Compare **sequence numbers**,
take first-seen. The UDP checksum already covers corruption, so comparing bytes would double the
work for nothing.


### Q: what happens on a total packet drop? (gap recovery)

Detect the gap from the sequence number (1038 -> 1040), **immediately stop quoting that
instrument**, request the missing messages from the exchange's out-of-band retransmission server,
inject them into the book builder, then unblind. Costs ~100us to several ms — an eternity, but
far cheaper than trading off a wrong book.

**Three tiers, cheapest first:**
```
1. A/B arbitration    the other feed already has it    free, usual case
2. retransmit request "send me 1039"                   ~100us - several ms
3. snapshot resync    for catastrophic loss (50k pkts) replay would flood us
```

**Gotcha:** the retransmission transport **varies by venue** — Nasdaq's MoldUDP64 uses a UDP
rewind server (unicast request/response), CME and others use TCP. The pattern is universal, the
transport isn't.

**Gotcha 2:** "stop quoting" is the whole point. A gap means the book is wrong and will **silently
stay wrong forever** — nothing downstream notices. Missing a few ms of opportunity beats quoting
prices derived from a stale book and getting filled on all of them.

See also: the A/B feeds entry above, and [opsys_machine_arch.md](opsys_machine_arch.md) — why
falling behind drops from both feeds at once.
