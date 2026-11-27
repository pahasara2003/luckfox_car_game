#!/bin/sh
# Drive demo3d with this PC's keyboard. The board's old adbd ignores `adb shell -t`,
# so put the local terminal in raw mode ourselves: keys go out immediately, no Enter needed.
model="${1:-/tmp/teapot.obj}"
old=$(stty -g)
trap 'stty "$old"' EXIT INT TERM
stty raw -echo
adb shell /tmp/demo3d 0 keys "$model"
