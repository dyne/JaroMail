"""Return one bounded native replay, including its producer."""

import base64
import json
import os
from pathlib import Path
import stat
import sys

PRODUCERS = {"search", "notmuch", "extract", "parse", "imap"}


def main():
    if len(sys.argv) != 3:
        return 2
    cache = Path(sys.argv[1]) / "cache"
    selected = sys.argv[2]
    fd = os.open(cache, os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        if selected == "last":
            target = Path(os.readlink("replay.last", dir_fd=fd))
            if target.is_absolute():
                if target.parent != cache.absolute():
                    return 3
            elif target.parent != Path("."):
                return 3
            if not target.name.startswith("replay."):
                return 3
            selected = target.name[7:]
        if selected not in PRODUCERS:
            return 2
        file = os.open(
            "replay." + selected, os.O_NOFOLLOW | os.O_RDONLY | os.O_NONBLOCK, dir_fd=fd
        )
        with os.fdopen(file, "rb") as stream:
            info = os.fstat(file)
            if not stat.S_ISREG(info.st_mode) or info.st_size > 1 << 20:
                return 3
            data = stream.read((1 << 20) + 1)
            if len(data) != info.st_size:
                return 3
        print(
            json.dumps(
                {
                    "version": 1,
                    "producer": selected,
                    "data": base64.b64encode(data).decode(),
                }
            )
        )
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OSError:
        sys.exit(3)
