#!/bin/bash

export SCRIPTDIR=$(readlink -e $(dirname $0))
export TOPDIR=$(readlink -f "${SCRIPTDIR}/..")

trap 'error_report $0 $LINENO' ERR SIGINT SIGTERM SIGQUIT

function error_report() {
  echo "Error: File:$1 Line:$2"
  exit 1
}

function getHostArch() {
  echo $(uname -m | sed -e \
      's/i.86/ia32/;s/x86_64/x64/;s/amd64/x64/;s/arm.*/arm/;s/i86pc/ia32/;s/aarch64/arm64/')
}

function setupAndExecuteTargetBuild() {
  local platform="$1"
  shift

  local PROFILE
  local ARCHITECTURE
  local CONF_FLAG
  local -a ARGS

  # "|| :" means "or always succeeding built-in command"
  PROFILE=$(echo "$@" | grep -Po "(?<=\-P\s)[^\s]*" || :)
  ARCHITECTURE=$(echo "$@" | grep -Po "(?<=\-A\s)[^\s]*" || :)

  if [ "$PROFILE" == "" ]; then
    if [[ $platform == "mobile" ]]; then
      PROFILE=tzmb_v3.0_arm64-wayland
    elif [[ $platform == "tv" ]]; then
      PROFILE=tztv_v3.0_arm-wayland
    elif [[ $platform == "wearable" ]]; then
      PROFILE=tzwr_v3.0_arm-wayland
    elif [[ $platform == "ivi" ]]; then
      PROFILE=tzivi_v3.0_arm
    elif [[ $platform == "common" ]]; then
      PROFILE=tzcommon_v3.0_arm-wayland
    else
      echo "Cannot set default PROFILE for platform=${platform}"
      exit 1
    fi
  fi
  echo "Set the profile : $PROFILE"

  if [ "$ARCHITECTURE" == "" ]; then
    if [[ $platform == "mobile" ]]; then
      ARCHITECTURE=aarch64
    elif [[ $platform == "tv" ]]; then
      ARCHITECTURE=armv7l
    elif [[ $platform == "wearable" ]]; then
      ARCHITECTURE=armv7l
    elif [[ $platform == "ivi" ]]; then
      ARCHITECTURE=armv7l
    elif [[ $platform == "common" ]]; then
      ARCHITECTURE=armv7l
    else
      echo "Cannot set default ARCHITECTURE for platform=${platform}"
      exit 1
    fi
  fi
  echo "Set the architecture : $ARCHITECTURE"

  if [ "$USE_GLOBAL_GBS_CONF" == "" ]; then
    CONF_FLAG="--conf ${SCRIPTDIR}/gbs.conf"
  fi

  local count=0
  while [[ $# > 0 ]]; do
    count=$(( $count + 1 ))
    case "$1" in
    --test)
        ARGS[$count]=--define
        count=$(( $count + 1 ))
        ARGS[$count]="_enable_test 1"
    ;;
    *)
      ARGS[$count]="$1"
    ;;
    esac
    shift;
  done

  cd $TOPDIR

  # 500 error can be occurred due to server side issue, then build needs to be run again.
  gbsBuildWithout500Error
}

function gbsBuildWithout500Error() {
  # NETSTED while statement does not allow to share variable with outer scope.
  # So, temporal file is used to share build status between outer and inner while scope.
  while
    echo "0" > $SCRIPTDIR/build_result.txt
    gbs $CONF_FLAG build -P $PROFILE --spec v8.spec --include-all -A $ARCHITECTURE "${ARGS[@]}" $BUILD_CONF_OPTS --incremental | \
    while read LINE; do
      echo -e "$LINE"
      if [ "`echo "$LINE" | grep "read timeout"`" != "" ]; then
        echo "500" > $SCRIPTDIR/build_result.txt
      fi
      if [ "`echo "$LINE" | grep "build failed"`" != "" ]; then
        echo "1" > $SCRIPTDIR/build_result.txt
      fi
    done
  [ `cat $SCRIPTDIR/build_result.txt` == "500" ]
  do :; done
  if [ `cat $SCRIPTDIR/build_result.txt` == "1" ]; then
    rm -f $SCRIPTDIR/build_result.txt
    echo "ERROR: build failed !!"
    exit 1
  fi
  rm -f $SCRIPTDIR/build_result.txt
}
