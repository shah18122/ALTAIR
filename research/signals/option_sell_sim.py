"""research/signals/option_sell_sim.py -- Smit's idea (2026-11-11): on the 60 s basket forecast, SELL the opposite ATM option
(up -> sell PE, down -> sell CE), sell at the bid, buy back at the ask, exit at a Rs target or a stop.
Drive ticks have no options, so the option is approximated: mid moves delta 0.5 x the future, spread Rs 1,
charges Rs 1.35 a unit (STT 0.15% of premium + exchange + GST), theta ignored (<Rs 0.05 a minute).
    BASKET_ROWS=rows.csv ./basket_replay cfg.csv < events; python3 option_sell_sim.py bnf2023.npz rows.csv
"""
import sys
import numpy as np, pandas as pd
z=np.load(sys.argv[1]); days=list(z['days']); fpx=z['fpx']
rows=pd.read_csv(sys.argv[2],header=None,names=['d','t','f10','f60'])
T0=9*3600+15*60; END=15*3600+20*60-T0
DELTA, SPREAD, CHG = 0.5, 1.0, 1.35   # rupees a unit
rng=np.random.default_rng(7)
def run(T,S,th,random=False):
    pnl=[];hold=[]
    for d,g in rows.groupby('d'):
        F=fpx[days.index(d)]; busy=-1
        for t,f in zip(g.t.values-T0, g.f60.values):
            if t<=busy or abs(f)<=th: continue
            side = (1 if rng.random()<0.5 else -1) if random else (1 if f>0 else -1)
            e=t+1
            if not np.isfinite(F[e]): continue
            move = side*DELTA*(F[e:END]-F[e])          # option mid gain for the seller
            tgt = np.nonzero(move >= T+SPREAD)[0]; stp = np.nonzero(move <= -(S-SPREAD))[0] if S else np.array([],int)
            i_t = tgt[0] if len(tgt) else 10**9; i_s = stp[0] if len(stp) else 10**9
            i = min(i_t,i_s, len(move)-1)
            pnl.append(move[i]-SPREAD-CHG); hold.append(i); busy=e+i
    p=np.array(pnl); return len(p), (p>0).mean(), p.mean(), np.median(hold)
print("target stop  thr | trades  win%  net Rs/unit  Rs/lot(30)  median hold s | random-side net Rs/unit")
for T in (5,10):
    for S in (2*5 if T==5 else 20, 3*T):
        for th in (0.0,0.5,1.0):
            n,w,m,h=run(T,S,th); nr,wr,mr,hr=run(T,S,th,True)
            print(f"{T:>6} {S:>4} {th:>4} | {n:>6} {w*100:5.1f} {m:+10.2f} {m*30:+10.0f} {h:>8.0f} | {mr:+.2f}")
