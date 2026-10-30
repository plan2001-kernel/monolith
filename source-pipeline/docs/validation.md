# Source Pipeline PoC: implementation and validation

## Implementation

The Plan 9 C handler implements the versioned status/tasks API, bearer-token
authorization, serialized task claims, renewable leases, staged binary uploads,
SHA-256 verification, completion/failure handling, and an administrative task
append. Writes go through a temporary file and alternate state generations.
Package publication uses a same-directory Plan 9 rename. Completion can recover
when the package rename succeeded but the following state update was interrupted.
Repeated completion for the most recently published lease is idempotent.

The static `/sp/` page polls status and task endpoints every five seconds and
shows the current source, test result, agent order, turns, and recent error.
The Python worker runs only on an agent machine. It downloads and verifies the
archive, rejects traversal and links, invokes configurable commands for
VERIFY/REPAIR/DEVELOP, runs the configured tests, and stages the complete new
source tree. The server is C/rc only and has no Python or rsync dependency.

## Results from the disposable 9front VM

The local VM used a copy-on-write disk overlay. The C server compiled with
`6c -FTVw` and `6l -lsec`; HTTPS requests were routed through the test-only
selector. No production route, credentials, files, or DNS settings were changed.

- P1: one worker completed a source download, SHA check, extraction, test,
  development, upload, and atomic publication end to end.
- P1 status UI: local `/sp/` returned the static HTML page and `/sp/api/v1/status`
  returned the live state, with no lease value in the public JSON.
- P2: alpha → beta → gamma completed three full rounds (9 accepted turns).
  The final package contained all nine history entries in order, with no lost
  or duplicate change. The HTTP status reported `turns: 9` and IDLE.
- P3 lease recovery: an abandoned lease expired; a new claim received a new
  lease for the same task. The old lease could not be used to publish.
- P3 authorization: a valid but out-of-turn agent received HTTP 409; a
  different agent attempting to use the current lease received HTTP 403.
- P3 checksum/test gates: a bad SHA-256 returned HTTP 422. Completion with
  `test_result: failed` returned HTTP 422 and left the current source unchanged.
- P3 duplicate completion: repeating the last successful completion returned
  HTTP 200 with `idempotent: true`.
- P3 server restart: an orderly Plan 9 file-server halt and VM restart preserved
  the active lease and source generation. The worker then reported the
  interrupted lease failed and successfully retried the same queued task.
- P3 interrupted upload: a TLS connection closed before its declared body length.
  The client got an empty reply from `rc-httpd`; the current source and state
  remained unchanged and no partial package appeared in the source directory.
  The active lease could be released and the task retried successfully.

## Limits and release blockers

- The interrupted upload preserved the accepted state, but `rc-httpd` did not
  return a structured HTTP error to the disconnected client. The worker needs
  retry guidance for ambiguous transport failures before this is production
  ready.
- A maximum-size (128 MiB) upload, simultaneous workers, and abrupt power loss
  during the state-file write have not been exercised.
- The test image runs read and write handlers with different local identities.
  The fixture therefore uses permissive local file modes outside the web root.
  Production file permissions and CGI identities must be checked and tightened
  before deployment; fixture modes must not be copied to production.
- The live server's `/sp/` route remains absent. Public write API deployment,
  credential provisioning, resource/free-space checks, and public UI deployment
  were not performed.
- The P0 assessment found IPv4 HTTPS and installed CGI support, but no IPv6
  AAAA record; free disk space and the precise Plan 9 source revision remain
  unverified. See [p0.md](p0.md).

## Build and start on a test Plan 9 image

```rc
cp /path/to/spserver.c /tmp/spserver.c
cd /tmp
6c -FTVw spserver.c
6l -o spserver spserver.6 -lsec
```

Install the binary and route only inside the disposable VM. Initialize test
data with `server/setup-test.rc`. The rc-httpd route in
`server/select-handler-test.rc` sets `SP_ROOT` and runs `/bin/aux/spserver`.
The handler emits HTTP/1.1 response headers to stdout and reads the standard
rc-httpd `request`, `reqlines`, and request-body stdin inputs.

The static page is in `web/index.html`; install it under the web root as
`sp/index.html`. Keep the API handler route ahead of the `/sp/*` static route.
