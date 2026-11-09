#!/bin/sh
# RetroForge: after sync.py and the build (upstream-sync.yml's env). A clean,
# passing rebase goes to $BRANCH. One that fails to build goes to sync/<tag>
# with an issue; a conflict sync.py can't settle opens an issue. One open issue
# per tag: later runs comment on it.
set -eu

report=$RUNNER_TEMP/report.md
body=$RUNNER_TEMP/issue.md
[ -f "$report" ] || : > "$report"
sync_branch=sync/$TAG
upstream_link="[$TAG](https://github.com/$UPSTREAM/releases/tag/$TAG)"

case "$RESULT/$BUILD" in
    current/*)
        echo "\`$BRANCH\` already has $upstream_link." >> "$GITHUB_STEP_SUMMARY"
        exit 0 ;;
    clean/success) state=push ;;
    clean/*) state=broken
        title="Upstream sync to $TAG: the rebase doesn't build or pass its tests" ;;
    *) state=conflict
        title="Upstream sync to $TAG: conflicts to fix" ;;
esac

if [ "$state" = push ]; then
    if [ "$DRY_RUN" = true ]; then
        { echo "Dry run: \`$BRANCH\` would move to $upstream_link."; echo; cat "$report"; } >> "$GITHUB_STEP_SUMMARY"
        exit 0
    fi
    if git push --force-with-lease="$BRANCH:$OLD" origin "HEAD:refs/heads/$BRANCH"; then
        { echo "\`$BRANCH\` is on $upstream_link, built and tested."; echo; cat "$report"; } >> "$GITHUB_STEP_SUMMARY"
        exit 0
    fi
    state=pushfail
    title="Upstream sync to $TAG: built and tested, but the push was refused"
fi

pushed=false
if [ "$state" != conflict ] && [ "$DRY_RUN" != true ] && git push --force origin "HEAD:refs/heads/$sync_branch"; then
    pushed=true
fi
git bundle create "$RUNNER_TEMP/sync.bundle" "$TAG..HEAD" 2>/dev/null || true

{
    echo "Rebasing \`$BRANCH\` onto upstream's $upstream_link ([run]($RUN_URL))."
    echo
    case "$state" in
        broken) echo "The rebase went through, but the build or its tests failed: see the run's log." ;;
        pushfail) echo "The rebase builds and passes \`linux-ci.sh\`, but the push to \`$BRANCH\` was refused." \
            "A push that rewrites workflow files needs a \`SYNC_TOKEN\` secret (fine-grained, this repository, Contents and Workflows read and write)." ;;
        conflict) echo "Nothing was pushed: fix the conflicts below on \`$BRANCH\` (rebase it onto \`$TAG\`), then close this." ;;
    esac
    echo
    if [ "$pushed" = true ]; then
        echo "The result is on [\`$sync_branch\`](https://github.com/$GITHUB_REPOSITORY/tree/$sync_branch). Once it looks right:"
        echo
        echo '```sh'
        echo "git fetch origin && git push --force-with-lease=$BRANCH:$OLD origin origin/$sync_branch:$BRANCH"
        echo '```'
        echo
    elif [ "$state" != conflict ]; then
        echo "The run's \`upstream-sync\` artifact has the result as a git bundle (\`git fetch sync.bundle HEAD\`)."
        echo
    fi
    cat "$report"
    echo
    echo "Close this issue once it's handled; scheduled runs leave $TAG alone while it's open."
} > "$body"

if [ "$DRY_RUN" = true ]; then
    { echo "Dry run, the issue it would open: **$title**"; echo; cat "$body"; } >> "$GITHUB_STEP_SUMMARY"
    exit 0
fi

gh label create upstream-sync --color D93F0B --description "The upstream sync needs you" > /dev/null 2>&1 || true
number=$(gh issue list --state open --label upstream-sync --search "$TAG in:title" --json number --jq '.[0].number // empty')
if [ -n "$number" ]; then
    gh issue comment "$number" --body-file "$body"
else
    gh issue create --title "$title" --label upstream-sync --body-file "$body"
fi
cat "$body" >> "$GITHUB_STEP_SUMMARY"
# a red run too: GitHub's email
exit 1
