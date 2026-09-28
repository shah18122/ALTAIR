# P4-04 — GETS/Greeksoft table specification from local text evidence

## 1. Scope and notation

This is a clean-room functional specification, not a visual clone. Only the
listed plain-text files were read. Citation keys: `COW` = `GETS Settings/GETS
ColumnOrderWidth.txt`; `CP` = `GETS Settings/GETS ColumnProfile.txt`; `GS` =
`GETS Settings/GETS GREEK SETTINGS.txt`; `GSP` = `GETS Settings/GETS Greek
Summary Portfolio.txt`; `GF` = `GETS Settings/GETS GenaralFormSetting.txt`;
`GEN` = `GETS Settings/GETS General Settings.txt`; `EX` =
`exchange_settings.ini`; `STM` = `BroadcastData/symboltokenmapping.txt`; `POS`
= `core/types/broker_positions.hpp`; `STATE` = `core/types/broker_state.hpp`;
`SPEC` = `instruments/contract_spec.hpp`; `GR` = `analytics/greeks.hpp`; `IV` =
`analytics/iv.hpp`; `PLAN` = `final.md`. Citations are one-based lines/records.

`COW` is parsed as repeating `flag|name|width`; `CP` has `name$width` pairs
after profile metadata. Width is an unknown-but-proportional internal unit, not
pixels. `[COW:1-18; CP:1-2]` Status is **Direct**, **Derived**, **Missing**, or
**Not applicable**. “P4-05a” below is a recommended follow-up card for portfolio
grouping, scenarios, history and cost aggregates absent from current types.
Existing data owners are P2-07/P2-08/P2-09, and funds separation is
P0-02d/P4-06. `[PLAN:2140-2142,2425-2438,2496-2508]`

## 2. Inventory — every record in both column files

| Record/view (verbatim) | Count | flags 0/1 | Citation |
|---|---:|---:|---|
| `HoldingsPosition` | 58 | 57/1 | `[COW:1]` |
| `RMSSummary` | 167 | 23/144 | `[COW:2]` |
| `RMSDetails` | 120 | 118/2 | `[COW:3]` |
| `RMSThreshold` | 7 | 7/0 | `[COW:4]` |
| `RMSMargin` | 46 | 46/0 | `[COW:5]` |
| `TopGainerAndLosers(Index Based)` | 6 | 6/0 | `[COW:6]` |
| `TopGainerAndLosers(Scrip Based)` | 6 | 6/0 | `[COW:7]` |
| `TradeHistory` | 28 | 27/1 | `[COW:8]` |
| `GreekSummaryReport` | 27 | 20/7 | `[COW:9]` |
| `Greek Market Watch` | 41 | 12/29 | `[COW:10]` |
| `GreekDelPortfolio` | 9 | 8/1 | empty ninth name, then dangling `1` `[COW:11]` |
| `GreekSimulation` | 13 | 12/1 | `[COW:12]` |
| `InvestorClientList` | 3 | 3/0 | `[COW:13]` |
| `IndexInformation` | 13 | 12/1 | `[COW:14]` |
| `RMSRejection` | 18 | 15/3 | `[COW:15]` |
| `GreekExpenseReport` | 14 | 13/1 | `[COW:16]` |
| `GreekHidePortfolio` | 9 | 8/1 | empty ninth name, then dangling `1` `[COW:17]` |
| `GreekMarginReport` | 18 | 17/1 | `[COW:18]` |
| `GreekMarketWatch` selector | 0 | n/a | selects `MAIN` `[CP:1]` |
| `GreekMarketWatch$MAIN` | 10 | n/a | `[CP:2]` |

`GS` corroborates profiles for `HoldingsPosition` (58), `Greek Market Watch`
(41), `GreekSimulation` (13), and two spellings of a 13-column day-wise
simulation profile; these are not additional records in the two inventoried
files. `[GS:1,21,230,260,265]`

## 3. Flag finding

The leading value is binary but unnamed. Only `Symbol` is `1` in holdings,
29/41 market-watch columns and 144/167 RMS-summary columns are `1`, while five
tables have no `1`; this does not settle visibility, hiding, pinning, sorting or
type. `[COW:1-7,10,13]` `GS` repeats it after name and width without defining it.
`[GS:1,21]` Both readings “1 = visible” and “1 = hidden/auxiliary” remain
possible; this specification uses neither and preserves the value as **F**.

