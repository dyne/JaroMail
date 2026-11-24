#!/usr/bin/env python3
"""Measure equivalent contact operations in two locally staged JaroMail builds."""

import argparse
import ast
from contextlib import closing
from datetime import datetime, timezone
from functools import partial
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import sqlite3
import statistics
import subprocess
import tempfile
import time


HEADER = '[format]\nprogram=abook\nversion=0.6.2\n'
SECONDARY = 'zzalternate@bench.invalid'
BLOCKED = 'blocked@bench.invalid'
MISSING = 'absent@bench.invalid'
FILTER_ROW = re.compile(
    rb'  if address :all :is :comparator "i;ascii-casemap" "From" "([^"]+)" '
    rb'\{ fileinto :create "(INBOX|zz\.blacklist)"; stop; \}')
FILTER_REPORT = re.compile(rb'JFC1\x00([a-f0-9]{64})\x00([01])\x00([^\x00]*)\x00')


def now():
    return datetime.now(timezone.utc).isoformat()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def save_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n')


def fixture(home, count):
    """Return independently specified completion rows and filter memberships."""
    home.mkdir(mode=0o700)
    profile = home / 'Maildir'
    profile.mkdir(mode=0o700)
    for path in (home / '.abook', home / 'tmp', profile / 'Accounts', profile / 'Groups'):
        path.mkdir(mode=0o700)
    (home / '.abook/abookrc').write_text('set mutt_return_all_emails=false\n')
    (profile / 'Accounts/default.txt').write_text(
        'name Benchmark Fixture\nemail own@bench.invalid\nlogin own\n'
        'proto pop\nhost host.invalid\nport 995\n')
    (profile / 'Filters.txt').write_text('# No custom rules\n')
    (profile / 'Aliases.txt').write_text('own@bench.invalid\n')
    (profile / 'blacklist.abook').write_text(
        f'{HEADER}\n[0]\nname=Blocked Fixture\nemail={BLOCKED}\n')

    name_hit = count // 2
    prefixes = (2, count // 3, count - 2)
    mid_names = (3, count - 3)
    mid_note = count // 4
    search_cases = {
        'name-hit': ('namedneedle', [name_hit]),
        'prefix': ('prefixtag', prefixes),
        'mid-name': ('searchtag', mid_names),
        'mid-note': ('notetag', [mid_note]),
        'secondary': (SECONDARY, [count - 2]),
        'miss': ('unfindable', []),
    }
    rows = []
    addresses = []
    with (profile / 'whitelist.abook').open('w') as book:
        book.write(HEADER)
        for i in range(count - 1):
            # Leave room for implicit policy entries beneath the shared rule cap.
            mailbox_index = 0 if 10 <= i < 18 else i
            address = f'person{mailbox_index:06d}@bench.invalid'
            name = f'Person {i:06d}'
            notes = 'Sample note'
            if i == name_hit:
                name = 'Namedneedle'
            elif i in prefixes:
                name = f'Prefixtag {i:06d}'
            elif i in mid_names:
                name = f'Person Searchtag {i:06d}'
            if i == mid_note:
                notes = 'A Notetag near the middle'
            mailboxes = address + ',' + SECONDARY if i == count - 2 else address
            book.write(f'\n[{i}]\nname={name}\nemail={mailboxes}\nnotes={notes}\n')
            rows.append(f'{address}\t{name}\t{notes}\n')
            addresses.append(address)
    cases = {name: (query, 0 if indexes else 1,
                    ('\n' + ''.join(rows[i] for i in indexes)).encode() if indexes else b'')
             for name, (query, indexes) in search_cases.items()}
    cases.update({'member-hit': (SECONDARY, 0, b''), 'member-miss': (MISSING, 1, b''),
                  'filter-export': ('', 0, b'')})
    filters = [(BLOCKED.encode(), b'zz.blacklist')] + [
        (address.encode(), b'INBOX') for address in sorted(set(addresses) | {SECONDARY})]
    return cases, filters


def configuration(prefix, home):
    profile = home / 'Maildir'
    runtime = prefix / 'share/jaromail/bin'
    # Take only environment variables needed for these local executable paths.
    env = {key: os.environ[key] for key in ('PATH', 'LD_LIBRARY_PATH') if key in os.environ}
    env['PATH'] = str(runtime) + os.pathsep + env['PATH']
    env.update(HOME=str(home), TMPDIR=str(home / 'tmp'), LC_ALL='C', TZ='UTC',
               XDG_CONFIG_HOME=str(home / 'config'), XDG_CACHE_HOME=str(home / 'cache'),
               XDG_DATA_HOME=str(home / 'data'), JARO_FILTER_ENGINE='sieve',
               JAROMAILDIR=str(profile), JAROACCOUNT=str(profile / 'Accounts'))
    return prefix / 'bin/jaro', runtime, profile, env


def command(configuration, version, layer, name, query):
    cli, runtime, profile, _ = configuration
    compiler = runtime / 'jaro-filter-compile'
    if name == 'filter-export':
        if layer == 'jaro':
            return [cli, '-n', '-q', 'sieve'], b''
        return [compiler, '--root', profile, '--accounts-dir', profile / 'Accounts',
                '--report0', '--check'], b''
    if name.startswith('member-'):
        if layer == 'jaro':
            return [cli, '-q', 'isknown'], f'From: Synthetic <{query}>\n\n'.encode()
        selector = [profile / 'whitelist.abook'] if version == 'legacy' else [profile, 'allow']
        return [compiler, '--has-contact', *selector, query], b''
    if layer == 'jaro':
        return [cli, '-q', 'complete', query], b''
    if version == 'legacy':
        return [runtime / 'abook', '--config', profile.parent / '.abook/abookrc',
                '--datafile', profile / 'whitelist.abook', '--mutt-query', query], b''
    return [compiler, '--recipient-contacts', profile, query], b''


def timed(time_command, argv, payload, env, home):
    """Measure from a small timer process so Python's heap cannot set the RSS floor."""
    argv = list(map(str, argv))
    with (tempfile.TemporaryFile(dir=home) as source, tempfile.TemporaryFile(dir=home) as out,
          tempfile.TemporaryFile(dir=home) as err, tempfile.NamedTemporaryFile(dir=home) as rss):
        source.write(payload)
        source.seek(0)
        wrapped = [*time_command, '-f', '%M %U %S', '-o', rss.name, *argv]
        start = time.perf_counter_ns()
        child = subprocess.Popen(wrapped, cwd=home, env=env, stdin=source, stdout=out, stderr=err,
                                 start_new_session=True)
        try:
            signal.setitimer(signal.ITIMER_REAL, 120)
            _, status = os.waitpid(child.pid, 0)
            wall = time.perf_counter_ns() - start
        except TimeoutError:
            signal.setitimer(signal.ITIMER_REAL, 0)
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
            raise RuntimeError(f'timed out: {argv}')
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
        child.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        err.seek(0)
        stdout, stderr = out.read(), err.read()
        data = rss.read().decode().splitlines()
        if not data or len(data[-1].split()) != 3:
            raise RuntimeError(f'timer failed: {wrapped}, {stderr[:500]!r}')
        peak, user, system = data[-1].split()
    return ({'argv': argv, 'cwd': str(home), 'observedAt': now(),
             'exit': child.returncode, 'wall_ns': wall, 'peak_rss_kib': int(peak),
             'user_seconds': float(user), 'system_seconds': float(system),
             'stdout_sha256': sha(stdout), 'stderr_sha256': sha(stderr)}, stdout, stderr)


def verify(measure, configuration, version, layer, name, case):
    _, _, profile, env = configuration
    query, expected_status, expected_stdout = case
    argv, payload = command(configuration, version, layer, name, query)
    sample, stdout, stderr = measure(argv, payload, env, profile.parent)
    if name == 'miss' and (layer == 'jaro' or version == 'legacy'):
        expected_stdout = b'Not found\n'
    if name == 'filter-export' and layer == 'helper':
        report = FILTER_REPORT.fullmatch(stdout)
        correct_output = report is not None and report.groups()[1:] == (b'0', b'')
    else:
        correct_output = stdout == expected_stdout
    if sample['exit'] != expected_status or stderr or not correct_output:
        raise RuntimeError(f'{argv}: expected status {expected_status} and correct rows; got '
                           f'{sample["exit"]}, stdout {stdout[:500]!r}, stderr {stderr[:500]!r}')
    return sample


def published_filter(measure, configuration, expected):
    _, runtime, profile, env = configuration
    argv = [runtime / 'jaro-filter-compile', '--root', profile,
            '--accounts-dir', profile / 'Accounts', '--report0']
    sample, stdout, stderr = measure(argv, b'', env, profile.parent)
    report = FILTER_REPORT.fullmatch(stdout)
    if sample['exit'] or stderr or report is None or report[2] != b'1':
        raise RuntimeError(f'{argv}: publication failed: {sample["exit"]}, {stdout[:300]!r}, {stderr[:500]!r}')
    data = (profile / 'Filters.sieve').read_bytes()
    actual = [match.groups() for line in data.splitlines()
              if (match := FILTER_ROW.fullmatch(line))]
    if actual != expected:
        first = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]), None)
        raise RuntimeError(f'filter membership: {len(actual)} rows, expected {len(expected)}, first delta {first}')
    membership = b''.join(mailbox + b'\t' + folder + b'\n' for mailbox, folder in actual)
    return dict(sample, generation=report[1].decode(), sieve_sha256=sha(data),
                membership_sha256=sha(membership), membership_rows=len(actual))


