---
name: savenotes
description: Save answers from this conversation into the topic-organized FAQ in z_notes/. Use when the user runs /savenotes, or asks to write up / save / record what was just explained.
---

# savenotes

Take explanations from **this conversation** and write them into `z_notes/` as simple FAQ
entries. The user skims these later to re-learn the material, so clarity beats completeness of
phrasing — but **no information may be dropped.**

`ARGUMENTS` names what to save (e.g. "the buffer and recvfrom stuff", "everything about
alignment"). If it's vague, cover everything substantive since the last `/savenotes` or the last
topic shift.

## Hard rules

1. **Only from this conversation.** Never invent an entry or pad with material that wasn't
   discussed. If something was discussed but you're unsure it's correct, write it and append
   `> ⚠️ verify this`.
2. **Simple language.** Short sentences. Plain words. Write for someone who has forgotten the
   context entirely and is skimming. No throat-clearing, no "it's important to note that".
3. **Lose no information.** Simple ≠ lossy. If an explanation had a gotcha, a number, a
   byte count, or a reason-why, it goes in. Compress the prose, never the content.
4. **Never explain at a higher level than the original.** If the conversation covered *why*
   something is true, the entry says why. A definition with the reasoning stripped out is a
   failed entry.

## Entry format

```markdown
### Q: <the question, as the user would actually ask it>

<One or two sentences. The direct answer. No preamble.>

<Optional: bullets for the details, a short table, or a code/byte diagram.>

**Example:** <concrete, specific, with real numbers where the topic has numbers>

**Gotcha:** <only if there is one — the thing that bites people>
```

- Phrase the Q the way the user asks things, not like a textbook heading.
  Good: `Q: what is a buffer and why do we need one?`
  Bad: `Q: On the Nature of Receive Buffers`
- Include `**Example:**` whenever the topic has anything concrete attached — byte offsets,
  numbers, a code line. Skip it for purely conceptual entries rather than inventing one.
- Cross-link related entries with a plain line: `See also: [cpp.md](cpp.md) — struct padding`.

## Where entries go

Route by **topic**, not by when it was discussed. One file per topic:

| File | Covers |
|---|---|
| `z_notes/cpp.md` | the C++ language itself — types, structs, alignment, `static`, templates, operators, stdlib |
| `z_notes/opsys_machine_arch.md` | OS and hardware — syscalls, kernel vs userspace, memory, cache, registers, CPU behaviour, networking stack |
| `z_notes/inbound_outbound_data.md` | wire formats and parsing — ITCH, SBE, offsets, byte order, framing, the parse loop |
| `z_notes/market_structure.md` | how markets work — order books, matching, message semantics, market data vs order entry |
| `z_notes/build_toolchain.md` | compiling, linking, CMake, flags, sanitizers, debugging tools |
| `z_notes/bad_habits.md` | things not to do in this codebase. **Rules list, not FAQ format** — leave its style alone. |

Create a topic file if a genuinely new topic comes up. Don't force an entry into a bad fit, and
don't create a file for a single entry that belongs in an existing one.

Append new entries to the end of the right file's FAQ section. **Check for an existing entry on
the same question first** — if there is one, improve it in place rather than adding a duplicate.

## Also prune `z_questions/`

`z_questions/` holds the user's *open* questions. When you write an entry that answers one of
them, **delete that question from `z_questions/`** — it's answered now, it lives in the notes.

Leave anything still unanswered. Don't delete a question because it was partially addressed; if
the entry only covers half of it, narrow the question in place to the part that's still open.

## After writing

Report, briefly:
- which files you wrote to and how many entries each
- which `z_questions/` entries you removed
- anything discussed that you deliberately did **not** save, and why
