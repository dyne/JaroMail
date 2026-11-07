#!/usr/bin/env python3
"""Prepare static abook dependencies, reusing Dyne's bundled ncurses first."""
import argparse
import fcntl
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

MANIFEST = Path(__file__).with_name('abook-deps.json')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bundled_ncurses(compiler, machine):
    # Dyne installs target libraries beside gcc-musl, not inside its sysroot.
    prefix = compiler.parents[2]/machine
    include, library = prefix/'include/ncursesw', prefix/'lib/libncursesw.a'
    if not include.exists() and not library.exists():
        return None
    if not library.is_file() or not (include/'ncurses.h').is_file():
        raise ValueError('incomplete bundled ncurses: '+str(prefix))
    headers = sorted(include.glob('*.h'))
    version = re.search(r'^#define NCURSES_VERSION\s+"([^"]+)"',
                        (include/'ncurses.h').read_text(),re.MULTILINE)
    if not version:
        raise ValueError('cannot identify bundled ncurses: '+str(include))
    return dict(prefix=prefix,include=include,library=library,
                identity=dict(provider='Dyne musl target libraries',version=version[1],
                              library_sha256=digest(library),
                              headers={p.name:digest(p) for p in headers}))


def stage_ncurses(bundle, installed):
    include = installed/'include'
    include.mkdir(parents=True)
    shutil.copytree(bundle['include'],include/'ncursesw')
    # Preserve nested include paths while supporting readline's <termcap.h>.
    for header in bundle['include'].glob('*.h'):
        (include/header.name).symlink_to('ncursesw/'+header.name)
    (installed/'lib').mkdir()
    shutil.copy2(bundle['library'],installed/'lib/libncursesw.a')
    notices = installed/'share/licenses/ncurses'
    notices.mkdir(parents=True)
    header = (bundle['include']/'curses.h').read_bytes()
    if not header.startswith(b'/*') or b'Permission is hereby granted' not in header.split(b'*/',1)[0]:
        raise ValueError('bundled ncurses copyright notice is missing')
    (notices/'COPYING').write_bytes(header.split(b'*/',1)[0]+b'*/\n')


def archive(source, cache, download):
    path = cache / (source['name']+'-'+source['version']+'.tar.gz')
    if path.exists():
        if digest(path) != source['sha256']:
            raise ValueError(f'archive checksum differs: {path}')
        return path
    if not download:
        raise ValueError(f'missing {path}; supply the pinned archive or pass --download')
    with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
        temporary = Path(output.name)
        try:
            with urllib.request.urlopen(source['url'], timeout=60) as response:
                shutil.copyfileobj(response, output)
            output.close()
            if digest(temporary) != source['sha256']:
                raise ValueError('download checksum differs: '+source['url'])
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    return path


def extract(path, work, source):
    directory = source['name']+'-'+source['version']
    with tarfile.open(path) as bundle:
        for member in bundle.getmembers():
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or not name.parts or name.parts[0] != directory:
                raise ValueError('unsafe dependency archive path: '+member.name)
            if not (member.isfile() or member.isdir()):
                raise ValueError('unsupported dependency archive entry: '+member.name)
        bundle.extractall(work, filter='data')
    return work / directory


def checked(command, cwd, env, log):
    with log.open('a') as output:
        output.write(shlex.join(map(str, command))+'\n')
        output.flush()
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f'command failed ({result.returncode}); see {log}')


