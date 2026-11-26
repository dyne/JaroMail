"""Wire operations around the native contact store and shell completion."""

import re

from profile_api import ProfileApiError, run_native, text


METHODS = (
    "contacts.create-book", "contacts.rename-book", "contacts.delete-book",
    "contacts.put", "contacts.delete", "contacts.complete", "contacts.export",
)
COMPLETE = (
    'source "$JAROWORKDIR/zlibs/bootstrap" source || exit $?; '
    'list=allow; if [[ -n "$2" ]]; then list="$2"; contact_list_explicit=1; fi; complete "$1"'
)


def number(params, key):
    value = text(params, key)
    if not re.fullmatch(r"[0-9]{1,19}", value):
        raise ProfileApiError("invalid_request")
    return value


def field(value):
    if not isinstance(value, str) or "\0" in value or len(value.encode("utf-8")) > 65536:
        raise ProfileApiError("invalid_request")
    return value


def completion(workdir, root, params):
    query = field(params.get("query"))
    book = field(params.get("book", ""))
    args = ["zsh", "-fc", COMPLETE, "jaro-api-complete", query, book]
    code, output = run_native(args, root, workdir)
    if code == 1 and output.strip() == b"Not found":
        return {"recipients": []}
    if code:
        raise ProfileApiError("backend_failure")
    recipients = []
    for line in output.decode("utf-8", errors="replace").rstrip("\n").split("\n")[1:]:
        address, separator, name = line.partition("\t")
        if not address or not separator:
            raise ProfileApiError("invalid_response")
        recipients.append({"address": address, "name": name.split("\t", 1)[0]})
    return {"recipients": recipients}


def operation(workdir, root, method, params):
    action = method.removeprefix("contacts.")
    if action == "complete":
        return completion(workdir, root, params)
    book = field(text(params, "book"))
    if action == "export":
        code, raw = run_native([workdir / "bin/jaro-filter-compile", "--export-contacts", root, book], root, workdir)
        if code:
            raise ProfileApiError("invalid_contact")
        return {"vcard": raw.decode("utf-8", errors="replace")}
    args = [workdir / "bin/jaro", "contacts", action, book]
    data = b""
    if action in ("put", "delete"):
        args.extend((number(params, "id"), number(params, "revision")))
    else:
        if action == "rename-book":
            args.append(field(text(params, "name")))
        args.append(number(params, "generation"))
    if action == "put":
        addresses = params.get("addresses")
        if not isinstance(addresses, list):
            raise ProfileApiError("invalid_request")
        edit = [params.get("name"), params.get("nickname"), params.get("notes"), *addresses]
        data = ("\0".join(map(field, edit)) + "\0").encode("utf-8")
    code, raw = run_native(args, root, workdir, data, writing=True)
    if code == 3 and raw.startswith(b"JCT1\0"):
        raise ProfileApiError("contact_policy")
    if code:
        codes = {1: "contact_conflict", 2: "invalid_contact", 4: "invalid_contact", 6: "contact_conflict"}
        raise ProfileApiError(codes.get(code, "backend_failure"))
    fields = raw.decode("utf-8", errors="replace").split("\0")
    if len(fields) != 4 or fields[0] != "JCT1" or fields[3] or not fields[1].isdigit() or not fields[2].isdigit():
        raise ProfileApiError("invalid_response")
    return {"generation": fields[1], "fields": [fields[2]]}