def query_plans(source, database):
    """Explain the production statements using the Python SQLite runtime."""
    store = (source / 'src/contact/store.c').read_text()
    definitions = (
        ('exact', r'has_mailbox_sql\[\]\s*=([^;]+);', (1, SECONDARY)),
        ('substring', r'jaro_contact_visit\([^}]+?static const char sql\[\]\s*=([^;]+);',
         (0, 'searchtag')),
        ('filter_export', r'jaro_contact_visit_keys\([^}]+?(?="SELECT DISTINCT)("SELECT DISTINCT.*?), -1,',
         (1,)),
    )
    database_uri = database.as_uri() + '?mode=ro'
    with closing(sqlite3.connect(database_uri, uri=True)) as db:
        plans = {}
        for name, pattern, parameters in definitions:
            match = re.search(pattern, store, re.S)
            if match is None:
                raise RuntimeError(f'cannot locate production statement for {name}')
            sql = ''.join(ast.literal_eval(literal) for literal in re.findall(r'"[^"]*"', match[1]))
            plans[name] = {'sql': sql, 'parameters': parameters,
                           'plan': db.execute('EXPLAIN QUERY PLAN ' + sql, parameters).fetchall()}
        return {'sqlite_runtime': sqlite3.sqlite_version, 'plans': plans}


