"""Offline tests for fyers_auto_login.py: no network, FYERS is simulated.

Run: python broker/tools/test_fyers_auto_login.py   (also a ctest, `fyers_auto_login_py`)
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fyers_auto_login as fal  # noqa: E402

SECRETS = {
    "ALTAIR_FYERS_CLIENT_ID": "ABCD1234-100",
    "ALTAIR_FYERS_SECRET": "S3CRETVALUE9",
    "ALTAIR_FYERS_FY_ID": "XS12345",
    "ALTAIR_FYERS_PIN": "4821",
    "ALTAIR_FYERS_TOTP_KEY": "JBSWY3DPEHPK3PXP",
    "ALTAIR_FYERS_REDIRECT_URI": "https://example.test/cb",
}
ACCESS = "eyJ" + "a" * 300
REFRESH = "eyJ" + "r" * 200


class FakeFyers:
    """Answers the six endpoints the way FYERS does; records what was sent."""

    def __init__(self, fail_otp_times=0, transient_times=0, bad_pin=False):
        self.calls = []
        self.fail_otp_times = fail_otp_times
        self.transient_times = transient_times
        self.bad_pin = bad_pin

    def __call__(self, url, payload, headers=None):
        self.calls.append((url, payload, headers or {}))
        if url == fal.URL_SEND_OTP:
            if self.transient_times:
                self.transient_times -= 1
                raise fal.Transient("api-t2.fyers.in: timed out")
            return 200, {"s": "ok", "code": 200, "request_key": "rk1"}
        if url == fal.URL_VERIFY_OTP:
            if self.fail_otp_times:
                self.fail_otp_times -= 1
                return 400, {"s": "error", "code": -1, "message": "Invalid OTP"}
            return 200, {"s": "ok", "request_key": "rk2"}
        if url == fal.URL_VERIFY_PIN:
            if self.bad_pin:
                return 400, {"s": "error", "message": "Incorrect PIN"}
            return 200, {"s": "ok", "data": {"access_token": "login-token"}}
        if url == fal.URL_TOKEN:
            return 308, {"s": "ok", "code": 308, "Url": "https://example.test/cb?s=ok&auth_code=AUTHCODE123&state=altair"}
        if url == fal.URL_VALIDATE:
            return 200, {"s": "ok", "code": 200, "access_token": ACCESS, "refresh_token": REFRESH}
        if url == fal.URL_PROFILE:
            ok = headers and headers.get("Authorization") == f"ABCD1234-100:{ACCESS}"
            return (200, {"s": "ok", "data": {"fy_id": "XS12345"}}) if ok else (401, {"s": "error"})
        raise AssertionError(url)


class AutoLoginTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        patches = [
            mock.patch.dict(os.environ, SECRETS, clear=False),
            mock.patch.object(fal, "vault_read", lambda account: None),
            mock.patch.object(fal, "fresh_totp", lambda key, used: ("123456", 0 if used is None else used + 1)),
            mock.patch.object(fal.time, "sleep", lambda s: None),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def run_login(self, fake, force=True):
        out = io.StringIO()
        with mock.patch.object(fal, "_request", fake), contextlib.redirect_stdout(out):
            code = fal.cmd_login(self.root, force=force, attempts=3)
        logs = "".join(p.read_text() for p in (self.root / "data" / "logs").glob("*.log"))
        return code, out.getvalue() + logs

    def assert_no_secret(self, text):
        for v in ["S3CRETVALUE9", "4821", "JBSWY3DPEHPK3PXP", ACCESS, REFRESH, "AUTHCODE123", "login-token"]:
            self.assertNotIn(v, text)

    def test_rfc6238_vector(self):
        # RFC 6238 appendix B, SHA-1, T=59 -> 94287082 (8 digits)
        key = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"  # base32 of "12345678901234567890"
        self.assertEqual(fal.totp(key, at=59, digits=8), "94287082")
        self.assertEqual(fal.totp(key, at=1111111109, digits=8), "07081804")
        self.assertEqual(fal.totp(key.lower(), at=59), "287082")

    def test_full_flow_writes_session_like_cpp_helper(self):
        fake = FakeFyers()
        code, out = self.run_login(fake)
        self.assertEqual(code, 0)
        doc = json.loads((self.root / "data" / "fyers_session.json").read_text())
        self.assertEqual(doc["access_token"], ACCESS)
        self.assertEqual(doc["refresh_token"], REFRESH)
        self.assertEqual(doc["data"]["broker"], "FYERS")
        self.assertEqual(doc["data"]["client_id"], "ABCD1234-100")
        self.assertEqual(doc["data"]["profile_id"], "XS12345")
        sent = {u: (p, h) for u, p, h in fake.calls}
        self.assertEqual(sent[fal.URL_SEND_OTP][0]["fy_id"], fal.b64("XS12345"))
        self.assertEqual(sent[fal.URL_VERIFY_PIN][0]["identifier"], fal.b64("4821"))
        self.assertEqual(sent[fal.URL_TOKEN][0]["app_id"], "ABCD1234")
        self.assertEqual(sent[fal.URL_TOKEN][0]["appType"], "100")
        self.assertEqual(sent[fal.URL_TOKEN][0]["redirect_uri"], "https://example.test/cb")
        self.assertEqual(sent[fal.URL_VALIDATE][0]["code"], "AUTHCODE123")
        self.assertEqual(sent[fal.URL_VALIDATE][0]["appIdHash"], fal.app_id_hash("ABCD1234-100", "S3CRETVALUE9"))
        self.assert_no_secret(out)
        self.assertFalse((self.root / "data" / "fyers_session.json.tmp").exists())

    def test_skips_when_todays_session_works(self):
        self.assertEqual(self.run_login(FakeFyers())[0], 0)
        fake = FakeFyers()
        code, out = self.run_login(fake, force=False)
        self.assertEqual(code, 0)
        self.assertEqual([u for u, _, _ in fake.calls], [fal.URL_PROFILE])
        self.assertIn("already valid", out)

    def test_retries_transient_failures(self):
        code, out = self.run_login(FakeFyers(transient_times=2))
        self.assertEqual(code, 0)
        self.assertEqual(out.count("WARN"), 4)   # stdout + activity log

    def test_gives_up_after_attempts(self):
        code, out = self.run_login(FakeFyers(transient_times=10))
        self.assertEqual(code, 4)
        self.assertNotIn("established", out)

    def test_totp_rejected_once_then_next_window(self):
        code, _ = self.run_login(FakeFyers(fail_otp_times=1))
        self.assertEqual(code, 0)

    def test_wrong_pin_is_not_retried(self):
        fake = FakeFyers(bad_pin=True)
        code, out = self.run_login(fake)
        self.assertEqual(code, 3)
        self.assertEqual(sum(1 for u, _, _ in fake.calls if u == fal.URL_VERIFY_PIN), 1)
        self.assertFalse((self.root / "data" / "fyers_session.json").exists())
        self.assert_no_secret(out)

    def test_refusal_blocks_later_runs_today(self):
        self.assertEqual(self.run_login(FakeFyers(bad_pin=True))[0], 3)
        fake = FakeFyers()
        code, out = self.run_login(fake, force=False)
        self.assertEqual(code, 3)
        self.assertEqual(fake.calls, [])                    # FYERS not even asked: no lockout
        self.assertEqual(self.run_login(FakeFyers(), force=True)[0], 0)
        self.assertFalse((self.root / "data" / "fyers_login_refused.txt").exists())

    def test_totp_retried_only_once(self):
        fake = FakeFyers(fail_otp_times=5)
        self.assertEqual(self.run_login(fake)[0], 3)
        self.assertEqual(sum(1 for u, _, _ in fake.calls if u == fal.URL_VERIFY_OTP), 2)

    def test_missing_credentials(self):
        with mock.patch.dict(os.environ, {"ALTAIR_FYERS_PIN": ""}):
            code, out = self.run_login(FakeFyers())
        self.assertEqual(code, 2)
        self.assertIn("ALTAIR_FYERS_PIN", out)

    def test_local_file_is_last_resort(self):
        cfg = self.root / "config"
        cfg.mkdir()
        (cfg / "fyers_login.local.env").write_text('# comment\nALTAIR_FYERS_PIN="9999"\nALTAIR_FYERS_FY_ID=FILEID\n')
        with mock.patch.dict(os.environ, {"ALTAIR_FYERS_PIN": ""}):
            values, source = fal.load_credentials(self.root)
        self.assertEqual(values["pin"], "9999")
        self.assertEqual(source["pin"], "config/fyers_login.local.env")
        self.assertEqual(values["fy_id"], "XS12345")      # environment wins over the file
        self.assertEqual(source["fy_id"], "environment")

    def test_mask(self):
        m = fal.mask('Authorization: Bearer abc.def access_token="xyz" pin=1234 ' + "Q" * 50)
        for v in ("abc.def", "xyz", "1234", "Q" * 50):
            self.assertNotIn(v, m)
        self.assertIn("4-digit PIN (ALTAIR_FYERS_PIN)", fal.mask("4-digit PIN (ALTAIR_FYERS_PIN)"))


if __name__ == "__main__":
    unittest.main(verbosity=1)
