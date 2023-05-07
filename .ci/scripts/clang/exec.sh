#!/bin/bash -ex

# SPDX-FileCopyrightText: 2021 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

mkdir -p "ccache"  || true
chmod a+x ./.ci/scripts/clang/docker.sh
# the UID for the container yuzu user is 1027
sudo chown -R 1027 ./
docker run -e ENABLE_COMPATIBILITY_REPORTING -e CCACHE_DIR=/yuzu/ccache -v "$(pwd):/yuzu" -w /yuzu yuzuemu/build-environments:linux-fresh /bin/bash /yuzu/.ci/scripts/clang/docker.sh "$1"
sudo chown -R $UID ./
