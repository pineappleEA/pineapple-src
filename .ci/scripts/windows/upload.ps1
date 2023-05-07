# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

param($BUILD_NAME)

$GITDATE = $(git show -s --date=short --format='%ad') -replace "-", ""
$GITREV = $(git show -s --format='%h')

if ("$BUILD_NAME" -eq "mainline") {
    $RELEASE_DIST = "yuzu-windows-msvc"
}
else {
    $RELEASE_DIST = "yuzu-windows-msvc-$BUILD_NAME"
}

$MSVC_BUILD_ZIP = "yuzu-windows-msvc-$GITDATE-$GITREV.zip" -replace " ", ""
$MSVC_BUILD_PDB = "yuzu-windows-msvc-$GITDATE-$GITREV-debugsymbols.zip" -replace " ", ""
$MSVC_SEVENZIP = "yuzu-windows-msvc-$GITDATE-$GITREV.7z" -replace " ", ""
$MSVC_TAR = "yuzu-windows-msvc-$GITDATE-$GITREV.tar" -replace " ", ""
$MSVC_TARXZ = "yuzu-windows-msvc-$GITDATE-$GITREV.tar.xz" -replace " ", ""
$MSVC_SOURCE = "yuzu-windows-msvc-source-$GITDATE-$GITREV" -replace " ", ""
$MSVC_SOURCE_TAR = "$MSVC_SOURCE.tar"
$MSVC_SOURCE_TARXZ = "$MSVC_SOURCE_TAR.xz"

$env:BUILD_ZIP = $MSVC_BUILD_ZIP
$env:BUILD_SYMBOLS = $MSVC_BUILD_PDB
$env:BUILD_UPDATE = $MSVC_SEVENZIP

$BUILD_DIR = ".\build\bin\Release"

# Cleanup unneeded data in submodules
git submodule foreach git clean -fxd

# Upload debugging symbols
mkdir pdb
Get-ChildItem "$BUILD_DIR\" -Recurse -Filter "*.pdb" | Copy-Item -destination .\pdb
7z a -tzip $MSVC_BUILD_PDB .\pdb\*.pdb
rm "$BUILD_DIR\*.pdb"

# Create artifact directories
mkdir $RELEASE_DIST
mkdir $MSVC_SOURCE
mkdir "artifacts"

$CURRENT_DIR = Convert-Path .

# Build a tar.xz for the source of the release
git clone --depth 1 file://$CURRENT_DIR $MSVC_SOURCE
7z a -r -ttar $MSVC_SOURCE_TAR $MSVC_SOURCE
7z a -r -txz $MSVC_SOURCE_TARXZ $MSVC_SOURCE_TAR

# Following section is quick hack to package artifacts differently for GitHub Actions
if ("$env:GITHUB_ACTIONS" -eq "true") {
    echo "Hello GitHub Actions"

    # With vcpkg we now have a few more dll files
    ls .\build\bin\*.dll
    cp .\build\bin\*.dll .\artifacts\

    # Hopefully there is an exe in either .\build\bin or .\build\bin\Release
    cp .\build\bin\yuzu*.exe .\artifacts\
    Copy-Item "$BUILD_DIR\*" -Destination "artifacts" -Recurse
    Remove-Item .\artifacts\tests.exe -ErrorAction ignore

    # None of the other GHA builds are including source, so commenting out today
    #Copy-Item $MSVC_SOURCE_TARXZ -Destination "artifacts"

    # Debugging symbols
    cp .\build\bin\yuzu*.pdb .\artifacts\

    # Write out a tag BUILD_TAG to environment for the Upload step
    # We're getting ${{ github.event.number }} as $env:PR_NUMBER"
    echo "env:PR_NUMBER: $env:PR_NUMBER"
    if (Test-Path env:PR_NUMBER) {
        $PR_NUMBER = $env:PR_NUMBER.Substring(2) -as [int]
        $PR_NUMBER_TAG = "pr"+([string]$PR_NUMBER).PadLeft(5,'0')
        if ($PR_NUMBER -gt 1){
            $BUILD_TAG="verify-$PR_NUMBER_TAG-$GITDATE-$GITREV"
        } else {
            $BUILD_TAG = "verify-$GITDATE-$GITREV"
        }
    } else {
        # If env:PR_NUMBER isn't set, we should still write out a variable
        $BUILD_TAG = "verify-$GITDATE-$GITREV"
    }
    echo "BUILD_TAG=$BUILD_TAG"
    echo "BUILD_TAG=$BUILD_TAG" >> $env:GITHUB_ENV

    # For extra job, just the exe
    $INDIVIDUAL_EXE = "yuzu-msvc-$BUILD_TAG.exe"
    echo "INDIVIDUAL_EXE=$INDIVIDUAL_EXE"
    echo "INDIVIDUAL_EXE=$INDIVIDUAL_EXE" >> $env:GITHUB_ENV
    echo "Just the exe: $INDIVIDUAL_EXE"
    cp .\artifacts\yuzu.exe .\$INDIVIDUAL_EXE


} else {

# Build the final release artifacts
Copy-Item $MSVC_SOURCE_TARXZ -Destination $RELEASE_DIST
Copy-Item "$BUILD_DIR\*" -Destination $RELEASE_DIST -Recurse
rm "$RELEASE_DIST\*.exe"
Get-ChildItem "$BUILD_DIR" -Recurse -Filter "yuzu*.exe" | Copy-Item -destination $RELEASE_DIST
Get-ChildItem "$BUILD_DIR" -Recurse -Filter "QtWebEngineProcess*.exe" | Copy-Item -destination $RELEASE_DIST
7z a -tzip $MSVC_BUILD_ZIP $RELEASE_DIST\*
7z a $MSVC_SEVENZIP $RELEASE_DIST

7z a -r -ttar $MSVC_TAR $RELEASE_DIST
7z a -r -txz $MSVC_TARXZ $MSVC_TAR

Get-ChildItem . -Filter "*.zip" | Copy-Item -destination "artifacts"
Get-ChildItem . -Filter "*.7z" | Copy-Item -destination "artifacts"
Get-ChildItem . -Filter "*.tar.xz" | Copy-Item -destination "artifacts"
}
