from pathlib import Path

p = Path(__file__).with_name("pipeline-history.txt")
lines = p.read_text(encoding="utf-8").splitlines()
for line in lines:
    fields = line.split("|")
    assert len(fields) == 2 and fields[0] and fields[1], line
assert len(lines) == len(set(lines)), "a task was applied twice"
