"""Real two-device storage checks, with a separate exit-77 prerequisite."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
def prerequisite():
    cross = os.environ.get('JARO_FILTER_CROSS_FS_DIR')
    if not cross:
        return 'JARO_FILTER_CROSS_FS_DIR required for real two-device delivery'
    # Probe both actual fixture locations, including retained-work overrides.
    retained = os.environ.get('JARO_FILTER_CROSS_WORK')
    destination = Path(retained).parent if retained else os.environ.get('JARO_FILTER_TEST_TMP') or os.environ.get('TMPDIR')
    try:
        with tempfile.TemporaryDirectory(prefix='jaro-cross-probe-', dir=cross) as source:
            with tempfile.TemporaryDirectory(prefix='jaro-local-probe-', dir=destination) as target:
                (Path(source) / 'writable').write_bytes(b'probe')
                (Path(target) / 'writable').write_bytes(b'probe')
                if Path(source).stat().st_dev == Path(target).stat().st_dev:
                    return 'JARO_FILTER_CROSS_FS_DIR and local fixtures must be on distinct devices'
    except OSError as error:
        return 'writable two-device fixture roots required: ' + str(error)
    return None


if __name__ == '__main__':
    missing = prerequisite()
    if missing:
        print('SKIP: ' + missing, file=sys.stderr)
        raise SystemExit(77)
    from sieve_maildir_test import MaildirTest
    suite = unittest.TestSuite([MaildirTest('test_distinct_devices')])
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
