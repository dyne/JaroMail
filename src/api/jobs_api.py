"""Keep slow native work off the request loop."""

import base64
import binascii
from concurrent.futures import ThreadPoolExecutor
import json
import tempfile
import secrets
import time

from profile_api import ProfileApiError


METHODS = (
    "jobs.start", "jobs.status", "jobs.release", "jobs.read",
    "jobs.upload-open", "jobs.upload-append", "jobs.upload-release",
)
MAX_ACTIVE = 2
MAX_RECORDS = 16
RETAIN_SECONDS = 300
INLINE = 640 * 1024
CHUNK = 192 * 1024
MAX_INPUT = 96 * 1024 * 1024
MAX_OUTPUT = 12 * 1024 * 1024
MAX_UPLOADS = 4


class Jobs:
    def __init__(self, handlers):
        self.handlers = handlers
        self.pool = ThreadPoolExecutor(max_workers=MAX_ACTIVE)
        self.records = {}
        self.uploads = {}

    def close(self):
        # Native jobs are allowed to settle and release their locks on EOF.
        self.pool.shutdown(wait=True)
        for upload in tuple(self.uploads):
            self.release_upload(upload)

    def expire(self):
        now = time.monotonic()
        for token, (future, completed) in tuple(self.records.items()):
            if future.done():
                if completed is None:
                    self.records[token] = (future, now)
                elif now - completed > RETAIN_SECONDS:
                    del self.records[token]
        for token, (_, updated) in tuple(self.uploads.items()):
            if now - updated > RETAIN_SECONDS:
                self.release_upload(token)

    def release_upload(self, ident):
        upload = self.uploads.pop(ident, None)
        if upload:
            upload[0].close()
        return {"released": True}

    def upload(self, method, params):
        self.expire()
        if method == "jobs.upload-open":
            if len(self.uploads) >= MAX_UPLOADS:
                raise ProfileApiError("busy")
            ident = secrets.token_hex(16)
            self.uploads[ident] = (tempfile.TemporaryFile(), time.monotonic())
            return {"upload": ident}
        ident = params.get("upload")
        if not isinstance(ident, str) or len(ident) != 32:
            raise ProfileApiError("invalid_request")
        if method == "jobs.upload-release":
            return self.release_upload(ident)
        entry = self.uploads.get(ident)
        if entry is None:
            raise ProfileApiError("job_unavailable")
        stream, _ = entry
        offset, data = params.get("offset"), params.get("data")
        if type(offset) is not int or offset != stream.tell() or not isinstance(data, str) or len(data) > CHUNK * 2:
            raise ProfileApiError("invalid_request")
        try:
            chunk = base64.b64decode(data, validate=True)
        except (ValueError, binascii.Error) as error:
            raise ProfileApiError("invalid_request") from error
        if not chunk or len(chunk) > CHUNK or offset + len(chunk) > MAX_INPUT:
            raise ProfileApiError("limit")
        stream.write(chunk)
        self.uploads[ident] = (stream, time.monotonic())
        return {"offset": stream.tell()}

    def data(self, params):
        if "upload" not in params:
            return params.get("args", {})
        ident = params["upload"]
        if not isinstance(ident, str) or len(ident) != 32 or "args" in params:
            raise ProfileApiError("invalid_request")
        entry = self.uploads.get(ident)
        if entry is None:
            raise ProfileApiError("job_unavailable")
        try:
            entry[0].seek(0)
            return json.load(entry[0])
        except (ValueError, RecursionError) as error:
            raise ProfileApiError("invalid_request") from error
        finally:
            self.release_upload(ident)

    @staticmethod
    def encode(handler, args):
        result = handler(args)
        raw = json.dumps(result, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        if len(raw) > MAX_OUTPUT:
            raise ProfileApiError("limit")
        return (result, None) if len(raw) <= INLINE else (None, raw)

    def start(self, params):
        self.expire()
        kind = params.get("kind")
        if not isinstance(kind, str) or kind not in self.handlers:
            raise ProfileApiError("invalid_request")
        if len(self.records) >= MAX_RECORDS or sum(not future.done() for future, _ in self.records.values()) >= MAX_ACTIVE:
            raise ProfileApiError("busy")
        args = self.data(params)
        if not isinstance(args, dict):
            raise ProfileApiError("invalid_request")
        ident = secrets.token_hex(16)
        future = self.pool.submit(self.encode, self.handlers[kind], args)
        self.records[ident] = (future, None)
        return {"job": ident}

    def status(self, params, *, reading=False):
        ident = params.get("job")
        if not isinstance(ident, str) or len(ident) != 32:
            raise ProfileApiError("invalid_request")
        record = self.records.get(ident)
        if record is None:
            raise ProfileApiError("job_unavailable")
        job, _ = record
        if not job.done():
            return {"state": "running"}
        try:
            result, raw = job.result()
            if reading:
                offset = params.get("offset")
                if raw is None or type(offset) is not int or not 0 <= offset < len(raw):
                    raise ProfileApiError("invalid_request")
                chunk = raw[offset:offset + CHUNK]
                return {"offset": offset, "data": base64.b64encode(chunk).decode("ascii")}
            if raw is not None:
                return {"state": "complete", "bytes": len(raw)}
            return {"state": "complete", "result": result}
        except ProfileApiError as error:
            return {"state": "failed", "code": str(error)}
        except (OSError, ValueError):
            return {"state": "failed", "code": "backend_failure"}

    def operation(self, method, params):
        if method.startswith("jobs.upload-"):
            return self.upload(method, params)
        if method == "jobs.start":
            return self.start(params)
        if method == "jobs.read":
            return self.status(params, reading=True)
        if method == "jobs.release":
            ident = params.get("job")
            if not isinstance(ident, str) or len(ident) != 32:
                raise ProfileApiError("invalid_request")
            record = self.records.get(ident)
            if record and record[0].done():
                del self.records[ident]
            return {"released": not record or record[0].done()}
        return self.status(params)