## 4. Mapping vocabulary

The compact codes below make every subsequent column row a complete mapping.

| Code | Means; Altair source; status |
|---|---|
| ctl | reference selection/spacer; none; **Not applicable** — use Altair controls |
| ord | row ordinal; view index + 1; **Derived** |
| acct | broker account identity/name; current `SessionKey` is opaque; **Missing — P2-07/P2-08** `[STATE:25-31]` |
| ex | exchange; `Position::instrument → ContractSpec::exchange`; **Derived** `[POS:47; SPEC:122]` |
| sym | symbol/name; `Position::instrument → ContractSpec` display symbol; **Derived** `[POS:23-26; SPEC:72-79]` |
| series | series/ISIN; no field; **Missing — P3-01** `[SPEC:72-124]` |
| hold | provider holding/settlement/pledge bucket; no such position field; **Missing — P2-07/P2-08** `[POS:46-57]` |
| qty | signed units; `Position::net_qty`; **Direct** `[POS:49-50]` |
| avg | average entry price; `Position::average_price`; **Direct** `[POS:51-52]` |
| mark | last price; optional `Position::last_mark`; **Direct**, absent is `—`, not zero `[POS:53-54]` |
| value | position value; `notional_of(price, net_qty)`; **Derived**, exact paise `[POS:49-54]` |
| pnl | unrealised P&L; `unrealised()` = `(mark-average)*net_qty`; **Derived**, error/absent is `—` `[POS:118-141]` |
| prior | previous/close/history value; no prior field; **Missing — P4-05a** `[POS:46-66]` |
| coll | collateral/haircut/margin component; current funds are only available cash, utilised margin, collateral; **Missing — P0-02d/P4-06** `[STATE:40-46]` |
| prod | product/instrument type; `Position::product` or spec segment/type; **Direct/Derived** `[POS:28-33,48; SPEC:122-124]` |
| expiry | expiry/time-left; `ContractSpec::expiry` (or expiry minus evaluation time); **Direct/Derived** `[SPEC:104-106]` |
| strike | option strike; `ContractSpec::strike`; **Direct via lookup** `[SPEC:87-88]` |
| opt | option right; `ContractSpec::opt_type`; **Direct via lookup** `[SPEC:124]` |
| lot | units/contract; `ContractSpec::lot_size`; **Direct via lookup** `[SPEC:81-82]` |
| greek | per-contract Greek; `Greeks::{delta,gamma,vega,theta}`; **Derived** `[GR:73-88]` |
| gval | position Greek; matching Greek × `Position::net_qty`; **Derived** `[GR:73-88; POS:49-50]` |
| iv | implied volatility; IV solver; **Derived** `[IV:320-361]` |
| model | model/scenario/history metadata; absent; **Missing — P4-05a** |
| token | Greeksoft-private token/client key; no safe equivalence; **Not applicable** `[SPEC:76-79]` |
| group | portfolio/strategy grouping or description; absent; **Missing — P4-05a** |
| trade | separate buy/sell/expense aggregate; net position cannot supply it; **Missing — P4-05a** `[POS:49-52]` |
| fund | provider funds/margin value; no sufficiently specific current field; **Missing — P0-02d/P4-06** `[STATE:40-46]` |
| util | total margin used; `AccountFunds::utilised_margin`; **Direct only when provider semantics match** `[STATE:42-46]` |

## 5. Full position view, verbatim order

Each row is `# | GETS name | W | F | mapping`; all layout facts cite `[COW:1]`.

