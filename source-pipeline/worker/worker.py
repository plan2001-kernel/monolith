#!/usr/bin/env python3
"""Vendor-neutral Source Pipeline worker; Python 3 standard library only."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import ssl
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

MAX_ARCHIVE = 128 * 1024 * 1024
EXCLUDED = {".git", ".hg", ".svn", "__pycache__", ".sp-work"}


class PipelineError(Exception):
    pass


class Api:
    def __init__(self, base: str, token: str, insecure: bool = False):
        self.base = base.rstrip("/")
        self.token = token
        self.context = ssl._create_unverified_context() if insecure else None

    def request(self, endpoint: str, method: str = "GET", payload: bytes | None = None,
                headers: dict[str, str] | None = None):
        h = {"Authorization": f"Bearer {self.token}", "Accept": "application/json"}
        h.update(headers or {})
        req = urllib.request.Request(self.base + endpoint, data=payload, headers=h, method=method)
        try:
            return urllib.request.urlopen(req, context=self.context, timeout=30)
        except urllib.error.HTTPError as e:
            msg = e.read(4096).decode("utf-8", "replace")
            raise PipelineError(f"HTTP {e.code}: {msg}") from e
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise PipelineError(f"API connection failed: {e}") from e

    def json(self, endpoint: str, method: str = "GET", data: dict | None = None,
             extra: dict[str, str] | None = None) -> dict:
        raw = json.dumps(data or {}, separators=(",", ":")).encode("utf-8") if method != "GET" else None
        h = {"Content-Type": "application/json"} if raw is not None else {}
        h.update(extra or {})
        with self.request(endpoint, method, raw, h) as r:
            if r.status == 204:
                return {}
            return json.loads(r.read(1024 * 1024))


def safe_extract(archive: Path, destination: Path) -> None:
    """Extract regular files/directories only; reject traversal and links."""
    destination.mkdir(parents=True, exist_ok=False)
    root = destination.resolve()
    try:
        with tarfile.open(archive, "r:gz") as tf:
            members = tf.getmembers()
            if len(members) > 100_000:
                raise PipelineError("archive has too many entries")
            total = 0
            for m in members:
                p = Path(m.name)
                if p.is_absolute() or any(x in ("..", "") for x in p.parts):
                    raise PipelineError(f"unsafe archive path: {m.name!r}")
                if not (m.isdir() or m.isfile()):
                    raise PipelineError(f"archive contains a link or special file: {m.name!r}")
                total += max(0, m.size)
                if total > MAX_ARCHIVE * 8:
                    raise PipelineError("expanded source exceeds size limit")
                target = (root / p).resolve()
                if target != root and root not in target.parents:
                    raise PipelineError(f"archive path escapes workspace: {m.name!r}")
            tf.extractall(destination, members=members, filter="data")
    except (tarfile.TarError, OSError) as e:
        shutil.rmtree(destination, ignore_errors=True)
        raise PipelineError(f"could not unpack source: {e}") from e


def package_tree(root: Path, archive: Path) -> str:
    root = root.resolve()
    with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT) as tf:
        for p in sorted(root.rglob("*")):
            rel = p.relative_to(root)
            if any(part in EXCLUDED for part in rel.parts) or p.suffix in {".pyc", ".pyo"}:
                continue
            if p.is_symlink():
                raise PipelineError(f"source tree contains a symlink: {rel}")
            if p.is_file():
                tf.add(p, arcname=rel.as_posix(), recursive=False)
            elif p.is_dir():
                tf.add(p, arcname=rel.as_posix(), recursive=False)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if archive.stat().st_size > MAX_ARCHIVE:
        raise PipelineError("source archive exceeds 128 MiB")
    return digest


def run_process(argv: list[str], env: dict[str, str], lost: threading.Event) -> int:
    p = subprocess.Popen(argv, env=env, cwd=env.get("SP_WORKSPACE"))
    while p.poll() is None:
        if lost.is_set():
            p.terminate()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
            raise PipelineError("lease lost while local command was running")
        time.sleep(0.2)
    return p.returncode


def worker_once(args) -> str:
    api = Api(args.url, args.token, args.insecure)
    try:
        claim = api.json("/sp/api/v1/claim", "POST", {"agent": args.agent})
    except PipelineError as e:
        if "HTTP 409" in str(e) or "HTTP 423" in str(e):
            print(f"{args.agent}: waiting ({e})")
            return "WAIT"
        raise
    if not claim:
        print(f"{args.agent}: no queued task")
        return "IDLE"
    lease = claim["lease"]
    lease_seconds = int(claim.get("lease_seconds", 300))
    lost = threading.Event()
    stop = threading.Event()

    def heartbeat_loop():
        while not stop.wait(max(1, lease_seconds // 3)):
            try:
                api.json("/sp/api/v1/heartbeat", "POST", {}, {"X-SP-Lease": lease})
            except PipelineError as e:
                print(f"{args.agent}: heartbeat failed: {e}", file=sys.stderr)
                lost.set()
                return

    heartbeat = threading.Thread(target=heartbeat_loop, daemon=True)
    heartbeat.start()
    tempdir = Path(tempfile.mkdtemp(prefix="sp-worker-"))
    archive = tempdir / "source.tar.gz"
    extracted = tempdir / "incoming"
    workspace = tempdir / "workspace"
    try:
        with api.request("/sp/api/v1/source", headers={"X-SP-Lease": lease}) as r:
            source_id = r.headers.get("X-SP-Source-ID", "")
            expected = r.headers.get("X-SP-SHA256", "")
            data = r.read(MAX_ARCHIVE + 1)
        if len(data) > MAX_ARCHIVE:
            raise PipelineError("server source archive exceeds size limit")
        if hashlib.sha256(data).hexdigest() != expected:
            raise PipelineError("source SHA-256 does not match response header")
        archive.write_bytes(data)
        safe_extract(archive, extracted)
        os.replace(extracted, workspace)

        env = os.environ.copy()
        env.update({
            "SP_AGENT": args.agent,
            "SP_TASK_ID": claim["task"]["id"],
            "SP_TASK_TITLE": claim["task"]["title"],
            "SP_BASE_SOURCE": source_id,
            "SP_WORKSPACE": str(workspace),
        })
        agent = shlex.split(args.agent_command)
        if not agent:
            raise PipelineError("--agent-command is empty")
        tests = [shlex.split(x) for x in args.test_command]
        if not tests:
            raise PipelineError("at least one --test-command is required")

        def invoke(phase: str) -> None:
            env["SP_PHASE"] = phase
            if run_process(agent, env, lost) != 0:
                raise PipelineError(f"agent command failed during {phase}")

        def test_tree() -> None:
            env["SP_PHASE"] = "TEST"
            for cmd in tests:
                if run_process(cmd, env, lost) != 0:
                    raise PipelineError("project test failed: " + shlex.join(cmd))

        # Verify the previous state before asking the agent to develop.
        invoke("VERIFY")
        try:
            test_tree()
        except PipelineError:
            for _ in range(args.repair_attempts):
                invoke("REPAIR")
                test_tree()
        invoke("DEVELOP")
        try:
            test_tree()
        except PipelineError:
            for _ in range(args.repair_attempts):
                invoke("REPAIR")
                test_tree()

        source_id = uuid.uuid4().hex
        packed = tempdir / "handoff.tar.gz"
        sha = package_tree(workspace, packed)
        headers = {
            "X-SP-Lease": lease,
            "X-SP-Source-ID": source_id,
            "X-SP-SHA256": sha,
            "Content-Type": "application/gzip",
        }
        with api.request("/sp/api/v1/source", "POST", packed.read_bytes(), headers) as r:
            r.read(4096)
        result = api.json("/sp/api/v1/complete", "POST", {"test_result": "passed"}, {"X-SP-Lease": lease})
        print(f"{args.agent}: published {result['source_id']} after task {claim['task']['id']}")
        return "DONE"
    except Exception as e:
        if not lost.is_set():
            try:
                api.json("/sp/api/v1/fail", "POST", {"message": str(e)[:500]}, {"X-SP-Lease": lease})
            except PipelineError as fe:
                print(f"{args.agent}: could not release failed lease: {fe}", file=sys.stderr)
        raise
    finally:
        stop.set()
        heartbeat.join(timeout=2)
        shutil.rmtree(tempdir, ignore_errors=True)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True, help="SP origin, e.g. https://127.0.0.1:17443")
    p.add_argument("--agent", required=True)
    p.add_argument("--token", default=os.environ.get("SP_TOKEN"))
    p.add_argument("--agent-command", required=True, help="argv string; invoked for VERIFY, REPAIR and DEVELOP")
    p.add_argument("--test-command", action="append", default=[], help="test argv string; repeatable")
    p.add_argument("--repair-attempts", type=int, default=2)
    p.add_argument("--insecure", action="store_true", help="allow a local self-signed test certificate")
    p.add_argument("--once", action="store_true", help="claim at most one task")
    p.add_argument("--poll-seconds", type=float, default=3)
    args = p.parse_args()
    if not args.token:
        p.error("--token or SP_TOKEN is required")
    if not 0 <= args.repair_attempts <= 5:
        p.error("--repair-attempts must be between 0 and 5")
    while True:
        try:
            state = worker_once(args)
        except PipelineError as e:
            print(f"worker: {e}", file=sys.stderr)
            return 1
        except Exception as e:
            print(f"worker: {e}", file=sys.stderr)
            return 1
        if args.once or state in {"IDLE"}:
            return 0
        time.sleep(args.poll_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
