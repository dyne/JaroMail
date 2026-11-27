"""Terminal presentation for the private IMAP command loop."""

import os
import sys
import textwrap
import unicodedata


def columns():
    try:
        width = os.get_terminal_size(sys.stdout.fileno()).columns
    except OSError:
        return None
    return width or 80


HELP = (
    "Commands: type quit or press Ctrl-D to leave.",
    "folders | pwd | cd [PATH] | refresh",
    "Use / between folders, even on dot-delimited servers. Quote spaces. "
    "Copy displayed paths, including any % escapes.",
    "ls [--folders | --messages] [--limit N] | more",
    "Page by newest UID first (default 50, limit 1-500). Date is the message "
    "header, with INTERNALDATE as fallback. Run ls again for new arrivals.",
    "du [PATH] [--exact] [--recursive] [--depth N] [--top N]",
    "Rank direct message bytes with du / --depth 2 --top 10. Measured bytes "
    "may count shared copies twice. Partial totals are lower bounds. Quota is separate.",
    "mkdir PATH | mv OLD NEW (alias rename)",
    "Give the complete new path under an existing parent.",
    "rename-glob 'List-*' 'Lists/{1}' | apply ID",
    "Review each mapping, then use its ID in this session and confirm on the terminal.",
    "rmdir EXACT-PATH [--recursive] [--yes]",
    "Deletes messages too. Review targets first. Scripts need explicit --yes.",
    "download FOLDER --output PATH.tar.gz [--recursive] [--resume]",
    "Allow space for raw mail plus the archive and inventory. Repeat an "
    "interrupted download with the same arguments and --resume.",
    "From your system shell, check gzip -t PATH.tar.gz and tar -tzf PATH.tar.gz. "
    "See man jaro for account settings, manifest verification and a worked session.",
)


def help_text():
    width = columns() or 80
    for line in HELP:
        print(textwrap.fill(line, width=width))


def header_page(rows):
    labels = ("UID", "SIZE", "DATE", "FROM", "SUBJECT")
    width = columns()
    if width is None or width >= 80:
        print("\t".join(labels))
        for _, line in rows:
            print(line)
        return
    # Preserve the complete fields on a small terminal. Combining marks use
    # the preceding cell, while full-width and wide characters occupy two.
    for _, line in rows:
        for label, value in zip(labels, line.split("\t")):
            cells, part = 0, []
            for char in label + " " + value:
                size = 0 if unicodedata.combining(char) else 2 if unicodedata.east_asian_width(char) in "WF" else 1
                if cells and cells + size > width:
                    print("".join(part))
                    cells, part = 0, []
                part.append(char)
                cells += size
            print("".join(part))
