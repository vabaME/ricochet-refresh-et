#!/bin/bash

set -ex

ROOT_LIB=$(pwd)/lib
OPENSSLDIR="${OPENSSLDIR:-${ROOT_LIB}/openssl/}"

pushd "../tests"
  export PKG_CONFIG_PATH=${ROOT_LIB}/protobuf/lib/pkgconfig:${PKG_CONFIG_PATH}
  export PATH=${ROOT_LIB}/protobuf/bin/:${PATH}

  qmake tests.pro CONFIG+=x86_64 CONFIG+=qtquickcompiler OPENSSLDIR=$OPENSSLDIR && make qmake_all && make
  TEST_COMMAND=$'find . -type f -regextype sed -regex "./.*\(test_\|tst_\)[^/]*" -executable | while read -r test; do $test || exit $?; done'

  if [ -n "$HEADLESS" ]; then
    Xvfb :1 -screen 0 800x600x16 &
    xvfb-run "$TEST_COMMAND"
  else
    "$TEST_COMMAND"
  fi
popd
