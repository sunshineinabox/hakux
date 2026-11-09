#!/usr/bin/env python3
"""RetroForge: rebase this checkout's commits (HEAD) onto an upstream hakuX-lr tag.

    python3 sync.py <tag> <report.md>

CHANGELOG.md conflicts are settled here: upstream's file with our
"## linux-v<version>-rf.<n>" section on top. Every other conflict hunk goes to
a GitHub Models chat model (SYNC_MODELS, comma-separated, the first that
answers; GITHUB_TOKEN with models: read). Either way our section is renamed
for the tag (rf.1): the release job wants a section named after the tag.

Prints result=current|clean|resolved|failed for $GITHUB_OUTPUT ("resolved":
a model settled something); the report is Markdown for an issue or the run's
summary. A failed rebase is aborted, HEAD as it was.
"""
import difflib
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

MODELS = [m.strip() for m in os.environ.get('SYNC_MODELS', '').split(',') if m.strip()] \
    or ['openai/gpt-4.1', 'openai/gpt-4o']
ENDPOINT = 'https://models.github.ai/inference/chat/completions'
MAX_PROMPT = 24000          # characters: the free tier takes about 8000 tokens a request
MAX_COMMITS = 10            # more to replay: HEAD's base isn't in the tag's history

OUR_HEADING = re.compile(r'^## linux-v(\d+(?:\.\d+)+)-rf\.(\d+)\b[^\n]*\n', re.M)
HUNK = re.compile(r'^<<<<<<< [^\n]*\n(.*?)^\|\|\|\|\|\|\| [^\n]*\n(.*?)^=======\n(.*?)^>>>>>>> [^\n]*\n',
                  re.M | re.S)
MARKERS = re.compile(r'^(<<<<<<<|\|\|\|\|\|\|\||>>>>>>>)( |$)', re.M)
REPLY = re.compile(r'<<<MERGED\n(.*?)\n?MERGED>>>', re.S)

SYSTEM = """You resolve git rebase conflicts for a fork of an Xbox emulator, \
portare-ch/hakuX-lr. The fork's commits add RetroForge's changes on top of upstream.

In each conflict, UPSTREAM is upstream's new text, BASE is the text both sides \
started from, and OURS is the fork commit's version of BASE. Write the text \
that keeps every upstream change and applies the fork commit's change (the \
difference between BASE and OURS) on top of it, in upstream's style.

Reply with the merged text only, between a line <<<MERGED and a line MERGED>>>. \
No conflict markers, no commentary. If the changes can't be combined safely, \
reply with the single word UNRESOLVABLE."""

PROMPT = """File: {path}
Fork commit: {message}

Lines before the conflict:
{before}
UPSTREAM:
{up}
BASE:
{base}
OURS:
{ours}
Lines after the conflict:
{after}"""

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


def ask(prompt):
    errors = []
    for model in MODELS:
        body = json.dumps({'model': model, 'temperature': 0, 'max_tokens': 4000,
                           'messages': [{'role': 'system', 'content': SYSTEM},
                                        {'role': 'user', 'content': prompt}]}).encode()
        for attempt in range(3):
            req = urllib.request.Request(ENDPOINT, body, {
                'Authorization': f'Bearer {os.environ.get("GITHUB_TOKEN", "")}',
                'Accept': 'application/vnd.github+json',
                'X-GitHub-Api-Version': '2022-11-28',
                'Content-Type': 'application/json'})
            try:
                with urllib.request.urlopen(req, timeout=180) as r:
                    raw = r.read()
                    try:
                        return model, json.loads(raw)['choices'][0]['message']['content']
                    except ValueError:
                        errors.append(f'{model}: HTTP {r.status} from {r.geturl()}, '
                                      f'{r.headers.get("content-type")}, not JSON: {raw[:200]!r}')
                        break
            except urllib.error.HTTPError as e:
                detail = e.read().decode(errors='replace')[:300]
                if e.code == 429 and attempt < 2:
                    time.sleep(min(int(e.headers.get('retry-after') or 30), 90))
                    continue
                errors.append(f'{model}: HTTP {e.code} {detail}')
            except (OSError, KeyError, IndexError, ValueError) as e:
                errors.append(f'{model}: {e}')
            break
    raise Failed('no model answered: ' + '; '.join(errors))


def fence(text):
    return '```\n' + text + ('' if text.endswith('\n') or not text else '\n') + '```\n'


def settle_with_model(path, message, report):
    try:
        text = Path(path).read_text()
    except (OSError, UnicodeDecodeError) as e:
        raise Failed(f'`{path}`: not a text file with conflict markers ({e})')
    out, pos, hunks = [], 0, 0
    for m in HUNK.finditer(text):
        up, base, ours = m.groups()
        prompt = PROMPT.format(path=path, message=message.strip(),
                               before=fence('\n'.join(text[:m.start()].splitlines()[-20:])),
                               after=fence('\n'.join(text[m.end():].splitlines()[:20])),
                               up=fence(up), base=fence(base), ours=fence(ours))
        if len(prompt) > MAX_PROMPT:
            raise Failed(f'`{path}`: a conflict too big for the free tier ({len(prompt)} characters)')
        model, reply = ask(prompt)
        got = REPLY.search(reply)
        if not got or reply.strip() == 'UNRESOLVABLE':
            raise Failed(f'`{path}`: {model} could not merge a hunk')
        merged = got.group(1)
        if merged and not merged.endswith('\n'):
            merged += '\n'
        if MARKERS.search(merged):
            raise Failed(f'`{path}`: {model} left conflict markers')
        diff = ''.join(difflib.unified_diff(up.splitlines(True), merged.splitlines(True),
                                            'upstream', f'merged by {model}'))
        shown = diff or "(upstream's text, unchanged)\n"
        report.append(f"`{path}`, hunk {hunks + 1} ({model}), against upstream's side:\n\n"
                      f'```diff\n{shown}```\n')
        out += [text[pos:m.start()], merged]
        pos, hunks = m.end(), hunks + 1
    if not hunks:
        raise Failed(f'`{path}`: no conflict hunks to merge (deleted on one side, or binary)')
    merged = ''.join(out) + text[pos:]
    if MARKERS.search(merged):
        raise Failed(f'`{path}`: conflict markers left after merging')
    Path(path).write_text(merged)


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
        used_model = False
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
            message = git('log', '-1', '--format=%B', 'REBASE_HEAD').stdout
            for f in files:
                if f == 'CHANGELOG.md' and settle_changelog(version):
                    report.append("`CHANGELOG.md`: upstream's file, our section on top, named for the tag.\n")
                else:
                    settle_with_model(f, message, report)
                    used_model = True
                git('add', '--', f)
            r = git('rebase', '--continue', check=False)
        if r.returncode and not rebasing():
            raise Failed(f'the rebase failed: {(r.stderr or r.stdout).strip()[-600:]}')
        if retarget_changelog(tag, version):
            report.append('`CHANGELOG.md`: our section renamed for the tag.\n')
        result = 'resolved' if used_model else 'clean'
    except Exception as e:      # Failed, or a bug here: the issue says which
        report.append(f'**Stopped:** {e if isinstance(e, Failed) else repr(e)}\n')
        if rebasing():
            git('rebase', '--abort', check=False)
    finally:
        report_path.write_text('\n'.join(report))
        print(f'result={result}')


if __name__ == '__main__':
    main()
