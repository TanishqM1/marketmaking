---
name: bypass
description: Override the project's no-code-writing rule for a single request. Use when the user invokes /bypass — write the requested code, then explain and justify it concisely.
---

# bypass

`AGENTS.md` for this project normally forbids writing implementation code — the user writes all
of it, and you give guidance, debugging, and review only.

**This invocation suspends that rule for this one request.** Write the code.

- Do not ask permission, restate the rule, or offer guidance instead.
- Scope is this request only. The next message reverts to guidance-only.

## Required output, in this order

**1. The code.**

**2. What it does** — brief. Focus on anything non-obvious; skip narrating the obvious parts.

**3. Justification, three short points.** A line or two each, not paragraphs:

- **Safe** — no UB, no leaks, no unit/type confusion. Name the specific hazard avoided where
  there was one (misaligned load, narrowing conversion, signed overflow, dangling view into a
  receive buffer).
- **Fast** — why this is the right cost. Be concrete: allocations (ideally zero), copies, cache
  behavior, branch predictability, syscalls. Say what the dominant cost actually is.
- **Sound for a quant system** — integer prices, deterministic/replayable, errors handled at the
  trust boundary, nothing bypassable that shouldn't be, layer/dependency rules respected.

## Be honest in the justification

It's a review, not a rubber stamp. If something is a placeholder, slower than it could be, or a
deliberate tradeoff, **say so in that section** instead of manufacturing a defense. "This
allocates — fine here because it's startup, would be wrong on the hot path" is a good line.
"Optimal" with no evidence is not.

Never claim a performance win as measured when it hasn't been. Unmeasured means "should be
cheaper, worth profiling."

## Still applies

If the request covers something genuinely instructive — decode logic, the book data structures,
the socket layer, anything where writing it by hand is the point — write it anyway, but note
that in one line. Overriding is the user's call, not yours to refuse.

Everything else in `AGENTS.md` holds: the three priorities (correctness, then performance, then
design), integer prices, no allocation on the hot path, `memcpy` + byteswap for ITCH field reads
(never a struct cast), and the layer/dependency rules.
