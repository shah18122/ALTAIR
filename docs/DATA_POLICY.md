# Data policy

Smit's standing rule (2026-10-26, made a hard, enforced rule 2026-11-11):

1. **Never the NSE bhavcopy.** No ALTAIR code, script, test, doc or data file
   may read, fetch, store or cite it, in either format (the old
   `fo<DD><MON><YYYY>bhav.csv` or the UDiFF `BhavCopy_NSE_*` files and their
   `*_udiff.zip` archives).
2. **FYERS is the price source.** If FYERS does not have something (expired
   options and futures, for one), the tool says so and stops. It never falls
   back to another source.
3. **Other sources are only what Smit names, and are kept apart.** `dataset/`
   (TradingView exports) is opt-in (`nightly.py --with-dataset`). The tick
   files Smit shared on Google Drive on 2026-11-11 (NIFTY and BANKNIFTY
   futures trades, NIFTY spot ticks; 2020, 2022, 2023) are a separate source
   tagged `drive-ticks`: never merged into a FYERS series, and every result
   trained on them says so.

## How it is enforced

| Lock | Where | What it does |
|---|---|---|
| Source scan | `tools/check_data_policy.sh`; CI job `data policy (FYERS only)`; ctest `data_policy_scan` | Fails on any file name or line that names the banned source, outside the three files that define the ban (this page, `app/data_policy.hpp`, the scan). |
| Run time | `app/data_policy.hpp`, called by `altair_store snapshot` (`app/training_store.hpp`) | Walks every input tree before a snapshot and refuses the whole snapshot if a banned file is anywhere in it. Nightly retrains only ever train on a snapshot. |
| Tests | ctest `data_policy`, `training_store` | Prove both locks: every banned name format is caught, and a planted file stops the snapshot. |

## What was removed (2026-11-11)

- `app/bhavcopy.hpp` and its test; the `--source bhavcopy`, `--bhavcopy`,
  `--bhavcopy-dir` and `--with-bhavcopy` options of `altair_vol_premium`,
  `altair_vol_of_vol`, `altair_quantum_smile`, `altair_forecast_curriculum`,
  `altair_trader`, `altair_store` and `ops/linux/nightly.py`.
- `instruments/udiff_master.hpp` and its test, and the real-exchange-master
  path of `altair --instruments`. Contract specs come from FYERS' `NSE_FO.csv`.
- `data/masters/nse/BhavCopy_NSE_FO_*.csv`, `fo_udiff.zip`, `cm_udiff.zip`.
- The desktop's bhavcopy run and the daily call's `--source bhavcopy` step.
- Every result measured on the bhavcopy (the 2015-2026 variance-premium,
  vol-of-vol and quantum-smile numbers, the old IV tracks of the curriculum).
  They are withdrawn, not just labelled. The variance premium is
  **unvalidated** until the saved FYERS chains are long enough to test it.

The shared chain types the FYERS loader uses now live in
`app/option_chain.hpp`.
