# Market Structure

FAQ. How markets actually work — books, matching, message semantics.

---

### Q: what is a market data book, and how is it different from an order book?

A market data book only tracks the **state** of the market. It never matches anything.

What it does **not** do:
- pair aggressors with resting orders, or generate fills
- enforce or track time priority / queue position
- maintain per-order queues within a price level
- care about MPID / market-maker identity

What it publishes is just:
```
bids:  price -> total shares   (descending)
asks:  price -> total shares   (ascending)
```

**Gotcha:** "doesn't match" ≠ "doesn't track orders". The distinction is **matching vs
bookkeeping**. We still keep one small record per live order — see the next entry.

---

### Q: why do we need a per-order map if we only care about price levels?

Because ITCH is **order-based (MBO)** — the aggregate price levels don't exist on the wire. We
derive them.

`E`, `C`, `X`, `D`, `U` carry **only an order reference number**. No price, no side, no symbol.
So `state/` needs:

```
order_ref -> { side, price, remaining_shares, book_id }
```

Every message except `A` starts with a lookup in it.

**Example:** the `D` (Order Delete) message has **no quantity field at all** — just an order
ref. The amount to remove from the price level can only come from our map. That's the proof the
map is mandatory, not an optimisation.

**Gotcha:** a *level-based (MBP)* feed is the opposite — the venue sends "bid level 3 is now
100.25 x 500" and you just overwrite. No per-order anything. CME MDP 3.0 works this way.

---

### Q: do we have to simulate matching when two orders cross?

No, never. **The exchange matches internally and then narrates what already happened.** We're
reading a transcript, not participating.

`E` (Order Executed) *is* the matched-trade message.

---

### Q: can the book ever be crossed (bid >= ask)?

Not within a single venue's book. The exchange resolves the cross before publishing anything.

**Free invariant to assert:** if best bid >= best ask, that's **our bug or a dropped packet.**
Never a real market state.

---

### Q: when an execution happens, do I decrement both sides of the book?

**One side only** — the resting order's side.

An execution has two parties: the **resting** order (in our book) and the **aggressor** (never
in our book, because it never rested). Only the resting one gets decremented. The `E` gives us
its order ref; our map tells us which side and price.

---

### Q: what's the full message flow when an aggressive order crosses the book?

Start: `bids {99: 100}`, `asks {}`. A sell order for 110 @ 98 arrives (crosses).

```
1. E - Order Executed, ref = the resting bid, executed_shares = 100
      bids {}          <- level 99 fully consumed
      asks {}

2. A - Add Order, NEW order ref, sell, 10 shares @ 98
      bids {}
      asks {98: 10}
```

Two messages, never crossed in between.

**Two things to notice:**
- The aggressor's original 110 **never appears**. Only its leftover 10 gets an `A`, with a brand
  new order ref. An order that executes instantly never rests, so it never gets an Add.
- The trade printed at **99**, not 98 — the *resting* order's price sets the trade price, so the
  aggressor got price improvement.

**Gotcha:** that second point is why plain `E` has **no price field**. It doesn't need one — the
price is the resting order's price, which we already have. `C` exists only for when the trade
price *isn't* the resting order's displayed price.

---

### Q: for `C` (executed with price) at a better price, which level do I decrement?

The level the order was **resting** at — from our map. Always.

`C`'s `execution_price` is telling us what the trade **printed** at, for last-sale and trade-stats
purposes. It's information about the trade, not about the book position.

**The rule for E, C, X, and D alike: quantity and position come from our map, never from the
price in the message.**

---

### Q: do executions delete the order?

**Decrement, not delete.** Partial executions are the common case — an `E` for 200 against a
500-share order leaves 300 resting. Only remove the order when remaining hits zero.

---

### Q: will I see cancels for orders I never saw an Add for?

Yes, by the hundreds — at start-of-day, or any time you join a live feed mid-stream, or after a
gap. The Add Order happened before you were listening.

So **"unknown order ref" cannot be fatal.** Count it, skip the message, don't corrupt a level.

**Gotcha:** this means the feed handler needs a notion of **warming-up vs live**, and the
strategy must not quote until it's live. What distinguishes a benign warmup miss from a real bug
is whether you've synced and whether the miss rate decays to zero. That state machine is
`session/`'s job.

---

### Q: how do I know which liquidity in the feed is mine?

You have to track it. **Your own resting orders appear in the market data feed you're consuming**
— the exchange doesn't mark them for you.

If you don't know which quote is yours, the strategy treats its own order as someone else's
liquidity and reacts to itself.

---

### Q: which exchanges use which protocol?

| Venue | Market data in | Order entry out |
|---|---|---|
| Nasdaq | ITCH | OUCH |
| CME | MDP 3.0 (SBE) | iLink 3 (SBE) |
| NYSE | XDP / Pillar | — |
| Cboe | PITCH | — |

**CME is the canonical real SBE shop.** Nasdaq, NYSE and Cboe all use their own binary formats,
not SBE.

**Gotcha:** real venues are always **asymmetric** — a different protocol each direction. That's
why our project is ITCH in / SBE out.

---

### Q: why is error handling vs speed not really a tradeoff here?

Because almost every check rides along on work you're already doing:

- You have to look up the order ref anyway → checking the lookup **succeeded** is free.
- Remaining qty is already in a register → checking it didn't go negative is one compare.
- Error branches predict essentially perfectly, because errors are rare.

Compare that to the ~100ns cache miss on the map lookup. The checks are noise.

**The real cost is the response, not the detection.** Formatting a log string on the hot path —
allocating, `snprintf`, touching a filesystem — is microseconds and wrecks your tail latency.
Increment a counter or push a small binary record to a ring buffer; let another thread format it.

---

### Q: what should the system DO when it detects a feed error?

Depends which of three classes it is. **"I missed data" is recoverable. "My logic is broken" is
not.**

| Class | Example | Response |
|---|---|---|
| **Structural** (trust boundary) | truncated datagram, bad length, unknown message type | drop + count. Persistent = feed broken, stop trading. Always checked, every build. |
| **Missing data** | sequence gap | mark book **stale**, pull quotes, resync, only resume when live. |
| **Invariant violation** | negative quantity, crossed book | **fail fast and loud** — assert, log, die. |

**Gotcha:** crashing on an invariant violation sounds drastic but it's the *safe* failure mode. A
crash stops you trading. A corrupted book **keeps quoting** prices derived from garbage and keeps
getting filled. This is the inverse of the web-service instinct, where you'd rather degrade than
go down.

Also: a gap means your book is now wrong and will **silently stay wrong forever**. Nothing
downstream notices. A market maker quoting off a stale book is the actual nightmare scenario.
