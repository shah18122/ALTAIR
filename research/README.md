# `research/`

Ideas get tested here before they go anywhere near the trade handler. Nothing
in this folder sends an order.

| Folder | What |
|---|---|
| `papers/` | Paper intake: drop a PDF in `papers/inbox/`, it becomes a feature card ([papers/README.md](papers/README.md)) |
| `feature_card.hpp`, `replication.hpp` | The card format and the replication / promotion gate |
| `models/` | Paper replications that showed **no tradable edge** on NIFTY / BANKNIFTY: ANFIS, Manhire's action principle, Orrell's quantum oscillator and quantum walk, conditional stochastic dominance, Parabolic SAR, gamma scalping |
| `execution/` | Execution papers, checked against their own examples but not yet against our fills: Forsyth HJB, Lai alpha-signal execution, Verma VRC execution state |
| `tests/` | One test per replication; `ctest -R` by its name |
| `tools/` | Python helpers (bar ingest, feature cards, threshold workbook extract) |

Each replication's result, and why it is here and not in production, is in
[../docs/research/research-ideas.md](../docs/research/research-ideas.md).
Paper credits are in [../docs/research/references.md](../docs/research/references.md).

The Model Atlas in the desktop still lists every one of them, with its path.
`altair_quantum_smile` and `altair_optimal_exec` are the CLIs that run the
quantum oscillator and the HJB solver on data.

To promote one: show an out-of-sample edge after costs, then move it back to
`analytics/`, `models/` or `oms/` in its own pull request.
