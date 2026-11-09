#!/usr/bin/env python3
"""Rebase HEAD onto an upstream hakuX-lr release tag.

usage: sync.py TAG REPORT

A CHANGELOG.md conflict is resolved by taking upstream's file and putting our
section back on top. Either way our section gets renamed for the new version,
since the release job wants a section named after the tag. Any other conflict
aborts the rebase.

Prints result=current, clean or failed, and writes a short Markdown report.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

OUR_HEADING = re.compile(r'^## linux-v(\d+(?:\.\d+)+)-rf\.(\d+)\b.*\n', re.M)
CONFLICT = re.compile(r'^<<<<<<< .*?^>>>>>>> .*?\n', re.M | re.S)


def git(*args, check=True):
    r = subprocess.run(['git', *args], capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f'git {" ".join(args)} failed: {r.stderr.strip()}')
    return r


def rebasing():
    git_dir = Path(git('rev-parse', '--git-dir').stdout.strip())
    return (git_dir / 'rebase-merge').exists() or (git_dir / 'rebase-apply').exists()


def our_section(text):
    m = OUR_HEADING.search(text)
    if not m:
        return None
    end = re.compile(r'^## ', re.M).search(text, m.end())
    return text[m.start():end.start() if end else None]


def renamed(section, version):
    old, n = OUR_HEADING.match(section).groups()
    if old == version:
        return section
    return (section.replace(f'{old}-rf.{n}', f'{version}-rf.1')
                   .replace(f'hakuX-lr {old} ', f'hakuX-lr {version} '))


def fix_changelog(version):
    # during a rebase, stage 2 is upstream's side and stage 3 is our commit's
    upstream = git('show', ':2:CHANGELOG.md', check=False).stdout
    section = our_section(git('show', ':3:CHANGELOG.md', check=False).stdout)
    if not upstream or not section:
        return False
    first = re.compile(r'^## linux-v', re.M).search(upstream)
    at = first.start() if first else len(upstream)
    section = renamed(section, version).rstrip('\n') + '\n\n'
    Path('CHANGELOG.md').write_text(upstream[:at] + section + upstream[at:])
    return True


def rename_section(tag, version):
    # after a clean rebase: rename it and squash that into the commit that added it
    path = Path('CHANGELOG.md')
    text = path.read_text()
    section = our_section(text)
    if not section or renamed(section, version) == section:
        return False
    path.write_text(text.replace(section, renamed(section, version)))
    commit = git('log', '-1', '--format=%H', f'{tag}..', '--', 'CHANGELOG.md').stdout.strip()
    git('commit', '-q', '--fixup', commit, 'CHANGELOG.md')
    subprocess.run(['git', 'rebase', '-q', '-i', '--autosquash', tag], check=True,
                   env=dict(os.environ, GIT_SEQUENCE_EDITOR='true'))
    return True


def conflict_hunks(path):
    try:
        hunks = CONFLICT.findall(Path(path).read_text())
    except (OSError, UnicodeDecodeError):
        hunks = []
    if not hunks:
        return f'`{path}` (no conflict markers, probably deleted or renamed on one side)\n'
    text = ''.join(hunks)
    if len(text) > 6000:
        text = text[:6000] + '...\n'
    return f'`{path}`:\n\n````\n{text}````\n'


def rebase(tag, report):
    version = tag.removeprefix('linux-v')
    if git('merge-base', '--is-ancestor', tag, 'HEAD', check=False).returncode == 0:
        return 'current'
    commits = git('log', '--reverse', '--format=%h %s', f'{tag}..HEAD').stdout.splitlines()
    if len(commits) > 10:
        raise RuntimeError(f'{len(commits)} commits to replay, is HEAD based on an older {tag}?')
    report.append('Replaying:\n\n' + ''.join(f'- {c}\n' for c in commits))

    r = git('-c', 'merge.conflictStyle=diff3', 'rebase', tag, check=False)
    if r.returncode and not rebasing():
        raise RuntimeError(f'rebase failed: {r.stderr.strip()}')
    while rebasing():
        commit = git('log', '-1', '--format=%h %s', 'REBASE_HEAD').stdout.strip()
        left = []
        for path in git('diff', '--name-only', '--diff-filter=U').stdout.split():
            if path == 'CHANGELOG.md' and fix_changelog(version):
                git('add', path)
                report.append('Resolved the CHANGELOG.md conflict.\n')
            else:
                left.append(path)
        if left:
            report.append(f'Conflicts in {commit}:\n')
            report.extend(conflict_hunks(path) for path in left)
            git('rebase', '--abort')
            return 'failed'
        r = git('rebase', '--continue', check=False)
        if r.returncode and rebasing() and not git('diff', '--name-only', '--diff-filter=U').stdout:
            raise RuntimeError(f'rebase stopped at {commit}: {r.stderr.strip()}')

    if rename_section(tag, version):
        report.append(f'Renamed our CHANGELOG.md section for {version}.\n')
    return 'clean'


def main():
    tag, report_file = sys.argv[1:3]
    os.environ['GIT_EDITOR'] = 'true'
    report = []
    try:
        result = rebase(tag, report)
    except Exception as e:
        report.append(f'Error: {e}\n')
        if rebasing():
            git('rebase', '--abort', check=False)
        result = 'failed'
    Path(report_file).write_text('\n'.join(report))
    print(f'result={result}')


if __name__ == '__main__':
    main()
