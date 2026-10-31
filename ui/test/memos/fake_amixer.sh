#!/bin/sh
# Stand-in for amixer in test/memos/run.sh: logs the call; cget of a missing control fails ($MEMOS_OLD_DRIVER)
echo "amixer $*" >> "${MEMOS_LOG:-/dev/null}"
[ -n "$MEMOS_OLD_DRIVER" ] && [ "$4" = "cget" ] && exit 1
exit 0
