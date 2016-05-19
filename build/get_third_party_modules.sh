#!/bin/bash

function set_environment() {
  if [ "$GLOBAL_HTTP_PROXY" == "" ] ; then
    echo "** Setup default proxy"
    export http_proxy="http://10.112.1.184:8080"
    export https_proxy="https://10.112.1.184:8080"
  else
    echo "** Setup global proxy"
    export http_proxy="$GLOBAL_HTTP_PROXY"
    export https_proxy="$GLOBAL_HTTPS_PROXY"
  fi
}

function download_third_party_modules() {
  git config --global http.sslVerify false
  git submodule update --init
  pushd build/gyp; git reset --hard 08429da7; popd;
}

set_environment
download_third_party_modules
