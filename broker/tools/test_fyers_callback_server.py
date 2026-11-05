"""Offline tests for fyers_callback_server.py: FYERS is simulated, the server runs on a free port.

Run: python broker/tools/test_fyers_callback_server.py   (also a ctest, `fyers_callback_server_py`)
"""

import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import HTTPServer
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fyers_auto_login as fal  # noqa: E402
import fyers_callback_server as cb  # noqa: E402

ENV = {"ALTAIR_FYERS_CLIENT_ID": "ABCD1234-100", "ALTAIR_FYERS_SECRET": "S3CRETVALUE9",
       "ALTAIR_FYERS_FY_ID": "XS12345", "ALTAIR_FYERS_REDIRECT_URI": "https://example.test/fyers/callback",
       "ALTAIR_FYERS_PIN": "", "ALTAIR_FYERS_TOTP_KEY": ""}
ACCESS = "eyJ" + "a" * 300


class _NoFollow(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


class CallbackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.profile = "XS12345"
        for p in (mock.patch.dict(os.environ, ENV), mock.patch.object(fal, "vault_read", lambda a: None),
                  mock.patch.object(fal, "_request", self.fake)):
            p.start()
            self.addCleanup(p.stop)
        cb._states.clear()
        cb._used_codes.clear()
        self.srv = HTTPServer(("127.0.0.1", 0), cb.make_handler(self.root))
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}"

    def tearDown(self):
        self.tmp.cleanup()

    def fake(self, url, payload, headers=None):
        if url == fal.URL_VALIDATE:
            if payload["code"] != "GOODCODE":
                return 400, {"s": "error", "code": -413, "message": "invalid auth code"}
            assert payload["appIdHash"] == fal.app_id_hash("ABCD1234-100", "S3CRETVALUE9")
            return 200, {"s": "ok", "access_token": ACCESS, "refresh_token": "r"}
        if url == fal.URL_PROFILE:
            return 200, {"s": "ok", "data": {"fy_id": self.profile}}
        raise AssertionError(url)

    def get(self, path):
        try:
            with urllib.request.build_opener(_NoFollow).open(self.base + path, timeout=5) as r:
                return r.status, r.read().decode(), r.headers
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode(), e.headers

    def test_good_code_saves_session_and_says_connected(self):
        st, body, _ = self.get("/fyers/callback?s=ok&code=200&auth_code=GOODCODE&state=x")
        self.assertEqual(st, 200)
        self.assertIn("Connected to FYERS", body)
        self.assertNotIn(ACCESS, body)
        doc = json.loads((self.root / "data" / "fyers_session.json").read_text())
        self.assertEqual(doc["access_token"], ACCESS)
        self.assertEqual(doc["data"]["profile_id"], "XS12345")
        st, body, _ = self.get("/fyers/callback?s=ok&auth_code=GOODCODE&state=x")   # a refresh
        self.assertIn("Already received", body)

    def test_bad_code_shows_error_and_saves_nothing(self):
        st, body, _ = self.get("/fyers/callback?s=ok&auth_code=BAD&state=x")
        self.assertEqual(st, 400)
        self.assertIn("did not accept", body)
        self.assertFalse((self.root / "data" / "fyers_session.json").exists())

    def test_other_account_is_refused(self):
        self.profile = "ZZ99999"
        st, body, _ = self.get("/fyers/callback?auth_code=GOODCODE")
        self.assertEqual(st, 400)
        self.assertIn("Wrong FYERS account", body)
        self.assertFalse((self.root / "data" / "fyers_session.json").exists())

    def test_fyers_error_redirect(self):
        st, body, _ = self.get("/fyers/callback?s=error&message=%3Cb%3Euser+cancelled")
        self.assertEqual(st, 400)
        self.assertIn("&lt;b&gt;user cancelled", body)   # escaped

    def test_login_redirects_to_fyers(self):
        st, _body, h = self.get("/fyers/login")
        self.assertEqual(st, 302)
        self.assertTrue(h["Location"].startswith(cb.URL_AUTHCODE + "?client_id=ABCD1234-100"))
        self.assertIn("redirect_uri=https%3A%2F%2Fexample.test%2Ffyers%2Fcallback", h["Location"])

    def test_health_and_404(self):
        self.assertEqual(self.get("/healthz")[0], 200)
        self.assertEqual(self.get("/etc/passwd")[0], 404)

    def test_post_without_webhook_receiver_is_503(self):
        req = urllib.request.Request(self.base + "/fyers/callback", data=b"{}", method="POST")
        with mock.patch.object(cb, "WEBHOOK_UPSTREAM", "http://127.0.0.1:1/fyers/callback"):
            try:
                urllib.request.urlopen(req, timeout=5)
                code = 200
            except urllib.error.HTTPError as e:
                code = e.code
        self.assertEqual(code, 503)


if __name__ == "__main__":
    unittest.main(verbosity=1)
