"""Native Maildir ownership/publication and exact full-body accounting."""
from pathlib import Path
import hashlib
import os
import stat
import sys
import unittest
sys.dont_write_bytecode = True


def digest(data):
    return hashlib.sha256(data).hexdigest()


def body_files(root):
    rows = []
    for parent,dirs,files in os.walk(os.fsencode(root),followlinks=False):
        if b'cache' in dirs:
            dirs.remove(b'cache')
        for name in files:
            path = os.path.join(parent,name)
            st = os.lstat(path)
            relative = os.path.relpath(path,os.fsencode(root)).split(b'/')
            if stat.S_ISREG(st.st_mode) and any(part in [b'cur',b'new',b'tmp'] for part in relative[:-1]):
                data = Path(os.fsdecode(path)).read_bytes()
                rows.append(dict(path_hex=path.hex(),bytes=len(data),sha256=digest(data),mode=stat.S_IMODE(st.st_mode)))
    return sorted(rows,key=lambda row:bytes.fromhex(row['path_hex']))


# Real Dovecot commit and source disposition faults use the installed worker.
if __name__ == '__main__':
    from sieve_maildir_test import MaildirTest
    cases = ['collisions_and_partial_retry', 'interruption_during_destination_save_and_retry',
             'concurrent_publish_with_long_flag_suffix', 'interruption_after_publication_preserves_both_copies',
             'source_expunge_error_preserves_both_copies', 'changed_destination_staging_retains_source',
             'concurrent_arrival_preserves_recovery_inventory', 'hard_kill_leaves_durable_source_inventory',
             'modified_claim_after_classification_retains_edited_source',
             'published_replacement_retains_source_and_foreign_mail', 'foreign_transaction_directory_blocks_before_claim']
    suite = unittest.TestSuite(MaildirTest('test_' + case) for case in cases)
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
