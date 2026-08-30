# Workflow Preferences

- Prefers a strict role split: an architect/reviewer writes task cards and interface contracts and reviews all output, but never writes bulk implementation; a separate implementer implements exactly one self-contained card per prompt. Confidence: 0.9
- Prefers project continuity across many prompts to be carried by self-contained task cards with verbatim interface contracts, not by model memory or a growing context window. Confidence: 0.9
- Prefers task cards with a fixed structure: short context (3–6 lines), exact file manifest, verbatim interface contract, numbered testable behavioural spec, constraints, named acceptance tests with expected numeric values, an explicit forbidden list, and a rules block. Confidence: 0.9
- Prefers failed reviews to produce targeted "correction cards" that change only the lines needed, rather than from-scratch rewrites that lose the parts that were correct. Confidence: 0.9
- Prefers one commit per task card and never squashing cards, so git history lines up with the ledger for regression tracing. Confidence: 0.9
- Prefers card execution to be strictly scoped to the manifest: change only the named files in place, create nothing, touch no other file, and alter only the functions the card names. Confidence: 0.8
- Prefers splitting cards when a file would exceed ~400 lines, a manifest exceeds 4 files, requirements exceed 12, or acceptance tests exceed 8. Confidence: 0.85
- Prefers explicit go/no-go phase gates, with full replay regression and exit criteria verified, before starting the next phase. Confidence: 0.85
- Prefers ambiguous requirements to be implemented using the most conservative reading, with the ambiguity listed under an "ASSUMPTIONS" section at the end of the response. Confidence: 0.85
- Prefers implementation output to be complete files — no "..." ellipsis, no "rest unchanged", no placeholder bodies — using card sizing (not truncation) to keep files short. Confidence: 0.85
- Prefers reviewers to inspect acceptance-test bodies, not just exit codes, to catch tests that swallow errors or assert whatever the implementation happens to do. Confidence: 0.8
- Prefers an implementer who disagrees with an interface signature to implement it as specified and surface the objection explicitly (a "CONTRACT OBJECTION" comment block) rather than silently redesigning the API. Confidence: 0.85
- Prefers deliberately seeding acceptance tests with an incorrect expected value (a "trap") so implementers who copy blindly fail; implementers must compute each expected value themselves and justify it under ASSUMPTIONS. Confidence: 0.8
