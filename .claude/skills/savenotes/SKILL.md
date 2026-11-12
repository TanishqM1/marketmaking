---
name: savenotes
description: Answer one question and save it to the topic FAQ in z_notes/. Use when the user runs /savenotes with a question they want recorded as they work.
---

# savenotes

`/savenotes <question>` — **one question, one entry.**

Two things happen, in this order:

1. **Answer the question in chat**, normally. Same depth and quality as any other answer. The
   user is learning right now; this is the teaching moment, not a filing operation.
2. **Append one entry** for it to the right `z_notes/` topic file.

That's it. Do not sweep the conversation for other topics. Do not write entries the user didn't
ask for. One invocation, one entry.

If `ARGUMENTS` is empty, ask what to write up. Don't guess.

## Scope discipline

The notes are the user's, built incrementally as they hit things they don't understand. A file
full of material they never asked for is worse than no file — they can't tell what they've
actually learned from what was dumped on them.

- One entry per invocation. If the question genuinely has two separable halves, write two
  entries, and say so.
- Keep entries **short**. A screen or less. If it needs more, the question was too broad — answer
  fully in chat and write a tight entry covering the core.
- Never reorganize, expand, or "improve" entries the user wrote themselves. Match the
  surrounding file's style.

## Entry format

```markdown
### Q: <the question, in the user's own words>

<One or two sentences. The direct answer, no preamble.>

<Optional: bullets, a small table, or a byte/code diagram — only if the topic needs one.>

**Example:** <concrete, with real numbers if the topic has numbers>

**Gotcha:** <only if there is one — the thing that actually bites>
```

Rules:

- **Use the user's phrasing for the Q.** They'll recognize their own question when skimming.
- **Simple language.** Short sentences, plain words. Write for someone who has forgotten the
  context entirely.
- **Simple ≠ lossy.** Keep the numbers, byte counts, and the *reason why*. An entry with the
  reasoning stripped out is a failed entry — the "why" is the part worth re-reading.
- **Only what was discussed.** Never invent. If something is uncertain, write it with
  `> ⚠️ verify this`.
- **Frame it as a tier-1 HFT firm's choice, not just a mechanism.** The user wants to know how a
  concept maps to *industry practice*: what firms engineer, what they measure, what they accept,
  what they'd never ship, and which hardware/OS features they rely on. Explain the mechanism only
  as far as it justifies the choice. An entry that defines a term without saying what a firm does
  about it is half an entry.
- Cross-link when relevant: `See also: [cpp.md](cpp.md) — struct padding`.

## Where the entry goes

Route by topic:

| File | Covers |
|---|---|
| `z_notes/decisions.md` | design decisions — the decision log, tradeoff write-ups, assumptions, standing rules |
| `z_notes/cpp.md` | the C++ language, inlining, and building — compiling, linking, CMake, flags, sanitizers |
| `z_notes/hardware.md` | CPU / ISA, caches, TLB / hugepages, OS, syscalls, network receive path |
| `z_notes/data_structures.md` | algorithms & containers — hashing, probing, deletion, the order map's mechanics |
| `z_notes/market_data.md` | wire formats (ITCH, SBE), the parse loop, message semantics, multicast / A-B, gap recovery |
| `z_notes/hft_systems.md` | firm-level systems (cores, sharding, ops setup, FPGA) + this project's architecture |

`changelog.md` is written only by `/changelog`. In `decisions.md`, a new decision gets a **row in the
decision log**; only a big tradeoff gets a write-up. "Standing rules" is a rules list, not FAQ — keep
its style. Assumptions go in its table.

Prefer an existing file — the user wants few files. Create a new one only for a genuinely new
topic, with just the topic name as an `# H1` and the entry under it.

The ITCH / SBE sections of `market_data.md` are the user's own reference notes — add to the
relevant section without rewriting their material.

Check whether an entry for the same question already exists. If so, improve it in place instead
of duplicating.

## Then prune `z_questions/`

If the question matches one of the user's open questions in `z_questions/`, **delete it from
there** — it's answered now.

If the entry only covers part of it, narrow the question in place to whatever is still open
rather than deleting the whole line.

## Report

One or two lines: which file you appended to, and which `z_questions/` line you removed (if any).
Don't summarize the entry — they just read the answer.
