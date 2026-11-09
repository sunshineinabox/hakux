#!/bin/sh
# Push the rebased branch, or open an issue saying why it wasn't pushed.
# Called from upstream-sync.yml, which sets the environment.
set -eu

link="[$TAG](https://github.com/$UPSTREAM/releases/tag/$TAG)"

case "$RESULT" in
current)
    echo "$BRANCH is already on $link." >> "$GITHUB_STEP_SUMMARY"
    exit 0
    ;;
clean)
    if [ "$BUILD" = success ]; then
        if [ "$DRY_RUN" = true ]; then
            echo "Would push $BRANCH rebased onto $link." >> "$GITHUB_STEP_SUMMARY"
            exit 0
        fi
        if git push --force-with-lease="$BRANCH:$OLD" origin "HEAD:$BRANCH"; then
            echo "Rebased $BRANCH onto $link." >> "$GITHUB_STEP_SUMMARY"
            exit 0
        fi
        title="Can't push the rebase onto $TAG"
        msg="The rebase builds and passes the tests, but the push was refused. If it touches workflow files, the push needs a SYNC_TOKEN secret with Contents and Workflows write access."
    else
        title="Rebase onto $TAG doesn't build"
        msg="The rebase applied cleanly, but the build or the tests failed (see the run)."
        if [ "$DRY_RUN" != true ] && git push --force origin "HEAD:refs/heads/sync/$TAG"; then
            msg="$msg It's on the sync/$TAG branch."
        fi
    fi
    ;;
*)
    title="Rebase onto $TAG has conflicts"
    msg="Nothing was pushed. $BRANCH needs rebasing onto $TAG by hand."
    ;;
esac

body=$RUNNER_TEMP/issue.md
{
    echo "$msg"
    echo
    cat "$RUNNER_TEMP/report.md" 2> /dev/null || true
    echo
    echo "Run: $RUN_URL"
} > "$body"
cat "$body" >> "$GITHUB_STEP_SUMMARY"

if [ "$DRY_RUN" = true ]; then
    exit 0
fi

gh label create upstream-sync --color d93f0b > /dev/null 2>&1 || true
issue=$(gh issue list --label upstream-sync --search "$TAG in:title" --json number --jq '.[0].number // empty')
if [ -n "$issue" ]; then
    gh issue comment "$issue" --body-file "$body"
else
    gh issue create --title "$title" --label upstream-sync --body-file "$body"
fi
# fail the run as well, so GitHub sends an email
exit 1