|#|GETS column|W|F|Map|
|---:|---|---:|---:|---|
|1|`Check`|195|0|ctl|
|2|`SrNo`|495|0|ord|
|3|`ClientId`|1500|0|acct|
|4|`ClientName`|1500|0|acct|
|5|`Exchange`|1500|0|ex|
|6|`Symbol`|1500|1|sym|
|7|`Series`|705|0|series|
|8|`PoolHoldingQty`|705|0|hold|
|9|`DailyQty`|2505|0|hold|
|10|`MTF Daily Qty`|2505|0|hold|
|11|`ActualPoolPayoutQty`|1995|0|hold|
|12|`ActualPoolNetQty`|1995|0|hold|
|13|`PoolHoldingPrice`|705|0|hold|
|14|`PoolHoldingValue`|1995|0|hold|
|15|`Holding_BTST_Qty`|1995|0|hold|
|16|`Actual_Holding_BTST_Qty`|2505|0|hold|
|17|`DpHoldingQty`|1995|0|hold|
|18|`NetAuthorizedQty`|1995|0|hold|
|19|`AuthorizedQty_Open`|1995|0|hold|
|20|`FreeDPQty`|1995|0|hold|
|21|`Pledged Qty POOL`|1995|0|hold|
|22|`ActualPledgedQtyPOOL`|2505|0|hold|
|23|`ActualDpPayoutQty`|1995|0|hold|
|24|`ActualDpNetQty`|1995|0|hold|
|25|`DpHoldingPrice`|1995|0|hold|
|26|`DpHoldingValue`|1995|0|hold|
|27|`MTFHoldingQty`|1995|0|hold|
|28|`MTFHoldingPrice`|1995|0|hold|
|29|`MTFHoldingValue`|1995|0|hold|
|30|`NetHoldingQty`|1995|0|qty|
|31|`ActualTotalNetQty`|1995|0|hold|
|32|`NetHoldingPrice`|1995|0|avg|
|33|`NetHoldingValue`|1995|0|value using average|
|34|`ISIN`|1200|0|series|
|35|`ActualPoolQty`|1995|0|hold|
|36|`ActualPoolPrice`|1995|0|hold|
|37|`ActualPoolValue`|1995|0|hold|
|38|`ActualDpQty`|1995|0|hold|
|39|`ActualDpPrice`|1995|0|hold|
|40|`ActualDpValue`|1995|0|hold|
|41|`ActualMTFQty`|1995|0|hold|
|42|`ActualMTFPrice`|1995|0|hold|
|43|`ActualMTFValue`|1995|0|hold|
|44|`ActualNetQty`|1995|0|hold|
|45|`ActualPayoutNetQty`|1995|0|hold|
|46|`ActualNetPrice`|1995|0|hold|
|47|`ActualNetValue`|1995|0|hold|
|48|`CurrentValue`|2505|0|value using mark|
|49|`CurrCollateralValue[LTP]`|2505|0|coll|
|50|`PrevFullValue`|2505|0|prior|
|51|`ClosingPrice`|1995|0|prior|
|52|`Haircut[PERC]`|1995|0|coll|
|53|`CollateralVal[ClosePrice]`|2505|0|coll|
|54|`LTP`|1995|0|mark|
|55|`SoldQty`|2505|0|hold|
|56|`PendingQty`|2505|0|hold|
|57|`RiskBlockQty`|2505|0|hold|
|58|`FreeHoldingQty`|2505|0|hold|

## 6. Full Greek market-watch and portfolio views

### 6.1 `Greek Market Watch`

All layout facts cite `[COW:10]`. `TheoriticalPrice` is the reference's
misspelling; Altair should label it “Theoretical price”.

