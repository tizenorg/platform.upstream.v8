#!/bin/bash

function set_environment() {
  echo "* Setup HQ proxy"
  export http_proxy="http://10.112.1.184:8080"
  export https_proxy="https://10.112.1.184:8080"
}

set_environment
