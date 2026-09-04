"""Derive the NSE trading calendar from the fetched bar data.

NOT a transcription of an NSE circular. Every row below is an observation:
a date either has 1-minute bars in dataset/ or it does not.
"""
import io, glob, datetime, collections

def sessions(sym):
    """date -> (bar count, first HH:MM, last HH:MM)."""
    n = collections.Counter(); lo = {}; hi = {}
    for f in sorted(glob.glob(f'dataset/spot/{sym}/1m/*.csv')):
        for ln in io.open(f, encoding='utf-8'):
            if ln.startswith('time,'):
                continue
            d, t = ln[:10], ln[11:16]
            n[d] += 1
            if d not in lo or t < lo[d]: lo[d] = t
            if d not in hi or t > hi[d]: hi[d] = t
    return {d: (n[d], lo[d], hi[d]) for d in n}

nifty = sessions('nifty')
vix = sessions('indiavix')
all_days = sorted(set(nifty) | set(vix))
cnt_n = {d: v[0] for d, v in nifty.items()}
cnt_v = {d: v[0] for d, v in vix.items()}
first = datetime.date.fromisoformat(all_days[0])
last = datetime.date.fromisoformat(all_days[-1])

NAMES = 'Mon Tue Wed Thu Fri Sat Sun'.split()
rows = []
d = first
while d <= last:
    iso = d.isoformat()
    n, v = cnt_n.get(iso, 0), cnt_v.get(iso, 0)
    wd = d.weekday()
    if n == 0 and v == 0:
        if wd < 5:
            rows.append((iso, NAMES[wd], 'CLOSED', 0, 'no bars in either series'))
    else:
        bars = max(n, v)
        src = nifty.get(iso) or vix.get(iso)
        lo, hi = src[1], src[2]
        # CLASSIFY BY OBSERVED TIME, not by bar count. A ~60-bar day is a
        # Muhurat session if it runs in the EVENING and a halt if it starts at
        # 09:15 and stops. 2021-02-24 has 54 bars running 09:15->10:08 and is
        # the NSE outage; 2022-10-24 has 60 running 18:15->19:14 and is Diwali.
        # Counting bars alone calls them the same thing.
        if iso == last.isoformat():
            kind = 'PARTIAL_TODAY'
        elif lo != '09:15' and hi >= '15:00' and bars >= 300:
            # Opened LATE and ran to the normal close: a delayed open, not a
            # separate session. 2015-03-16 ran 09:35-15:29 with 355 bars.
            kind = 'DELAYED_OPEN'
        elif lo != '09:15':
            # Did not start at the normal open, so it is a SEPARATE session
            # rather than a shortened one. Diwali Muhurat, whenever NSE puts
            # it: 18:15-19:14 in 2022, 13:45-14:44 in 2025. A rule keyed on
            # "evening" would call the 2025 one a halt.
            kind = 'SPECIAL_SESSION'
        elif bars >= 300:
            kind = 'FULL_SESSION' if wd >= 5 else None
        elif wd >= 5:
            kind = 'WEEKEND_HALF'
        else:
            kind = 'TRUNCATED'
        if kind is not None:
            rows.append((iso, NAMES[wd], kind, bars, f'{lo}-{hi}'))
        # ordinary full weekday sessions are not listed; absence from this
        # file with a weekday date means "traded normally"
    d += datetime.timedelta(days=1)

# one-symbol gaps are a DIFFERENT thing and are called out separately
mismatch = [k for k in all_days
            if (cnt_n.get(k, 0) == 0) != (cnt_v.get(k, 0) == 0)]

