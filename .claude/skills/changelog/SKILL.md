---
name: changelog
description: Append a dated session entry to z_notes/changelog.md summarizing what was built and learned. Use when the user runs /changelog.
---

# changelog

`/changelog <date>` — append one dated entry to `z_notes/changelog.md` covering **this
conversation**.

Date comes from `ARGUMENTS` (e.g. `Oct 29 2026`, `2026-10-29`). If absent, use today's date.
Normalize to `## YYYY-MM-DD — <Day Mon D, YYYY>` for the heading.

**Newest entry goes at the top**, directly under the file's `# Learning Changelog` header. This
is a log you skim, so most recent first.

If an entry for that date already exists, **extend it in place** — don't create a second heading
for the same day.

## Hard constraints

- **1–3 sentences MAX per bullet.** This is the point of the format. A bullet that runs longer
  gets split or cut.
- **Only from this conversation.** Never invent, never pad from general knowledge. If something
  is uncertain, mark it `⚠️`.
- **Bold the claim, then explain.** `**Locals live in registers, not memory.** Taking an
  address doesn't force memory if it doesn't escape.` The bold is what gets skimmed.
- **Keep the numbers.** Byte counts, cycle counts, cache sizes, ASCII values. A bullet with the
  reasoning but no figures is half an entry.
- Omit any section with nothing to put in it. Don't write "N/A".

## Entry shape

```markdown
## YYYY-MM-DD — <Day Mon D, YYYY>

**Built:** <1-2 sentences on code progress. Files touched, what now works.>

### C++
- **<claim>.** <why / consequence>

### Machine & OS
- ...

### Wire format
- ...

### Market structure
- ...

### Bugs I hit
- **<the bug>.** <what it did, why it was silent or loud>

### Still open
- <blocking decisions and known gaps, with what they block>
```

Section names match the `z_notes/` topic split, so an entry and its detailed note are easy to
pair up. Add a section if a session genuinely covered something outside them.

## The "Bugs I hit" section is the important one

Not a confession list — it's the most re-readable part of the log, because a bug you made once
is the one you'll make again. For each: name the bug, say what it actually did, and say whether
it was **silent or loud**.

Silent bugs (compile and run, wrong result) deserve more space than loud ones (compile errors).
Note which tool caught it — a warning flag, the compiler, a test, or nobody.

## Cross-linking

When a bullet has a fuller write-up in the topic notes, link it:
`See [cpp.md](cpp.md#q-when-is-something-in-memory-vs-in-a-register).`

Don't link speculatively — only when the note actually exists. Bullets with no note are fine;
they're a hint that `/savenotes` is worth running.

## After writing

One line: the date you wrote, and roughly how many bullets. Don't restate the entry.
