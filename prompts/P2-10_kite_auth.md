# P2-10 — `broker/`: the Kite session handshake

> Phase 2 · Card 10 · Status: **a/b DONE** 2026-08-31 · **c BLOCKED (vcpkg)**
> Depends on: nothing. Pure computation.
> Feeds: `feed/` (WebSocket connect), `oms/` (order placement), P1-07 (margins)

---

## 1. WHY `broker/` IS A NEW DIRECTORY

It is the **only directory that ever touches a credential.**

`feed/` needs an access token to open a socket; `oms/` needs one to place an
order. If the session lived in either, the other would have to include it — and
a component that can include an OMS header is a component that can be made to
trade. CLAUDE.md's rule is one component, one directory; this one is also one
**blast radius**, and it is the directory to audit when asking "what can see a
secret".

Nothing in `broker/` reads an environment variable, opens a socket, or retains
a secret past the call it was handed one in.

---

## 2. THE HANDSHAKE

From Zerodha's own client (`gokiteconnect/user.go` `GenerateSession`,
`connect.go` `GetLoginURL`) — read, not guessed:

1. Send the user to `https://kite.zerodha.com/connect/login?api_key=K&v=3`
2. **They log in in a browser.** Zerodha redirects to the registered
   `redirect_url` carrying `?request_token=...`
3. POST `/session/token` with `api_key`, `request_token`, and
   `checksum = SHA256(api_key + request_token + api_secret)`, lowercase hex
4. The response carries `access_token`, valid until the next morning

**Step 2 cannot be automated, and must not be.** It requires a Zerodha user ID,
password and TOTP. No part of Altair asks for those, and no part of Altair can
supply them.

**Step 3 is the one that fails silently.** Kite answers a wrong checksum with a
generic error that does not distinguish a wrong concatenation order from a
wrong encoding from a wrong hash. So its input is computed here and proven
against NIST vectors, leaving only the HTTP call for when vcpkg unblocks.

---

## 3. SPLIT

| Card | Deliverable | Status |
|---|---|---|
| **P2-10a** | `broker/sha256.hpp` — FIPS 180-4, NIST-verified | **DONE** |
| **P2-10b** | `broker/kite_session.hpp` — login URL + session checksum | **DONE** |
| P2-10c | the `/session/token` POST + `data/kite_session.json` persistence | BLOCKED — vcpkg |

## 4. FILE MANIFEST (a + b)

```
CREATE   broker/sha256.hpp
CREATE   broker/kite_session.hpp
CREATE   broker/tests/test_kite_auth.cpp
CREATE   broker/CMakeLists.txt
MODIFY   CMakeLists.txt          (add_subdirectory)
```

## 5. THE DECISIONS

**D1 — SHA-256 is written out, not depended on.**
The only consumer is a 96-byte hash computed once per trading day, and vcpkg is
blocked. It is **not** a general-purpose crypto library and must not grow into
one: it hashes a byte range and stops.

**D2 — Verified against NIST vectors, multi-block included.**
A SHA-256 whose padding is wrong only past 55 bytes passes `"abc"` perfectly
and then fails on every real input — which is exactly the shape of an API key
plus a request token plus a secret. The 56-byte, 112-byte and one-million-byte
vectors are all present; the last one proves the length field counts **bits**,
not bytes.

**D3 — The checksum function takes the secret as a parameter and forgets it.**
No copy, no member, no log. `broker/` reads no environment variable; the caller
does, and hands over a pointer.

**D4 — An empty secret is an ERROR, not an empty hash.**
Hashing an empty secret produces a perfectly well-formed 64-character checksum
that is always wrong, and Kite's reply would not say why. Refused instead.

**D5 — The access token is never config and never an environment variable.**
It is a *daily* artifact of an interactive login. It belongs in
`data/kite_session.json`, which is gitignored, and it expires the next morning.

## 6. TESTS

Five groups, 24 checks: NIST vectors; streaming vs one-shot (byte-at-a-time and
a split landing exactly on the 64-byte boundary); the checksum's value, length,
case and **order sensitivity**; refusal of every incomplete input; and the
login URL against Zerodha's own construction.

Run with an API key to print the real login URL:
`altair_kite_auth_test <api_key>`.

## 7. REVIEW RECORD

Reviewed 2026-08-31. MSVC 19.51.36256, `/W4`, zero warnings, 24/24 ctest.

Every digest was **cross-checked against Python's `hashlib`** rather than only
against the vectors written into the test — for a crypto primitive, a test that
agrees with its own author's transcription proves less than one that agrees
with an independent implementation. All six cases matched exactly.

Gate 7 note: there is no arithmetic on money here, but the failure mode is
financial. A wrong checksum means no session, which means no feed and no
orders — a total outage rather than a wrong number. That is the right way for
this to fail, and it is why the input is proven offline rather than debugged
against a live endpoint that answers "invalid" and nothing more.
