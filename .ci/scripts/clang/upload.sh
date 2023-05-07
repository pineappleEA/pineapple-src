#!/bin/bash -ex

# SPDX-FileCopyrightText: 2021 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

. .ci/scripts/common/pre-upload.sh

REV_NAME="yuzu-linux-${GITDATE}-${GITREV}"
ARCHIVE_NAME="${REV_NAME}.tar.xz"
COMPRESSION_FLAGS="-cJvf"

if [ "${RELEASE_NAME}" = "mainline" ]; then
    DIR_NAME="${REV_NAME}"
else
    DIR_NAME="${REV_NAME}_${RELEASE_NAME}"
fi

mkdir "$DIR_NAME"

cp build/bin/yuzu-cmd "$DIR_NAME"
cp build/bin/yuzu "$DIR_NAME"

. .ci/scripts/common/post-upload.sh