|#|GETS column|W/F|Map|
|---:|---|---|---|
|1|`UserIV`|120/0|model (user override)|
|2|`Daysleft`|495/0|expiry (difference)|
|3|`Ser/Exp`|2790/0|expiry|
|4|`Strike Price`|735/0|strike|
|5|`Option Type`|1785/0|opt|
|6|`Units`|1800/0|qty|
|7|`TheoriticalPrice`|2940/0|`Greeks::price`; **Derived** `[GR:73-88]`|
|8|`RealizedIV`|2430/0|iv|
|9|`LTP`|2025/0|mark|
|10|`TradPrice`|2040/0|avg|
|11|`MtoM`|4830/0|pnl|
|12|`Delta`|855/0|greek|
|13|`DVal`|855/1|gval|
|14|`Gamma`|735/1|greek|
|15|`GVal`|615/1|gval|
|16|`Vega`|735/1|greek|
|17|`VVal`|735/1|gval|
|18|`Theta`|855/1|greek|
|19|`TVal`|990/1|gval|
|20|`Exchange`|375/1|ex|
|21|`Mkt Seg`|120/1|prod|
|22|`Instrument Name`|735/1|sym|
|23|`Symbol`|1230/1|sym|
|24|`Last IV`|615/1|model (history)|
|25|`Market Type`|120/1|prod (segment label)|
|26|`lOurToken`|1110/1|token|
|27|`LotSize`|375/1|lot|
|28|`AssetToken`|1110/1|token|
|29|`lOurToken2`|1230/1|token|
|30|`TradAmt`|990/1|value using average|
|31|`TradeAmtCd`|150/1|model (semantics undefined)|
|32|`MaturityDate`|1110/1|expiry|
|33|`TimeValue`|990/1|mark − `intrinsic()`; **Derived** `[GR:224-228]`|
|34|`ChangeInIV`|495/1|model (current IV − prior IV)|
|35|`PreviousIV`|615/1|model (history)|
|36|`DaysUnits`|495/1|model (semantics undefined)|
|37|`DaysATP`|735/1|model (semantics undefined)|
|38|`DaysMtoM`|615/1|model (daily baseline)|
|39|`IVType`|1605/1|model (provenance; `GreekBasis` is narrower `[GR:64-88]`)|
|40|`PrvLTP`|495/1|prior|
|41|`GreekClientId`|735/1|token|

The `MAIN` profile is exactly: `UserIV` 570, `Daysleft` 660, `Ser/Exp` 675,
`Strike Price` 900, `Option Type` 540, `Units` 1080, `TradPrice` 780,
`RealizedIV` 855, `MtoM` 495, `TheoriticalPrice` 1185. It has no flag.
Mappings are the same as above. `[CP:1-2]`

### 6.2 `GreekSummaryReport`

All layout facts cite `[COW:9]`.

|#|GETS column|W/F|Map|
|---:|---|---|---|
|1|`Portfolio Description`|1500/0|group|
|2|`StrategyWise`|1035/0|group|
|3|`ExpiryWise`|870/1|expiry (group)|
|4|`Mkt.Rate`|750/1|mark|
|5|`Balance`|1170/0|model (semantics undefined)|
|6|`MtoM`|1365/0|sum `unrealised()`; **Derived** `[POS:144-188]`|
|7|`CallIV`|495/0|iv|
|8|`PutIV`|480/0|iv|
|9|`DaysLeft`|720/0|expiry (difference)|
|10|`DeltaNeutral`|975/1|aggregate delta; **Derived**, hedge policy P4-05a|
|11|`Delta`|465/0|gval (sum)|
|12|`Theta`|510/0|gval (sum)|
|13|`Vega`|465/0|gval (sum)|
|14|`Gamma`|630/0|gval (sum)|
|15|`Expense`|705/0|trade|
|16|`DayExpense`|990/0|trade|
|17|`NetMtoM`|750/0|MTM − expense; **Missing — P4-05a**|
|18|`Description`|885/1|group|
|19|`IVType`|600/1|model|
|20|`EQPosn`|675/0|cash-segment quantity; **Derived** `[SPEC:50-52]`|
|21|`Balance Neutral`|1230/0|model (semantics undefined)|
|22|`CDBalance`|900/0|model (balance semantics undefined)|
|23|`CDMTOM`|810/1|currency-segment sum `unrealised()`; **Derived**|
|24|`Span Margin`|990/1|fund|
|25|`Exposure Margin`|1275/0|fund|
|26|`Total Margin`|975/0|util|
|27|`%DelWrtGamma`|1245/0|model (formula undefined)|

### 6.3 Remaining portfolio views

All layout facts cite their named record: delete `[COW:11]`, simulation
`[COW:12]`, expense `[COW:16]`, hide `[COW:17]`, margin `[COW:18]`. `(empty)`
denotes the source's empty string, not a renamed column.

