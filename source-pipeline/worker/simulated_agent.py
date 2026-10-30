#!/usr/bin/env python3
"""Deterministic vendor-neutral agent used only by the PoC tests."""
from __future__ import annotations

import os
from pathlib import Path

root = Path(os.environ["SP_WORKSPACE"])
phase = os.environ["SP_PHASE"]
if phase == "DEVELOP":
    p = root / "pipeline-history.txt"
    with p.open("a", encoding="utf-8") as f:
        f.write(f"{os.environ['SP_AGENT']}|{os.environ['SP_TASK_ID']}\n")
elif phase == "REPAIR":
    # Deliberate no-op by default. Fault tests use a separate repair fixture.
    pass
