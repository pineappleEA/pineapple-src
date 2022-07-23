#!/bin/bash -ex

BRANCH=`echo ${GITHUB_REF##*/}`
build_date=$(date +%F -r .)

ver=$(cat /yuzu/README.md | grep -o 'early-access [[:digit:]]*' | cut -c 14-17)
title="yuzu Early Access $ver"

yuzupatch=( $(ls -d patches/* ) )
for i in "${yuzupatch[@]}"; do patch -p1 < "$i"; done

find . -name "CMakeLists.txt" ! -path "*/externals/*" -exec sed -i 's/^.*-Werror$/-W/g' {} +
find . -name "CMakeLists.txt" ! -path "*/externals/*" -exec sed -i 's/^.*-Werror=.*)$/ )/g' {} +
find . -name "CMakeLists.txt" ! -path "*/externals/*" -exec sed -i 's/^.*-Werror=.*$/ /g' {} +
find . -name "CMakeLists.txt" ! -path "*/externals/*" -exec sed -i 's/-Werror/-W/g' {} +

# Add cache if does not exist
if [[ ! -e ~/.ccache ]]; then
	mkdir ~/.ccache 
fi 
CACHEDIR=~/.ccache
ls -al $CACHEDIR
###############################################
# Install SDL
SDL2VER=2.0.22
#SDL2
cd $CACHEDIR
if [[ ! -e SDL2-${SDL2VER} ]]; then
	curl -sLO https://libsdl.org/release/SDL2-${SDL2VER}.tar.gz
	tar -xzf SDL2-${SDL2VER}.tar.gz
	cd SDL2-${SDL2VER}
	./configure --prefix=/usr
	make && cd ../
	rm SDL2-${SDL2VER}.tar.gz
fi
make -C SDL2-${SDL2VER} install
sdl2-config --version
cd /yuzu
###############################################

#pip3 install conan --upgrade --no-cache-dir
#pip3 install wheel

mkdir build && cd build 

cmake ..                                    \
  -DCMAKE_BUILD_TYPE=Release                \
  -DCMAKE_C_COMPILER=/usr/lib/ccache/gcc    \
  -DCMAKE_CXX_COMPILER=/usr/lib/ccache/g++  \
  -DTITLE_BAR_FORMAT_IDLE="$title"          \
  -DTITLE_BAR_FORMAT_RUNNING="$title | {3}" \
  -DENABLE_COMPATIBILITY_LIST_DOWNLOAD=ON   \
  -DGIT_BRANCH="HEAD"                       \
  -DGIT_DESC="$msvc"                        \
  -DUSE_DISCORD_PRESENCE=ON                 \
  -DENABLE_QT_TRANSLATION=ON                \
  -DBUILD_DATE="$build_date"                \
  -DYUZU_USE_QT_WEB_ENGINE=OFF              \
  -DYUZU_USE_EXTERNAL_SDL2=OFF 		    \
  -DYUZU_TESTS=OFF                          \ 
  -DYUZU_USE_BUNDLED_VCPKG=ON               \
  -G Ninja 

ninja


cd /tmp
curl -sLO "https://raw.githubusercontent.com/$GITHUB_REPOSITORY/$BRANCH/.github/workflows/appimage.sh"
chmod a+x appimage.sh
./appimage.sh