|View/#|GETS column|W/F|Map|
|---|---|---|---|
|Del/1|`Sr.No`|300/1|ord|
|Del/2|`Check`|150/0|ctl|
|Del/3|`Exchange`|570/0|ex|
|Del/4|`Client Code`|945/0|acct|
|Del/5|`Symbol`|1695/0|sym|
|Del/6|`Expiry`|1695/0|expiry|
|Del/7|`Strategy`|1695/0|group|
|Del/8|`Portfolio Description`|7365/0|group|
|Del/9|*(empty)*|375/0|ctl (malformed/spacer); dangling `1` unmapped|
|Hide/1|`Sr.No`|375/1|ord|
|Hide/2|`Hidden`|150/0|ctl (Altair profile state)|
|Hide/3|`Exchange`|570/0|ex|
|Hide/4|`Client Code`|945/0|acct|
|Hide/5|`Symbol`|1890/0|sym|
|Hide/6|`Expiry`|1695/0|expiry|
|Hide/7|`Strategy`|1695/0|group|
|Hide/8|`Portfolio Description`|7560/0|group|
|Hide/9|*(empty)*|375/0|ctl (malformed/spacer); dangling `1` unmapped|
|Sim/1|`MktRate`|1500/1|model (scenario input)|
|Sim/2|`Balance`|1500/0|model|
|Sim/3|`ExpBalance`|1500/0|model|
|Sim/4|`Delta`|1500/0|greek|
|Sim/5|`Theta`|1500/0|greek|
|Sim/6|`Vega`|1500/0|greek|
|Sim/7|`Gamma`|1500/0|greek|
|Sim/8|`ExpDelta`|1500/0|model (scenario Greek)|
|Sim/9|`ExpTheta`|1500/0|model (scenario Greek)|
|Sim/10|`ExpVega`|1500/0|model (scenario Greek)|
|Sim/11|`ExpGamma`|1500/0|model (scenario Greek)|
|Sim/12|`DeltaNeutral`|1500/0|aggregate delta; **Derived**, policy P4-05a|
|Sim/13|`ExpDeltaNeutral`|1500/0|model (scenario hedge)|
|Expense/1|`_`|585/1|ctl|
|Expense/2|`Date`|435/0|trade|
|Expense/3|`Symbol`|600/0|sym|
|Expense/4|`InstType`|705/0|prod|
|Expense/5|`ExpiryDate`|855/0|expiry|
|Expense/6|`Strike`|495/0|strike|
|Expense/7|`OptType`|705/0|opt|
|Expense/8|`BuyQty`|600/0|trade|
|Expense/9|`BuyAmt`|630/0|trade|
|Expense/10|`BuyAvgRate`|990/0|trade|
|Expense/11|`SellQty`|585/0|trade|
|Expense/12|`SellAmt`|615/0|trade|
|Expense/13|`SellAvgRate`|975/0|trade|
|Expense/14|`TotalExpense`|1065/0|trade|
|Margin/1|`_`|585/1|ctl|
|Margin/2|`Date`|525/0|snapshot observation time; **Direct** `[POS:64]`|
|Margin/3|`Symbol`|780/0|sym|
|Margin/4|`InstType`|870/0|prod|
|Margin/5|`ExpiryDate`|1080/0|expiry|
|Margin/6|`Strike`|600/0|strike|
|Margin/7|`OptType`|885/0|opt|
|Margin/8|`BuyQty`|735/0|trade|
|Margin/9|`BuyAmount`|1110/0|trade|
|Margin/10|`SellQty`|735/0|trade|
|Margin/11|`SellAmount`|1110/0|trade|
|Margin/12|`Individual Span`|1470/0|fund|
|Margin/13|`AvailableNet`|1260/0|fund|
|Margin/14|`Total Rquired Amount`|2040/0|fund (misspelling verbatim)|
|Margin/15|`Total Portfolio Span`|1860/0|fund|
|Margin/16|`Exposure`|945/0|fund|
|Margin/17|`Var`|405/0|fund|
|Margin/18|`TotalMargin`|1170/0|util|

## 7. Recommended Altair P4-05 open-position columns

This is an Altair recommendation, not GETS behavior. It covers P4-05, retains
real/paper distinctions, shows non-flat rows only and never nets broker sessions.
`[PLAN:2496-2502; POS:59-66,191-198]` Priorities come from early identity columns
in holdings and the compact `MAIN` price/MTM profile. `[COW:1; CP:2]`