closed = sum(1 for r in rows if r[2] == 'CLOSED')
out = io.open('config/nse_calendar.csv', 'w', encoding='utf-8', newline='\n')
out.write(f"""# config/nse_calendar.csv -- the NSE trading calendar, DERIVED FROM DATA.
#
# P2-12g. Every row here is an OBSERVATION, not a transcription of an NSE
# circular: a date either has 1-minute bars in dataset/ or it does not.
#
# WHAT THAT BUYS, AND WHAT IT COSTS.
#
# It cannot be stale in the way a typed list can, and it agrees with the data
# any backtest actually replays -- which is the property that matters, because
# a calendar that disagrees with the bars is worse than no calendar.
#
# It also cannot tell a HOLIDAY from a DATA GAP. A weekday with no bars is
# recorded as CLOSED, and if Kite simply never served that day it is recorded
# as CLOSED wrongly. The cross-check is that NIFTY and India VIX are fetched
# independently: a real closure hits both. Days where only ONE series is
# missing are listed at the bottom as SUSPECT rather than being called
# holidays -- there are {len(mismatch)} of them.
#
# RANGE: {first} .. {last}. You asked for fifteen years; this is
# {round((last - first).days / 365.25, 1)}, because that is how far Kite's
# 1-minute history goes. Earlier years would have to come from a published
# circular, and inventing them here would be exactly the kind of confident,
# plausible, wrong reference data CLAUDE.md's gate 7 exists to catch.
#
# ORDINARY full weekday sessions are NOT listed. A weekday absent from this
# file inside the range above traded normally. Only the exceptions are here:
# {closed} closed weekdays, plus every weekend and short session.
#
# STATUS values:
#   CLOSED           a weekday with no bars in either series
#   DELAYED_OPEN     opened LATE but ran to the normal close: a delayed
#                    start, not a separate session
#   SPECIAL_SESSION  did NOT start at the normal 09:15 open, so it is a
#                    separate session rather than a shortened one. This is
#                    Diwali Muhurat, wherever NSE puts it
#   FULL_SESSION     a Sat/Sun that traded a normal 375-bar day (Budget
#                    Saturday, or an NSE disaster-recovery live session)
#   WEEKEND_HALF     a Sat/Sun that opened normally and closed at midday
#   TRUNCATED        a WEEKDAY that opened normally and stopped early. An
#                    outage or a halt, not a scheduled short day
#   PARTIAL_TODAY    the last day in the data, still in progress
#   SUSPECT          present in ONE series and not the other
#
# STATUS IS DECIDED BY OBSERVED SESSION TIMES, NOT BAR COUNT, and both halves
# of that matter:
#
#   2021-02-24  54 bars  09:15-10:08  opened normally and STOPPED -> outage
#   2022-10-24  60 bars  18:15-19:14  never opened at 09:15    -> Muhurat
#   2025-10-21  60 bars  13:45-14:44  never opened at 09:15    -> Muhurat
#
# All three have about sixty bars, so counting bars calls them the same thing.
# And a rule keyed on "evening" would get 2025 wrong, because NSE moved that
# year's Muhurat to the afternoon. The discriminator that survives both is
# WHETHER THE SESSION STARTED AT THE NORMAL OPEN.
#
# `bars` is the observed 1-minute bar count (375 is a full day); `note` is the
# observed first and last bar time.
date,weekday,status,bars,note
""")
for r in rows:
    out.write(','.join(str(x) for x in r) + '\n')
if mismatch:
    out.write('# SUSPECT -- present in one series and not the other.\n')
    out.write('# Not called holidays: a real closure hits both.\n')
    out.write('#\n')
    out.write('# 2019-07-02 was RE-FETCHED to check: Kite returns\n')
    out.write('# 2019-07-01 and 2019-07-03 for India VIX and nothing\n')
    out.write('# between. A genuine vendor gap on their side, not data\n')
    out.write('# lost here.\n')
    out.write('#\n')
    out.write('# 2024-09-02 and 2024-09-03 WERE ours, and are now fixed.\n')
    out.write('# They sat at the seam between a two-year fetch and a\n')
    out.write('# year-by-year one, and the month-file skip dropped them\n')
    out.write('# silently. The fetcher now prints what a skip costs.\n')
    for k in mismatch:
        wd = NAMES[datetime.date.fromisoformat(k).weekday()]
        a, b = cnt_n.get(k, 0), cnt_v.get(k, 0)
        out.write(f'{k},{wd},SUSPECT,{max(a, b)},nifty={a} indiavix={b}\n')
out.close()
print(f'  wrote config/nse_calendar.csv')
print(f'    range         {first} .. {last}')
print(f'    closed weekdays {closed}')
for k in ('SPECIAL_SESSION','FULL_SESSION','WEEKEND_HALF','TRUNCATED'):
    print(f'    {k:16s} {sum(1 for r in rows if r[2] == k)}')
print(f'    suspect (one series only) {len(mismatch)}')
