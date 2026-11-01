#!/usr/bin/env bash
# Load the vcan kernel module for the SocketCAN suites (CAN bus, CANopen).
#
# On Azure runner kernels vcan ships in linux-modules-extra-<kernel>. A new runner kernel can
# reach the runners before Ubuntu publishes that package, and installing it by the running
# kernel's name then fails. So try the module first, then the matching package, and fail with
# the kernel's name when neither works.
set -euo pipefail

if sudo modprobe vcan 2>/dev/null; then
    echo "vcan loaded (kernel $(uname -r))"
    exit 0
fi

pkg="linux-modules-extra-$(uname -r)"
sudo apt-get update -qq
if apt-cache show "$pkg" >/dev/null 2>&1; then
    sudo apt-get install -y "$pkg"
    sudo modprobe vcan
    echo "vcan loaded from $pkg"
    exit 0
fi

echo "error: kernel $(uname -r) has no vcan module and $pkg is not published" >&2
exit 1