|#|Altair heading|Relative width|Pin/alignment|Default|Source/render rule|
|---:|---|---:|---|---|---|
|1|Broker / account|14|pin left|shown|safe broker/account label; separate sessions|
|2|Exchange|7|pin left|shown|spec lookup; unknown `—`|
|3|Instrument|20|pin left|shown|canonical symbol; unknown `—`|
|4|Expiry|10|left|shown|`—` for cash/unknown|
|5|Strike|9|right numeric|shown|`—` for non-option/unknown; paise→INR|
|6|Type|6|centre|shown|CE/PE; otherwise `—`|
|7|Product|10|left|shown|preserve `Unknown`, never assume Intraday `[POS:28-33]`|
|8|Net qty|9|right numeric|shown|signed units; known zero is `0`|
|9|Average|11|right numeric|shown|integer paise; zero `₹0.00`|
|10|Last mark|11|right numeric|shown|absent `—`; zero `₹0.00`|
|11|Unrealised P&L|13|right numeric|shown|`unrealised()`; absent/error `—`|
|12|Freshness|11|right|shown|age from `marked`; explicit stale/unknown|
|13|Segment|8|left|hidden|extra: disambiguates cash/future/option|
|14|Lot size|8|right numeric|hidden|extra: explains units versus lots|
|15|Marked value|12|right numeric|hidden|mark × quantity; absent `—`; exact paise|

Sorting, filtering and resizing are required. `[PLAN:2500-2502]` Persisted Altair
profiles are reasonable, but the unresolved GETS flag must not seed visibility.

## 8. Density and workflow findings

- The 41-column market watch has a named ten-column `MAIN` profile, establishing
  compact profile selection, not its UI gesture. `[COW:10; CP:1-2]`
- Widths vary greatly and therefore establish relative priority only. `[COW:1-18]`
- Greek rounding stores `4|4|4|4|4|4|0`, but does not identify target columns.
  `[GS:204]` Refresh values are `1`, with no unit. `[GS:205; GF:1]`
- The summary stores `LastSortColumn=0` and twelve numbered NSE portfolio entries;
  that establishes persisted order metadata, not grouping behavior. `[GSP:1-13]`
- No supplied record defines row height, font size/family, keyboard bindings,
  focus traversal, pinning or alignment. `[COW:1-18; CP:1-2; GS:1-267; GF:1;
  GEN:1-2]` P4-07 must choose and visually test them rather than attribute them
  to GETS. `[PLAN:2509-2515]`

## 9. Not established by this evidence

The evidence does not establish: (1) flag meaning `[COW:1-18]`; (2) physical
width units `[COW:1-18; CP:2]`; (3) row height, fonts, colours, DPI, alignment,
pinning or selection styling `[COW:1-18; CP:1-2; GS:1-267]`; (4) keyboard,
menus, editing, filter mechanics or profile-switch gesture `[CP:1-2]`; (5)
business formulas for `Balance`, `TradeAmtCd`, `DaysUnits`, `DaysATP`,
`DaysMtoM`, `%DelWrtGamma`, or `Actual`/`CUR`/`Comm`/`POOL`/`DP`/`MTF`/`TNC`
distinctions `[COW:1-3,9-10]`; (6) the dangling `1` after each empty portfolio
column `[COW:11,17]`; (7) meanings of exchange codes in the sole INI record
`[EX:1]`; (8) any symbol/token mapping because `STM` is empty `[STM:whole
file]`; or (9) refresh units, calculation provenance, or whether GETS visually
distinguishes absent from zero `[GS:180-230; GF:1; GEN:1-2]`.

Altair must render unknown optional money as `—` and known zero as `₹0.00`, with
stale/error state explicit. This is an Altair contract, not a GETS finding.
`[POS:41,53-56,126-127; STATE:40-46; PLAN:2499-2508]`

## 10. Compliance

No binary was executed, launched, decompiled, disassembled or unpacked. No
branded asset was copied. The reference directory was not modified. Only
`prompts/P4-04_GETS_TABLE_SPEC.md` was modified; no C++, CMake, configuration or
other prompt file was changed, and no build or `ctest` was run.