def identity(prefix):
    cli, runtime, _, _ = configuration(prefix, prefix)
    paths = (cli, runtime / 'jaro', runtime / 'abook', runtime / 'fetchaddr',
             runtime / 'jaro-filter-compile', runtime.parent / 'sieve/bin/sievec')
    return {str(path): sha(path.read_bytes()) for path in paths}


def collect(args):
    args.out.mkdir(mode=0o700)
    prefixes = {'legacy': args.legacy, 'sqlite': args.sqlite}
    measure = partial(timed, args.time_command)
    controls = [measure(['/bin/true'], b'', {'LC_ALL': 'C'}, args.out)[0] for _ in range(11)]
    if any(sample['exit'] for sample in controls):
        raise RuntimeError('timer control failed')
    save_json(args.out / 'identity.json', {
        'observedAt': now(), 'platform': platform.platform(),
        'python': platform.python_version(), 'sizes': args.sizes,
        'starting_runs': args.starting_runs, 'warm_runs': args.warm_runs,
        'harness_sha256': sha(Path(__file__).read_bytes()),
        'installed_binaries_sha256': {key: identity(value) for key, value in prefixes.items()},
        'time_command': args.time_command, 'time_binary_sha256': sha(Path(args.time_command[0]).read_bytes()),
        'timer_controls': controls,
        'path': os.environ['PATH'], 'ld_library_path': os.environ.get('LD_LIBRARY_PATH'),
    })
    work = []
    for count in args.sizes:
        directory = args.out / str(count)
        directory.mkdir()
        homes = {version: directory / version for version in prefixes}
        cases, expected_filter = fixture(homes['legacy'], count)
        shutil.copytree(homes['legacy'], homes['sqlite'])
        config = {version: configuration(prefix, homes[version])
                  for version, prefix in prefixes.items()}
        initial = verify(measure, config['sqlite'], 'sqlite', 'helper', 'name-hit', cases['name-hit'])
        profile = homes['sqlite'] / 'Maildir'
        database = profile / 'contacts.sqlite'
        archive_size = sum(path.stat().st_size for path in profile.glob('*.abook'))
        hashes = {str(path.relative_to(homes['legacy'])): sha(path.read_bytes())
                  for path in sorted(homes['legacy'].rglob('*')) if path.is_file()}
        # Remove the new profile's archived inputs after first-use migration.
        for path in profile.glob('*.abook'):
            path.unlink()
        save_json(directory / 'fixture.json', {
            'archive_bytes': archive_size, 'sqlite_bytes': database.stat().st_size,
            'sqlite_sha256': sha(database.read_bytes()), 'input_sha256': hashes,
            'migration': initial, 'query_plans': query_plans(args.source, database),
        })
        proofs = {}
        warm_configs = {}
        for version, prefix in prefixes.items():
            proof_home = directory / f'{version}-verified'
            shutil.copytree(homes[version], proof_home)
            proof_config = configuration(prefix, proof_home)
            operations = [dict(verify(measure, proof_config, version, layer, name, case),
                               layer=layer, case=name)
                          for layer in ('jaro', 'helper') for name, case in cases.items()]
            exported = published_filter(measure, proof_config, expected_filter)
            report = ('JFC1\0' + exported['generation'] + '\0' + '0\0\0').encode()
            check = next(row for row in operations if row['layer'] == 'helper'
                         and row['case'] == 'filter-export')
            if check['stdout_sha256'] != sha(report):
                raise RuntimeError('read-only filter export disagrees with published generation')
            proofs[version] = {'operations': operations, 'published_filter': exported}
            warm_home = directory / f'{version}-warm'
            shutil.copytree(homes[version], warm_home)
            warm_configs[version] = configuration(prefix, warm_home)
        save_json(directory / 'verified.json', proofs)
        work.append((count, directory, homes, warm_configs, cases))
        print(f'verified {count:,} contacts on both installations', flush=True)

    results = []
    with (args.out / 'samples.jsonl').open('w', buffering=1) as raw:
        for count, directory, homes, warm_configs, cases in work:
            for layer in ('jaro', 'helper'):
                for name, case in cases.items():
                    for condition, runs in (('starting', args.starting_runs), ('warm', args.warm_runs)):
                        samples = {version: [] for version in prefixes}
                        if condition == 'warm':
                            for version, config in warm_configs.items():
                                verify(measure, config, version, layer, name, case)
                        for trial in range(runs):
                            order = list(prefixes)
                            if trial % 2:
                                order.reverse()
                            for version in order:
                                if condition == 'starting':
                                    with tempfile.TemporaryDirectory(dir=directory, prefix='starting-') as temp:
                                        home = Path(temp) / 'home'
                                        shutil.copytree(homes[version], home)
                                        config = configuration(prefixes[version], home)
                                        sample = verify(measure, config, version, layer, name, case)
                                else:
                                    sample = verify(measure, warm_configs[version], version, layer, name, case)
                                row = dict(sample, contacts=count, layer=layer, case=name,
                                           condition=condition, version=version, trial=trial + 1)
                                raw.write(json.dumps(row) + '\n')
                                samples[version].append(row)
                        for version, rows in samples.items():
                            wall = sorted(row['wall_ns'] / 1_000_000 for row in rows)
                            results.append(dict(contacts=count, layer=layer, case=name,
                                                condition=condition, version=version, samples=len(rows),
                                                median_ms=statistics.median(wall),
                                                p95_ms=wall[math.ceil(len(wall) * 0.95) - 1],
                                                peak_rss_kib=max(row['peak_rss_kib'] for row in rows)))
                    save_json(args.out / 'summary.json', results)
                    print(f'timed {count:,} {layer} {name}', flush=True)


def timeout(*_):
    raise TimeoutError('benchmark subprocess timeout')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('legacy', 'sqlite', 'source', 'out'):
        parser.add_argument('--' + option, type=lambda path: Path(path).resolve(), required=True)
    parser.add_argument('--sizes', nargs='+', type=int, default=[100, 10_000, 100_000])
    parser.add_argument('--starting-runs', type=int, default=5)
    parser.add_argument('--warm-runs', type=int, default=11)
    parser.add_argument('--time-command', nargs='+', default=['/usr/bin/time'],
                        help='time command supporting -f and -o (GNU time or BusyBox time)')
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('Linux peak RSS units are required')
    if min(args.sizes) < 100 or min(args.warm_runs, args.starting_runs) < 1:
        parser.error('use sizes of at least 100 contacts and positive run counts')
    if len(args.sizes) != len(set(args.sizes)):
        parser.error('contact sizes must be distinct')
    timer = shutil.which(args.time_command[0])
    if timer is None:
        parser.error('timer executable not found; set --time-command')
    args.time_command[0] = str(Path(timer).resolve())
    signal.signal(signal.SIGALRM, timeout)
    collect(args)


if __name__ == '__main__':
    main()
