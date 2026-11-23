"""Large sparse metadata-only Maildirs for the index/RSS check, excluded from I/O timings."""
import argparse
import json
import os
from pathlib import Path
import stat
import sys

from dedup_corpus import reserve


def run(root, count, verify, guest):
    boxes = ([root / f"box{i:04d}" for i in range(2048)] if guest else
             [root / f"box{i:02d}" for i in range(16)])
    parents = [box / sub for box in boxes for sub in ("cur", "new")]
    if not verify:
        reserve(root.parent, count * 2048, count + 4 * len(boxes))
        root.mkdir(mode=0o700)
        for box in boxes:
            for sub in (("cur", "new") if guest else ("cur", "new", "tmp")):
                (box / sub).mkdir(parents=True)
    else:
        entries = sum(sum(1 for _ in os.scandir(d)) for d in parents)
        if entries != count:
            raise ValueError(f"expected {count} names, found {entries}")
    total_blocks = 0
    for i in range(count):
        name = f"{i:011d}" + "s" * 240 if guest else f"{i:010x}." + "q" * 236 + ":2,S"
        path = parents[i % len(parents)] / name
        size = 0 if guest else i
        if verify:
            st = path.lstat()
            if st.st_size != size or st.st_nlink != 1 or not stat.S_ISREG(st.st_mode):
                raise ValueError(f"wrong sparse fixture: {path}")
            total_blocks += st.st_blocks
        elif guest:
            os.close(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600))
        else:
            with path.open("xb") as f:
                if size:
                    f.truncate(size)
    print(json.dumps(dict(count=count, unique_sizes=1 if guest else count, basename_bytes=251,
                          allocated_body_bytes=total_blocks * 512 if verify else None,
                          stage="verify" if verify else "generate")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--count", type=int)
    parser.add_argument("--guest-inventory", action="store_true", help="900k empty names across 2048 Maildirs")
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    count = args.count if args.count is not None else (900000 if args.guest_inventory else 2100000)
    if count < 1:
        sys.exit("count must be positive")
    run(args.root, count, args.verify, args.guest_inventory)
