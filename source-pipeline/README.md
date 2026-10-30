# Source Pipeline 0.1 PoC

Local-only PoC for a serial, Git-independent agent pipeline. It has **not**
been installed on plan2001.com. P0 findings are in [docs/p0.md](docs/p0.md);
the local implementation and test results are in [docs/validation.md](docs/validation.md).

## Components

- `server/spserver.c`: Plan 9 C `rc-httpd` handler. Build with 9front `6c`
  and `6l -lsec`. The Plan 9 server requires no Python or rsync.
- `server/*.rc`: disposable VM routing and fixture setup only.
- `web/index.html`: read-only status page, polling every five seconds.
- `worker/worker.py`: Python 3 standard-library worker for agent machines.
- `worker/simulated_agent.py`: deterministic test agent, not an AI provider.
- `tests/`: seed fixture generator and sample project checks.

The server stores opaque `source.tar.gz` packages and alternating state files.
It never extracts, builds, or executes a submitted package. Only an authenticated
worker with the current lease can stage a package or complete a task. The
worker verifies SHA-256 and rejects unsafe archive paths and links before
extracting locally.

## Local fixture

Generate a three-round test fixture on the agent/development machine:

```sh
python3 source-pipeline/tests/make_fixture.py /tmp/sp-fixture --rounds 3
```

Copy that fixture into an isolated 9front VM, install the handler there, and
route `/sp/api/v1/*` to it using `server/select-handler-test.rc`. The test
selector must only be installed in a disposable VM overlay. The production
`select-handler` and server were not changed.

Example worker invocation from the agent machine:

```sh
python3 source-pipeline/worker/worker.py \
  --url https://plan9-vm.example:17443 --agent alpha --token "$SP_TOKEN" \
  --agent-command 'python3 /path/to/simulated_agent.py' \
  --test-command 'python3 check_project.py' --once
```

For a self-signed local VM certificate only, add `--insecure`. Configure
real workers with a provider-specific `--agent-command`; the worker itself
does not depend on any AI vendor.

Do not copy the fixture permissions or test tokens to production. Production
deployment requires a deliberate `rc-httpd` route, protected data directory,
agent-specific credentials, and a verified HTTPS path. No deployment command
is included here.
