#!/bin/bash

function download_third_party_modules() {
  git config --global http.sslVerify false
  git submodule update --init
  pushd build/gyp; git reset --hard 08429da7; popd;
}

download_third_party_modules
