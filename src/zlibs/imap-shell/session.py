"""Private optional IMAP shell; configuration arrives only on descriptor 3."""

import contextlib
import imaplib
import os
import shlex
import signal
import socket
import ssl
import sys

# Helpers are installed as data; invocation must not modify that installation.
sys.dont_write_bytecode = True
from folders import FolderTree
import headers


def account_record():
    fields = []
    with os.fdopen(3, "rb") as source:
        for _ in range(8):
            size = bytearray()
            while True:
                char = source.read(1)
                if char == b":":
                    break
                if not char or not char.isdigit() or len(size) >= 7:
                    raise ValueError("invalid account frame")
                size.extend(char)
            length = int(size)
            if length > 1048576:
                raise ValueError("account frame too large")
            value = source.read(length)
            if len(value) != length:
                raise ValueError("incomplete account frame")
            fields.append(value.decode("utf-8"))
        if source.read(1):
            raise ValueError("trailing account data")
    return fields


@contextlib.contextmanager
def deadline(seconds):
    def expired(signum, frame):
        raise TimeoutError("IMAP deadline exceeded")
    previous = signal.signal(signal.SIGALRM, expired)
    signal.alarm(seconds)
    try:
        yield
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous)


def server_host(host):
    # Account syntax accepts URI-style brackets; socket/SSL need the address.
    return host[1:-1] if host.startswith("[") and host.endswith("]") else host


def connect(config):
    host, port, transport, policy, ca, user, password, seconds = config
    host = server_host(host)
    port, seconds = int(port), int(seconds)
    if not host or host.startswith("-") or any(c.isspace() for c in host):
        raise ValueError("invalid host")
    if not 1 <= port <= 65535 or not 1 <= seconds <= 300:
        raise ValueError("invalid port or deadline")
    if any(c in user + password for c in "\r\n\0"):
        raise ValueError("invalid credentials")
    if policy not in ("check", "ignore") or transport not in ("implicit", "starttls", "plain"):
        raise ValueError("invalid TLS policy")
    context = None
    if transport != "plain" and policy == "check":
        context = ssl.create_default_context(cafile=ca or None)
    elif transport != "plain":
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    if transport == "plain":
        print("WARNING: IMAP plaintext explicitly requested; credentials are unencrypted", file=sys.stderr)
    elif policy == "ignore":
        print("WARNING: IMAP cert ignore: peer certificate and identity are NOT verified", file=sys.stderr)
    # Older imaplib releases reject rev2-only servers at greeting time. Its
    # existing command/parser machinery supports this shell's common commands.
    # Extend version admission only in this private interpreter, without
    # advertising capabilities the server did not send.
    imaplib.AllowedVersions = (*imaplib.AllowedVersions, "IMAP4REV2")
    client = None
    try:
        with deadline(seconds):
            if transport == "implicit":
                client = imaplib.IMAP4_SSL(host, port, ssl_context=context, timeout=seconds)
            else:
                client = imaplib.IMAP4(host, port, timeout=seconds)
                if transport == "starttls":
                    client.starttls(ssl_context=context)
            if client.state != "AUTH":
                if "LOGINDISABLED" in client.capabilities:
                    raise imaplib.IMAP4.error("LOGIN disabled")
                # imaplib quotes passwords but callers must quote user names.
                client.login(client._quote(user), password)
            client._get_capabilities()
            if "IMAP4REV2" in client.capabilities and "IMAP4REV1" not in client.capabilities:
                client._mode_utf8()
            elif "UTF8=ACCEPT" in client.capabilities and "ENABLE" in client.capabilities:
                # A dual rev1/rev2 advertisement remains rev1 until enabled.
                # imaplib.enable switches encodings on tagged OK alone; require
                # the server's explicit ENABLED confirmation before doing so.
                try:
                    status, data = client._simple_command("ENABLE", "UTF8=ACCEPT")
                    status, data = client._untagged_response(status, data, "ENABLED")
                except imaplib.IMAP4.abort:
                    raise
                except imaplib.IMAP4.error:
                    status, data = "BAD", []
                confirmed = b" ".join(item for item in data if isinstance(item, bytes)).upper().split()
                if status == "OK" and b"UTF8=ACCEPT" in confirmed:
                    client._mode_utf8()
        return client, seconds
    except BaseException:
        if client is not None:
            with contextlib.suppress(OSError):
                client.shutdown()
        raise


