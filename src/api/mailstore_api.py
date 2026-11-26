"""Page through native Maildir inventories without retaining them in memory."""

from contextlib import closing
from dataclasses import dataclass
import json
import os
import secrets
import time
from typing import Iterator, Optional

import operations


METHODS = ("maildir.messages", "maildir.release")
MAX_CURSORS = 8
PAGE_ROWS = 128
PAGE_BYTES = 768 * 1024
CURSOR_SECONDS = 60


class MailstoreApiError(Exception):
    pass


@dataclass
class Cursor:
    folder: str
    rows: Iterator
    expires: float
    buffered: Optional[dict] = None


class Mailstore:
    def __init__(self, root):
        self.root = root
        self.cursors = {}

    def release(self, token):
        cursor = self.cursors.pop(token, None)
        if cursor:
            cursor.rows.close()

    def close(self):
        for token in tuple(self.cursors):
            self.release(token)

    def expire(self):
        now = time.monotonic()
        for token, cursor in tuple(self.cursors.items()):
            if cursor.expires <= now:
                self.release(token)

    def inventory(self, folder):
        fd = os.open(self.root, operations.DIR)
        try:
            if os.fstat(fd).st_uid != os.getuid():
                raise MailstoreApiError("root_unavailable")
            with closing(operations.messages(fd, folder)) as rows:
                yield from rows
        finally:
            os.close(fd)

    def page(self, params):
        folder = params.get("folder")
        cursor_id = params.get("cursor")
        if not isinstance(folder, str) or len(os.fsencode(folder)) > 4096:
            raise MailstoreApiError("invalid_resource")
        if cursor_id is not None:
            if not isinstance(cursor_id, str) or len(cursor_id) != 32:
                raise MailstoreApiError("invalid_request")
            cursor = self.cursors.get(cursor_id)
            if cursor is None or cursor.folder != folder:
                raise MailstoreApiError("inventory_expired")
        else:
            if len(self.cursors) >= MAX_CURSORS:
                raise MailstoreApiError("busy")
            cursor_id = secrets.token_hex(16)
            cursor = Cursor(folder, self.inventory(folder), time.monotonic() + CURSOR_SECONDS)
            self.cursors[cursor_id] = cursor

        encoded = 0
        rows = []
        try:
            for _ in range(PAGE_ROWS):
                row = cursor.buffered if cursor.buffered is not None else next(cursor.rows)
                cursor.buffered = None
                encoded += len(json.dumps(row, ensure_ascii=True, separators=(",", ":"))) + 1
                if encoded >= PAGE_BYTES:
                    cursor.buffered = row
                    break
                rows.append(row)
        except StopIteration:
            self.release(cursor_id)
            return {"rows": rows, "cursor": None}
        except (OSError, ValueError) as error:
            self.release(cursor_id)
            raise MailstoreApiError("invalid_resource") from error
        except BaseException:
            self.release(cursor_id)
            raise
        cursor.expires = time.monotonic() + CURSOR_SECONDS
        return {"rows": rows, "cursor": cursor_id}

    def operation(self, method, params):
        if method == "maildir.messages":
            return self.page(params)
        cursor = params.get("cursor")
        if not isinstance(cursor, str) or len(cursor) != 32:
            raise MailstoreApiError("invalid_request")
        self.release(cursor)
        return {"released": True}
