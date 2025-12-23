#!/usr/bin/env bash
#
git clean -xfd
git reset --hard
#
git submodule foreach git clean -xfd
git submodule foreach git reset --hard
git submodule update --init --recursive
#
git pull
#
if [[ "$1" = "--build" ]]; then
  dpkg-buildpackage -b --no-sign
fi