def command_loop(client, seconds, reconnect):
    tree = FolderTree(client, seconds, deadline)
    pager = headers.Pager()
    interactive = sys.stdin.isatty()
    if interactive:
        try:
            import readline
            readline.set_auto_history(False)
        except ImportError:
            pass
    while True:
        try:
            line = input("imap> " if interactive else "")
        except EOFError:
            return 0
        try:
            words = shlex.split(line)
        except ValueError:
            print("Invalid quoted command", file=sys.stderr)
            continue
        if not words:
            continue
        if words in (["quit"], ["exit"]):
            return 0
        if words == ["help"]:
            print("Commands: folders, pwd, cd [PATH], refresh, ls [--folders|--messages] [--limit N], more, help, quit")
        elif words == ["pwd"]:
            print(tree.current.path)
        elif words[0] in ("folders", "refresh", "ls", "cd", "more"):
            try:
                if words[0] == "cd" and len(words) <= 2:
                    tree.cd(words[1] if len(words) == 2 else "/")
                    pager.reset()
                elif words in (["folders"], ["refresh"]):
                    tree.show(refresh=words == ["refresh"])
                    if words == ["refresh"]:
                        pager.reset()
                elif words == ["more"]:
                    if not tree.current.selectable:
                        raise headers.PagingError("Choose a selectable folder")
                    pager.show(tree.client, tree.current, seconds, deadline, more=True)
                elif words[0] == "ls":
                    options, limit, mode = list(words[1:]), headers.PAGE_SIZE, None
                    seen = set()
                    while options:
                        option = options.pop(0)
                        if option in seen:
                            raise headers.PagingError("Invalid ls options; use help")
                        seen.add(option)
                        if option in ("--messages", "--folders") and mode is None:
                            mode = option
                        elif option == "--limit" and options:
                            value = options.pop(0)
                            if len(value) > 3 or not value.isascii() or not value.isdecimal() or not 1 <= int(value) <= 500:
                                raise headers.PagingError("--limit must be an integer from 1 to 500")
                            limit = int(value)
                        else:
                            raise headers.PagingError("Invalid ls options; use help")
                    if mode != "--messages":
                        tree.show()
                    if mode != "--folders" and tree.current.selectable:
                        pager.show(tree.client, tree.current, seconds, deadline, limit)
                    elif mode == "--messages":
                        raise headers.PagingError("Choose a selectable folder")
                else:
                    print("Invalid folder command; use help", file=sys.stderr)
            except headers.PagingError as exc:
                print(str(exc), file=sys.stderr)
            except (ValueError, UnicodeError):
                print("Folder operation failed", file=sys.stderr)
            except (KeyboardInterrupt, headers.LiteralLimit):
                # A partial literal leaves imaplib's parser unsynchronized.
                # Close without LOGOUT, reconnect, and revalidate the raw name
                # before another command can use this connection.
                with contextlib.suppress(OSError):
                    tree.client.shutdown()
                old_encoding = tree.client.utf8_enabled
                pager.reset()
                renewed = reconnect()
                if renewed.utf8_enabled != old_encoding:
                    tree = FolderTree(renewed, seconds, deadline)
                    print("Mailbox encoding changed; returned to /; rediscover folders", file=sys.stderr)
                else:
                    tree.client = renewed
                if tree.current.selectable:
                    with deadline(seconds):
                        headers.examine(tree.client, tree.current.raw)
                print("Header operation cancelled; connection renewed", file=sys.stderr)
        else:
            print("Unknown command; use help", file=sys.stderr)


def main():
    client = None
    seconds = 1
    try:
        config = account_record()
        client, seconds = connect(config)
        def reconnect():
            nonlocal client
            client, _ = connect(config)
            return client
        return command_loop(client, seconds, reconnect)
    except KeyboardInterrupt:
        print("IMAP session cancelled", file=sys.stderr)
        return 130
    except (TimeoutError, socket.timeout):
        print("IMAP deadline exceeded", file=sys.stderr)
        return 124
    except (OSError, ValueError, UnicodeError, imaplib.IMAP4.error) as exc:
        # Server errors may echo LOGIN arguments; never print their text.
        detail = type(exc).__name__
        if isinstance(exc, ssl.SSLCertVerificationError):
            detail += " (verification code " + str(exc.verify_code) + ")"
        print("IMAP connection or authentication failed: " + detail, file=sys.stderr)
        return 1
    finally:
        if client is not None:
            try:
                with deadline(seconds):
                    client.logout()
            except (OSError, imaplib.IMAP4.error, KeyboardInterrupt):
                pass
            finally:
                with contextlib.suppress(OSError):
                    client.shutdown()


if __name__ == "__main__":
    sys.exit(main())
