#!/usr/bin/env python3
"""Create disposable local Plan 9 seed data; never used by the server."""
import argparse
import hashlib
import shutil
import tarfile
from pathlib import Path

AGENTS = (
    "admin test-admin-token\n"
    "agent alpha test-alpha-token\n"
    "agent beta test-beta-token\n"
    "agent gamma test-gamma-token\n"
)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("output", type=Path)
    p.add_argument("--rounds", type=int, default=3)
    a = p.parse_args()
    shutil.rmtree(a.output, ignore_errors=True)
    a.output.mkdir(parents=True)
    project = Path(__file__).parent / "fixtures" / "sample-project"
    with tarfile.open(a.output / "seed.tar.gz", "w:gz") as tf:
        for f in sorted(project.iterdir()):
            tf.add(f, arcname=f.name, recursive=False)
    archive = a.output / "seed.tar.gz"
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (a.output / "agents.cfg").write_text(AGENTS, encoding="ascii")
    agents = ("alpha", "beta", "gamma")
    tasks = []
    for rnd in range(1, a.rounds + 1):
        for agent in agents:
            tid = f"r{rnd}-{agent}"
            tasks.append(f"{tid}|Round {rnd}: {agent} continues the same source tree")
    (a.output / "tasks.cfg").write_text("\n".join(tasks) + "\n", encoding="utf-8")
    state = (
        "magic SP0\n"
        "generation 1\n"
        "phase RUNNING\n"
        "current seed\n"
        f"currentsha {digest}\n"
        f"currentsize {archive.stat().st_size}\n"
        "active -\nlease -\nleaseuntil 0\ntask -\nbase -\n"
        "staged -\nstagedsha -\nstagedsize 0\n"
        "nextagent 0\nnexttask 0\nfailures 0\nturns 0\n"
        "lastlease -\nlastsource -\ntestresult none\n"
    )
    (a.output / "state.0").write_text(state, encoding="ascii")
    (a.output / "tokens.txt").write_text(
        "admin test-admin-token\nalpha test-alpha-token\nbeta test-beta-token\ngamma test-gamma-token\n",
        encoding="ascii",
    )
    print(f"fixture: {a.output} ({len(tasks)} tasks, seed sha256 {digest})")


if __name__ == "__main__":
    main()
