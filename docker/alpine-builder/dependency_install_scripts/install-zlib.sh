#!/bin/bash
#usage install-zlib.sh <directory> <version> <url> <parallelism>

set -exo pipefail

DEPENDENCIES_DIR=$1
VERSION=$2
DEPENDENCIES_URL=$3
MAKE_JOBS=$4

cd $DEPENDENCIES_DIR
wget $DEPENDENCIES_URL/zlib-$VERSION.tar.gz
tar -xzf zlib-$VERSION.tar.gz
cd zlib-$VERSION
./configure
make -j $MAKE_JOBS