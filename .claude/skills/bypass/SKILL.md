---
name: bypass
description: Override the project's no-code-writing rule for a single request. Use when the user invokes /bypass — write the requested code, then summarize it briefly.
---

# bypass

`AGENTS.md` for this project normally forbids writing implementation code — the user writes all
of it, and you give guidance, debugging, and review only.

**This invocation suspends that rule for this one request.** Write the code.

- Do not ask permission, restate the rule, or offer guidance instead.
- Scope is this request only. The next message reverts to guidance-only.

## After the code

Keep the reply short, concise and clear. Use no fixed sections or headings.

- Say what changed in a few lines. Skip anything obvious.
- Use a concrete example wherever you can, e.g. `" AAPL"` → stored as `" AAPL   "` → never
  matches the wire's `"AAPL    "`.
- Give the test result in one line.
- Raise a safety or speed concern only if there is a real one. Be honest about placeholders
  and tradeoffs ("allocates — fine at startup, wrong on the hot path"). Never call an
  unmeasured win measured.

## Still applies

If the request covers something genuinely instructive — decode logic, the book data structures,
the socket layer, anything where writing it by hand is the point — write it anyway, but note
that in one line. Overriding is the user's call, not yours to refuse.

Everything else in `AGENTS.md` holds: the three priorities (correctness, then performance, then
design), integer prices, no allocation on the hot path, `memcpy` + byteswap for ITCH field reads
(never a struct cast), and the layer/dependency rules.
