# Feature Card Index

> Auto-maintained. One card per ingested paper. See `README.md` for the schema.
> **Empty — drop PDFs into `research/papers/inbox/` and ask for an ingest.**

---

## Template (copy for each paper)

### `kyle-1985-lambda`
- **Paper:** Kyle, A. (1985). Continuous Auctions and Insider Trading. *Econometrica* 53(6).
- **Claim:** Price impact is linear in signed order flow; the coefficient λ measures
  illiquidity and is inversely proportional to noise-trader volume.
- **Inputs:** signed trade volume, mid-price series
- **Horizon:** intraday, any bar
- **Formula:** `ΔP = λ · Q_signed`, `λ = σ_v / (2 · σ_u)`
- **Assumptions:** single informed trader, normal priors, continuous auction.
  Breaks in auction/circuit-breaker windows and around expiry pinning.
- **Impl:** `none`
- **Status:** `unverified`
- **Edge (bps, post-cost):** —

---
