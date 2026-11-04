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
- Cross-link when relevant: `See also: [cpp.md](cpp.md) — struct padding`.

## Where the entry goes

Route by topic:

| File | Covers |
|---|---|
| `z_notes/cpp.md` | the C++ language — types, structs, alignment, keywords, stdlib |
| `z_notes/cache.md` | caches — line/way/set, hierarchy, inclusion policy, clean/dirty, alignment, working set, CAT |
| `z_notes/opsys_machine_arch.md` | OS & hardware — syscalls, kernel vs userspace, registers/ISA, network receive path |
| `z_notes/data_structures.md` | algorithms & containers — hashing, probing, layout choices |
| `z_notes/inbound_outbound_data.md` | wire formats — ITCH, SBE, offsets, byte order, framing, parse loop |
| `z_notes/market_structure.md` | how markets work — books, matching, message semantics |
| `z_notes/build_toolchain.md` | compiling, linking, CMake, flags, sanitizers |
| `z_notes/bad_habits.md` | things not to do here. **Rules list, not FAQ — leave its style alone.** |

Create the file if it doesn't exist yet, with just the topic name as an `# H1` and the entry
under it. Don't pre-create files for topics with no entries.

`inbound_outbound_data.md` already holds the user's own reference notes. Append FAQ entries below
a `# FAQ` divider at the end, leaving their material untouched.

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
