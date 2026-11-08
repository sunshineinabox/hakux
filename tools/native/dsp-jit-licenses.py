#!/usr/bin/env python3
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
"""Collect the pinned Rust workspace's license notices for the Linux archive."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--cargo', default='cargo')
    args = parser.parse_args()
    source = args.source.resolve()
    # the host's crates: the library is built for it (aarch64 or x86_64 Linux)
    host = next(line.split(None, 1)[1] for line in output('rustc', '-vV').splitlines()
                if line.startswith('host: '))
    metadata = json.loads(output(args.cargo, 'metadata', '--locked', '--format-version', '1',
                                 '--filter-platform', host,
                                 '--manifest-path', str(source / 'Cargo.toml')))
    toolchain = Path(output('rustc', '--print', 'sysroot'))
    rust_notices = toolchain / 'share' / 'doc' / 'rust'
    destination = args.destination
    destination.mkdir(parents=True, exist_ok=True)
    notices = []
    # Include build and test dependencies too: this is deliberately a superset
    # of the crates whose code the final static library retains.
    for package in sorted(metadata['packages'], key=lambda p: (p['name'], p['version'])):
        root = Path(package['manifest_path']).parent
        files = set()
        for parent in (root, source if root.is_relative_to(source) else root):
            files.update(p for p in parent.iterdir() if p.is_file() and
                         p.name.lower().startswith(('license', 'copying', 'notice', 'copyright')))
        if package.get('license_file'):
            files.add(root / package['license_file'])
        name = f"{package['name']}-{package['version']}"
        target = destination / name
        target.mkdir(exist_ok=True)
        if not files:
            # Cranelift crates publish their SPDX expression without standalone
            # license files. Rust ships the same Apache and LLVM terms.
            expression = package.get('license')
            if not expression:
                raise SystemExit(f'No license expression or notices for {name}')
            identifiers = set(expression.replace('(', ' ').replace(')', ' ').split())
            for identifier in sorted(identifiers - {'AND', 'OR', 'WITH'}):
                path = rust_notices / 'licenses' / f'{identifier}.txt'
                if not path.is_file():
                    raise SystemExit(f'No license text for {name}: {identifier}')
                files.add(path)
            readme = package.get('readme')
            if readme and (root / readme).is_file():
                files.add(root / readme)
            (target / 'ATTRIBUTION').write_text(
                name + '\n' + '\n'.join(package.get('authors', [])) + '\n' +
                f"License: {expression}\nRepository: {package.get('repository')}\n")
        for path in sorted(files):
            shutil.copyfile(path, target / path.name)
        notices.append({'name': package['name'], 'version': package['version'],
                        'license': package.get('license'), 'source': package.get('source'),
                        'authors': package.get('authors'), 'repository': package.get('repository'),
                        'notices': [f'{name}/{p.name}' for p in sorted(files)]})
    target = destination / 'rust-toolchain'
    target.mkdir(exist_ok=True)
    rust_files = [p for p in rust_notices.iterdir() if p.is_file() and
                  p.name.lower().startswith(('copyright', 'license'))]
    library_copyright = rust_notices / 'COPYRIGHT-library.html'
    if library_copyright.is_file():
        rust_files = [p for p in rust_files if p.name.lower().startswith('license')]
        rust_files.append(library_copyright)
    if not any(p.name.lower().startswith('copyright') for p in rust_files):
        raise SystemExit(f'No Rust standard-library copyright notice in {rust_notices}')
    for path in rust_files:
        shutil.copyfile(path, target / path.name)
    if (rust_notices / 'licenses').is_dir():
        shutil.copytree(rust_notices / 'licenses', target / 'licenses', dirs_exist_ok=True)
    elif not all((target / name).is_file() for name in ('LICENSE-APACHE', 'LICENSE-MIT')):
        raise SystemExit(f'No Rust standard-library license texts in {rust_notices}')
    (destination / 'packages.json').write_text(json.dumps(notices, indent=2) + '\n')
    (destination / 'BUILD-INFO').write_text(output('rustc', '--version') + '\n' +
                                           output(args.cargo, '--version') + '\n')
    print(f'Collected Rust license notices for {len(notices)} packages')


if __name__ == '__main__':
    main()
