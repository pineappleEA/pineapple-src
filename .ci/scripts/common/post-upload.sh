#!/bin/bash -ex

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Copy documentation
cp LICENSE.txt "$DIR_NAME"
cp README.md "$DIR_NAME"

if [[ -z "${NO_SOURCE_PACK}" ]]; then
  git clone --depth 1 file://$(readlink -e .) ${REV_NAME}-source
  tar -cJvf "${REV_NAME}-source.tar.xz" ${REV_NAME}-source
  cp -v "${REV_NAME}-source.tar.xz" "$DIR_NAME"
  cp -v "${REV_NAME}-source.tar.xz" "${ARTIFACTS_DIR}/"
fi

tar $COMPRESSION_FLAGS "$ARCHIVE_NAME" "$DIR_NAME"

# move the compiled archive into the artifacts directory to be uploaded by travis releases
mv "$ARCHIVE_NAME" "${ARTIFACTS_DIR}/"
