#!/usr/bin/env python3
"""RetroForge: rebase this checkout's commits (HEAD) onto an upstream hakuX-lr tag.

    python3 sync.py <tag> <report.md>

CHANGELOG.md conflicts are settled here: upstream's file with our
"## linux-v<version>-rf.<n>" section on top. Either way our section is renamed
for the tag (rf.1): the release job wants a section named after the tag. Any
other conflict stops the rebase (aborted, HEAD as it was) and the report
shows its hunks.

Prints result=current|clean|failed for $GITHUB_OUTPUT; the report is Markdown
for an issue or the run's summary.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

MAX_COMMITS = 10            # more to replay: HEAD's base isn't in the tag's history
MAX_SHOWN = 6000            # characters of conflict hunks shown per file

OUR_HEADING = re.compile(r'^## linux-v(\d+(?:\.\d+)+)-rf\.(\d+)\b[^\n]*\n', re.M)
HUNK = re.compile(r'^<<<<<<< [^\n]*\n.*?^>>>>>>> [^\n]*\n', re.M | re.S)

os.environ['GIT_EDITOR'] = 'true'


class Failed(Exception):
    pass


def git(*args, check=True):
    r = subprocess.run(['git', *args], capture_output=True, text=True)
    if check and r.returncode:
        raise Failed(f'`git {" ".join(args)}`: {(r.stderr or r.stdout).strip()[-600:]}')
    return r


def rebasing():
    d = Path(git('rev-parse', '--git-dir').stdout.strip())
    return (d / 'rebase-merge').is_dir() or (d / 'rebase-apply').is_dir()


def stage(n, path):
    r = git('show', f':{n}:{path}', check=False)
    return r.stdout if r.returncode == 0 else None


# Our CHANGELOG section: from our heading to the next "## ".
def our_section(text):
    m = OUR_HEADING.search(text or '')
    if not m:
        return None
    end = re.compile(r'^## ', re.M).search(text, m.end())
    return text[m.start():end.start() if end else len(text)]


def retarget(section, version):
    m = OUR_HEADING.match(section)
    old, n = m.group(1), m.group(2)
    if old == version:
        return section
    return section.replace(f'{old}-rf.{n}', f'{version}-rf.1').replace(f'hakuX-lr {old} ', f'hakuX-lr {version} ')


def place(upstream, section):
    m = re.compile(r'^## linux-v', re.M).search(upstream)
    at = m.start() if m else len(upstream)
    return upstream[:at] + section.rstrip('\n') + '\n\n' + upstream[at:]


def settle_changelog(version):
    up, ours = stage(2, 'CHANGELOG.md'), stage(3, 'CHANGELOG.md')
    section = our_section(ours)
    if up is None or section is None:
        return False
    old = our_section(up)
    if old:
        up = up.replace(old, '', 1)
    Path('CHANGELOG.md').write_text(place(up, retarget(section, version)))
    return True


def retarget_changelog(tag, version):
    """After the rebase: our section named for the tag, folded into the commit that wrote it."""
    p = Path('CHANGELOG.md')
    text = p.read_text() if p.exists() else ''
    section = our_section(text)
    if not section or retarget(section, version) == section:
        return False
    p.write_text(text.replace(section, retarget(section, version), 1))
    target = git('log', '-1', '--format=%H', f'{tag}..HEAD', '--', 'CHANGELOG.md').stdout.strip()
    git('commit', '-q', '--fixup', target, '--', 'CHANGELOG.md')
    env = dict(os.environ, GIT_SEQUENCE_EDITOR='true')
    r = subprocess.run(['git', 'rebase', '-q', '-i', '--autosquash', tag], env=env, capture_output=True, text=True)
    if r.returncode:
        raise Failed(f'renaming our CHANGELOG section: {r.stderr.strip()[-600:]}')
    return True


def hunks(path):
    """The file's conflict hunks for the report (HEAD is upstream's side, the other ours)."""
    try:
        found = HUNK.findall(Path(path).read_text())
    except (OSError, UnicodeDecodeError):
        return f'`{path}`: no text to show (deleted on one side, or binary).\n'
    if not found:
        return f'`{path}`: no conflict markers (deleted or renamed on one side).\n'
    shown = ''.join(found)
    if len(shown) > MAX_SHOWN:
        shown = shown[:MAX_SHOWN] + '\n[... cut]\n'
    return f'`{path}`, {len(found)} hunk(s):\n\n````diff\n{shown}````\n'


def main():
    tag, report_path = sys.argv[1], Path(sys.argv[2])
    version = tag.removeprefix('linux-v')
    report = []
    result = 'failed'
    try:
        if git('merge-base', '--is-ancestor', tag, 'HEAD', check=False).returncode == 0:
            result = 'current'
            return
        commits = git('log', '--reverse', '--format=%h %s', 'HEAD', '--not', tag).stdout.splitlines()
        if len(commits) > MAX_COMMITS:
            raise Failed(f"{len(commits)} commits to replay: is HEAD's base in {tag}'s history?")
        report.append('Our commits: ' + ', '.join(f'`{c}`' for c in commits) + '.\n')
        r = git('-c', 'merge.conflictStyle=diff3', 'rebase', tag, check=False)
        while rebasing():
            files = git('diff', '--name-only', '--diff-filter=U').stdout.split()
            current = git('log', '-1', '--format=%h %s', 'REBASE_HEAD').stdout.strip()
            if not files:
                r = git('rebase', '--continue', check=False)
                if r.returncode and rebasing() and not git('diff', '--name-only', '--diff-filter=U').stdout.strip():
                    raise Failed(f'the rebase stopped at `{current}`: {(r.stderr or r.stdout).strip()[-600:]}')
                continue
            report.append(f'Conflicts applying `{current}`: ' + ', '.join(f'`{f}`' for f in files) + '.\n')
            left = []
            for f in files:
                if f == 'CHANGELOG.md' and settle_changelog(version):
                    report.append("`CHANGELOG.md`: upstream's file, our section on top, named for the tag.\n")
                    git('add', '--', f)
                else:
                    report.append(hunks(f))
                    left.append(f)
            if left:
                raise Failed(', '.join(f'`{f}`' for f in left) + (' needs' if len(left) == 1 else ' need') + f' a hand (`{current}`)')
            r = git('rebase', '--continue', check=False)
        if r.returncode and not rebasing():
            raise Failed(f'the rebase failed: {(r.stderr or r.stdout).strip()[-600:]}')
        if retarget_changelog(tag, version):
            report.append('`CHANGELOG.md`: our section renamed for the tag.\n')
        result = 'clean'
    except Exception as e:      # Failed, or a bug here: the issue says which
        report.append(f'**Stopped:** {e if isinstance(e, Failed) else repr(e)}\n')
        if rebasing():
            git('rebase', '--abort', check=False)
    finally:
        report_path.write_text('\n'.join(report))
        print(f'result={result}')


if __name__ == '__main__':
    main()
