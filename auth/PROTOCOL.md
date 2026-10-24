# Sign-in provider protocol

Pinhole never signs in to iCloud itself. A separate process, the sign-in
provider, owns everything about Apple's authentication: the password, the
two-factor code, the trust token and where the session is kept. The core
only ever receives a finished session. Swapping pyicloud for a native
implementation means writing another program that speaks this protocol;
nothing on the other side changes.

## Transport

The core starts the provider and talks to it over stdin and stdout, one JSON
object per line. Every request carries an `id`, and the matching reply echoes
it. Log output goes to stderr and is never parsed. The provider keeps no
state between runs beyond its own session store.

```json
{"id": 1, "method": "session"}
{"id": 1, "ok": true, "result": {...}}
{"id": 1, "ok": false, "error": {"code": "needs_sign_in", "message": "..."}}
```

The process takes one argument, `--state-dir DIR`, where it keeps whatever it
needs (for the pyicloud provider: a cookie jar and a session file, mode 0600).

## Methods

| Method | Params | Result |
|---|---|---|
| `status` | | `{"state": "signed_out" \| "signed_in" \| "needs_code", "apple_id": str?}` |
| `sign_in` | `{"apple_id", "password"}` | `{"state": "signed_in" \| "needs_code"}` |
| `submit_code` | `{"code"}` | `{"state": "signed_in"}` |
| `session` | | a session, see below |
| `sign_out` | | `{"state": "signed_out"}` |

`session` checks the stored session with Apple before returning it, so a
result is known to be valid at `validated_at`. When the core gets a 401 or
421 from iCloud it asks for `session` again; if that fails with
`needs_sign_in` the window shows the sign-in card.

## Session

```json
{
  "apple_id": "someone@example.com",
  "dsid": "1234567890",
  "services": {"ckdatabasews": "https://p00-ckdatabasews.icloud.com:443"},
  "params": {"clientBuildNumber": "...", "clientMasteringNumber": "...", "clientId": "...", "dsid": "..."},
  "headers": {"Origin": "https://www.icloud.com", "Referer": "https://www.icloud.com/", "User-Agent": "..."},
  "cookies": [{"name": "...", "value": "...", "domain": ".icloud.com", "path": "/", "secure": true, "expires": 1790000000}],
  "validated_at": "2026-10-24T20:00:00Z"
}
```

`params` go on every CloudKit query string, `headers` on every request, and
`cookies` are sent to matching hosts. The core keeps the session in memory
only.

## Errors

| Code | Meaning |
|---|---|
| `needs_sign_in` | no session, or Apple rejected it |
| `needs_code` | a two-factor code is required (`submit_code`) |
| `wrong_password` | Apple refused the Apple ID or password |
| `wrong_code` | Apple refused the two-factor code |
| `rate_limited` | Apple is refusing sign-ins for a while; retrying extends the wait |
| `unavailable` | Apple's service answered 5xx or the network is down |
| `bad_request` | unknown method or missing params |
