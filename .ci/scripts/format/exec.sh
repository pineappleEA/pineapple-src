#!/bin/bash -ex

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

chmod a+x ./.ci/scripts/format/docker.sh
# the UID for the container yuzu user is 1027
sudo chown -R 1027 ./
docker run -v "$(pwd):/yuzu" -w /yuzu yuzuemu/build-environments:linux-clang-format /bin/bash -ex /yuzu/.ci/scripts/format/docker.sh
sudo chown -R $UID ./
