#!/bin/sh
# pubcheck.sh: the publication rules on the whole tracked tree (the CI; fw/tools/pre-commit checks
# the staged changes only, and can be bypassed):
#  - nothing from loader/, flash/, work/, fwbase/ (local folders, never published);
#  - no Rockchip SDK text (a local reference only): its copyright lines.
cd "$(git rev-parse --show-toplevel)" || exit 1
FAIL=0
if git ls-files | grep -E '^(loader|flash|work|fwbase)/'; then
    echo "pubcheck: local folder tracked (above)"; FAIL=1
fi
if git grep -nIiE 'fuzhou|copyright[^a-z]{0,20}(\(c\)[^a-z]{0,20})?rock-?chips?|2014 - 2014 \+ 5' \
    -- . ':!fw/tools/pre-commit' ':!fw/tools/pubcheck.sh'; then
    echo "pubcheck: Rockchip SDK text (above): the SDK is a local reference only"; FAIL=1
fi
[ $FAIL = 0 ] && echo "pubcheck: ok"
exit $FAIL
