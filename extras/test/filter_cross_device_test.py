"""Real two-device storage checks, with a separate exit-77 prerequisite."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
import filter_storage_test as storage


class CrossDeviceTests(storage.StorageTests):
    work_variable = 'JARO_FILTER_CROSS_WORK'

    def test_real_cross_filesystem_transfer(self):
        self.compile()
        with tempfile.TemporaryDirectory(prefix='jaro-cross-source-', dir=os.environ['JARO_FILTER_CROSS_FS_DIR']) as directory:
            owned = Path(directory)
            path = owned / 'cross:2,FS'
            data = b'Subject: cross device\n\n' + bytes(range(256)) * 4096
            path.write_bytes(data)
            source_device, destination_device = path.stat().st_dev, self.root.stat().st_dev
            self.assertNotEqual(source_device, destination_device, 'fixture must be a real cross-device transfer')
            rows = self.apply([path])
            self.assertEqual(rows[0]['status'], b'moved')
            self.assertFalse(path.exists())
            self.assertEqual(list(owned.iterdir()), [])
            self.inventory([(data, 1)])
            self.clean_private()
            self.counts['real_cross_filesystem'] += 1
            self.events.append(dict(cross_filesystem=dict(source=str(owned), source_device=source_device,
                                                         destination_device=destination_device, source_removed=True)))

    def test_pool_real_cross_filesystem_keeps_complete_singleton_protocol(self):
        paths, bodies = self.pool_messages(8)
        with tempfile.TemporaryDirectory(prefix='jaro-pool-cross-', dir=os.environ['JARO_FILTER_CROSS_FS_DIR']) as directory:
            cross = Path(directory)
            source_device, destination_device = cross.stat().st_dev, self.root.stat().st_dev
            self.assertNotEqual(source_device, destination_device, 'real cross-filesystem prerequisite')
            external = []
            for path in paths:
                target = cross / path.name
                target.write_bytes(path.read_bytes())
                path.unlink()
                external.append(target)
            rows = self.apply(external)
            self.assertEqual([row['status'] for row in rows], [b'moved'] * 8)
            self.assertTrue(all(not path.exists() for path in external))
            self.assertEqual(list(cross.iterdir()), [])
            self.assert_pool_bodies(bodies)
            self.counts['real_cross_filesystem'] += 1
            self.events.append(dict(cross_filesystem=dict(source=str(cross), source_device=source_device,
                                                         destination_device=destination_device, source_removed=True)))


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CrossDeviceTests(name) for name in sorted(CrossDeviceTests.__dict__) if name.startswith('test_'))


def prerequisite():
    cross = os.environ.get('JARO_FILTER_CROSS_FS_DIR')
    if not cross:
        return 'JARO_FILTER_CROSS_FS_DIR required for real two-device delivery'
    # Probe both actual fixture locations, including retained-work overrides.
    retained = os.environ.get(CrossDeviceTests.work_variable)
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
    program = unittest.main(exit=False, verbosity=2)
    print('Cross-device counts: ' + json.dumps(getattr(CrossDeviceTests, 'counts', {}), sort_keys=True))
    raise SystemExit(not program.result.wasSuccessful())
