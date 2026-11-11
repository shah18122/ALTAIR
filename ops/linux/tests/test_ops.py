"""Unit tests for the Linux ops jobs (python3 -m unittest discover -s ops/linux/tests)."""

from __future__ import annotations

import datetime as dt
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
OPS = HERE.parent
sys.path.insert(0, str(OPS))

import altair_ops  # noqa: E402
import chain_snapshot  # noqa: E402
import nightly  # noqa: E402
import push_data  # noqa: E402


class Secrets(unittest.TestCase):
    def test_masks_values_not_keys(self):
        s = altair_ops.hide_secrets('access_token="abc.def" pin: 1234 Authorization: Bearer XYZ ok=1')
        self.assertNotIn("abc.def", s)
        self.assertNotIn("1234", s)
        self.assertNotIn("XYZ", s)
        self.assertIn("access_token", s)
        self.assertIn("ok=1", s)

    def test_one_line(self):
        self.assertNotIn("\n", altair_ops.hide_secrets("a\nb\r\nc"))


class Calendar(unittest.TestCase):
    def test_last_tuesday(self):
        self.assertEqual(altair_ops.last_tuesday(2026, 11), dt.date(2026, 11, 24))
        self.assertEqual(altair_ops.last_tuesday(2026, 12), dt.date(2026, 12, 29))

    def test_near_future_rolls_after_expiry(self):
        # No universe.csv in a clean checkout's data/live: the fallback is used.
        if (altair_ops.ROOT / "data/live/universe.csv").exists():
            self.skipTest("a live universe.csv is present")
        self.assertEqual(nightly.near_future("NIFTY", dt.date(2026, 11, 24)), "NSE:NIFTY26NOVFUT")
        self.assertEqual(nightly.near_future("NIFTY", dt.date(2026, 11, 25)), "NSE:NIFTY26DECFUT")
        self.assertEqual(nightly.near_future("BANKNIFTY", dt.date(2026, 12, 30)), "NSE:BANKNIFTY27JANFUT")


class Push(unittest.TestCase):
    def test_refuses_public_repo(self):
        self.assertTrue(push_data.public_origin("https://github.com/shah18122/ALTAIR.git"))
        self.assertTrue(push_data.public_origin(""))
        self.assertFalse(push_data.public_origin("git@github.com:shah18122/altair-data.git"))

    def test_content_fence(self):
        self.assertTrue(push_data.CONTENT_FENCE.search('"access_token": "abcdefghijklmnop"'))
        self.assertFalse(push_data.CONTENT_FENCE.search("ltp,bid,ask"))


class Quotes(unittest.TestCase):
    def test_read_quotes(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "q.json"
            p.write_text(json.dumps({"responses": [{"body": {"d": [
                {"n": "NSE:A", "s": "ok", "v": {"lp": 1}}, {"n": "NSE:B", "s": "error", "v": {"lp": 2}}]}},
                {"body": None}]}))
            self.assertEqual(chain_snapshot.read_quotes(p), {"NSE:A": {"lp": 1}})


class DryRuns(unittest.TestCase):
    """Each job's dry run, against stub tools, writes nothing and exits 0."""

    def test_dry_runs(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ("altair_feed_recorder", "altair_price_service", "altair_fyers_quotes", "altair_fyers_chain",
                      "altair_store", "altair_iv_record", "altair_fyers_history", "altair_forecast_curriculum"):
                p = Path(d) / n
                p.write_text("#!/bin/sh\nexit 0\n")
                p.chmod(p.stat().st_mode | stat.S_IEXEC)
            env = dict(os.environ, ALTAIR_BIN=d)
            for script in ("record_session.py", "nightly.py", "fyers_chain.py", "train_all.py"):
                r = subprocess.run([sys.executable, str(OPS / script)], env=env, capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, script + r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
