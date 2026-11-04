# Market Structure

### Why multicast, and why A/B feeds
**Multicast is forced by fanout, not by packet loss.** The exchange sends each packet once and
hundreds of subscribers pick it off the wire; unicast would mean N copies of every message.

```
fanout -> multicast -> UDP (multicast only exists for UDP; TCP is 1:1)
       -> no reliability -> need seq numbers + A/B + recovery
```
The unreliability isn't a speed tradeoff someone chose — it falls out of the delivery model.

**A/B is redundancy layered on top, not the same thing as multicast.** The exchange publishes the
*same* data twice, on two groups, usually over separate physical paths. Subscribe to both, take
whichever sequence number arrives **first**, drop the duplicate.

- Latency is the **minimum** of two paths, not the average — A/B is **faster**, not slower.
- Cost is 2× packets plus a de-dup compare against "highest seq seen". Throughput, not latency.
- A drop on A is covered by B, so you skip recovery — and recovery is the slow thing, because a
  gap means you stop quoting.

⚠ **A/B does not guarantee delivery.** Both paths can drop the same packet, and **if we're too slow
both sockets drop** — that drop is in our own kernel buffer, not the network. A/B protects against
*network* loss, not against *us* being slow.

⚠ Never compare the two payloads — compare **sequence numbers**, take first-seen. The UDP checksum
already covers corruption.

### Gap recovery
Detect the gap from the sequence number (1038 → 1040), **immediately stop quoting that
instrument**, request the missing messages from the exchange's out-of-band retransmission server,
inject them into the book builder, unblind.

```
1. A/B arbitration     the other feed already has it      free, usual case
2. retransmit request  "send me 1039"                     ~100us - several ms
3. snapshot resync     catastrophic loss (50k pkts) — replay would flood us
```

⚠ Retransmission **transport varies by venue** — Nasdaq's MoldUDP64 uses a UDP rewind server
(unicast request/response), CME and others use TCP. The pattern is universal, the transport isn't.

⚠ "Stop quoting" is the point. A gap means the book is wrong and **silently stays wrong forever**.
Missing a few ms beats quoting off a stale book and getting filled on all of it.
