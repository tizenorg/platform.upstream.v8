#!/bin/bash

. `dirname $0`/common.sh
trap 'exit 1' ERR SIGINT SIGTERM SIGQUIT

setupAndExecuteTargetBuild tv "$@"