def verify_cache(entry, identity):
    record = json.loads((entry/'build.json').read_text())
    if record['identity'] != identity:
        raise ValueError('dependency cache identity differs: '+str(entry))
    prefix = entry/'prefix'
    actual = {p.relative_to(prefix).as_posix(): digest(p) for p in prefix.rglob('*') if p.is_file()}
    if actual != record['files']:
        raise ValueError('dependency cache contents differ: '+str(entry))
    return prefix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--arch', choices=('x86_64','arm64'), required=True)
    parser.add_argument('--cache-dir', type=Path, required=True)
    parser.add_argument('--source-cache', type=Path, required=True)
    parser.add_argument('--download', action='store_true')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    compiler = args.cc.resolve(strict=True)
    machine = subprocess.check_output([str(compiler),'-dumpmachine'],text=True).strip()
    expected = 'aarch64' if args.arch == 'arm64' else 'x86_64'
    if not machine.startswith(expected+'-') or 'musl' not in machine:
        parser.error(f'compiler {machine!r} is not {args.arch} musl')
    identity = dict(manifest=digest(MANIFEST), builder=digest(Path(__file__)), compiler=digest(compiler),
                    machine=machine, version=subprocess.check_output([str(compiler),'--version'],text=True))
    bundle = bundled_ncurses(compiler,machine)
    identity['ncurses'] = bundle['identity'] if bundle else dict(provider='pinned source fallback')
    key = hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
    cache, sources = args.cache_dir.resolve(), args.source_cache.resolve()
    cache.mkdir(parents=True,exist_ok=True)
    sources.mkdir(parents=True,exist_ok=True)
    with (cache/(key+'.lock')).open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        entry = cache/key
        if entry.exists():
            print(verify_cache(entry,identity))
            return
        manifest = json.loads(MANIFEST.read_text())
        if manifest['schema_version'] != 1:
            raise ValueError('unsupported dependency source manifest')
        work = Path(tempfile.mkdtemp(prefix=key+'.work-',dir=cache))
        prefix = entry/'prefix'
        stage = work/'stage'
        installed = stage/str(prefix).lstrip('/')
        if bundle:
            stage_ncurses(bundle,installed)
        env = dict(os.environ,CC=shlex.join([str(compiler)]),CFLAGS='-O2 -std=gnu17',
                   CPPFLAGS='',LDFLAGS='-static',LIBS='',LC_ALL='C')
        env.pop('DESTDIR',None)
        launcher = shutil.which('ccache')
        if launcher:
            env['CC'] = shlex.join([launcher,str(compiler)])
        for source in manifest['sources']:
            if source['name'] == 'ncurses' and bundle:
                continue
            src = extract(archive(source,sources,args.download),work,source)
            build = work/(source['name']+'-build')
            build.mkdir()
            build_tuple = subprocess.check_output(['sh',str(src/('config.guess' if source['name']=='ncurses' else 'support/config.guess'))],text=True).strip()
            command = [src/'configure','--prefix='+str(prefix),'--host='+machine,'--build='+build_tuple]
            if source['name'] == 'ncurses':
                command += ['--without-shared','--with-normal','--enable-widec','--enable-overwrite','--without-debug','--without-ada',
                            '--without-cxx','--without-cxx-binding','--without-progs','--without-tests',
                            '--without-manpages','--disable-db-install',
                            '--with-default-terminfo-dir=/usr/share/terminfo',
                            '--with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo',
                            '--with-fallbacks=xterm,xterm-256color,screen,screen-256color,linux,vt100']
            else:
                env.update(CPPFLAGS=shlex.join(['-I'+str(installed/'include')]),
                           LDFLAGS=shlex.join(['-static','-L'+str(installed/'lib')]),LIBS='-lncursesw')
                command += ['--disable-shared','--enable-static','--with-curses','--disable-install-examples']
            log = work/(source['name']+'.log')
            checked(command,build,env,log)
            checked(['make','-j'+str(args.jobs)],build,env,log)
            checked(['make','install','DESTDIR='+str(stage)],build,env,log)
            notices = installed/'share/licenses'/source['name']
            notices.mkdir(parents=True,exist_ok=True)
            shutil.copy2(src/'COPYING',notices/'COPYING')
            shutil.rmtree(src)
            shutil.rmtree(build)
        notices = installed/'share/licenses'
        provenance = dict(manifest)
        if bundle:
            provenance['sources'] = [s for s in manifest['sources'] if s['name'] != 'ncurses']
            provenance['bundled_ncurses'] = bundle['identity']
        (notices/'sources.json').write_text(json.dumps(provenance,sort_keys=True,indent=2)+'\n')
        shutil.move(installed,work/'prefix')
        shutil.rmtree(stage)
        files = {p.relative_to(work/'prefix').as_posix():digest(p) for p in (work/'prefix').rglob('*') if p.is_file()}
        for required in ('lib/libncursesw.a','lib/libreadline.a','include/ncurses.h','include/readline/readline.h'):
            if required not in files:
                raise ValueError('missing target dependency: '+required)
        (work/'build.json').write_text(json.dumps(dict(identity=identity,files=files),sort_keys=True,indent=2)+'\n')
        work.rename(entry)
        print(verify_cache(entry,identity))


if __name__ == '__main__':
    try:
        main()
    except (OSError,ValueError,KeyError,subprocess.SubprocessError) as error:
        sys.exit('abook dependencies: '+str(error))
