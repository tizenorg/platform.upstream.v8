Name:          v8
Summary:       This is v8 library.
Version:       4.7.83
Release:       0
Group:         Web Framework/Web Engine
License:       LGPL-2.1 or BSD-2-Clause
Source0:       %{name}-%{version}.tar.gz
Source1:       v8.manifest

# Conditions for OBS build
# The OBS build does not support running script 'build_{target}.sh'.
# TODO: There is a bug regarding mismatched versions from repository.
#       So, the versions need to be considered as originally intended versions,
#       until those versions are fixed by platform team.
#       1) The value '2.3' of macro 'tizen' should be '2.4'.
#       2) The value '2.0' of macro 'tizen' should be '2.3'.
%if "%{tizen}" == "3.0"
%define chromium_efl_tizen_version 3.0
%endif
%if "%{tizen}" == "2.3" || "%{tizen}" == "2.4"
%define chromium_efl_tizen_version 2.4
%endif
%if "%{tizen}" == "2.0"
%define chromium_efl_tizen_version 2.3
%endif

%if %{!?TIZEN_PROFILE_TV:0}%{?TIZEN_PROFILE_TV:1} || "%{?profile}" == "tv"
%define chromium_efl_tizen_profile tv
%endif
%if "%{!?profile:0}%{?profile}" == "mobile"
%define chromium_efl_tizen_profile mobile
%endif
%if "%{!?profile:0}%{?profile}" == "wearable"
%define chromium_efl_tizen_profile wearable
%endif
%if "%{!?profile:0}%{?profile}" == "common"
%define chromium_efl_tizen_profile common
%endif
%if "%{!?profile:0}%{?profile}" == "ivi"
%define chromium_efl_tizen_profile ivi
%endif

BuildRequires: python, python-xml
%ifarch armv7l
BuildRequires: python-accel-armv7l-cross-arm
%endif
%ifarch aarch64
BuildRequires: python-accel-aarch64-cross-aarch64
# TODO(youngsoo): The binutils-gold crashes mini_browser on the Tizen v3.0 ARM 64 bit images.
#                 Once fixed, use binutils-gold for all targets on Tizen v3.0.
%if "%{chromium_efl_tizen_version}" == "3.0"
BuildRequires: binutils-gold
%endif
%endif

BuildRequires: pkgconfig(icu-i18n)

Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
This is v8 library.

%package devel
Summary:  v8 library (Development)
Group:    System/Development
Requires: %{name} = %{version}-%{release}

%description devel
This is v8 library.
%devel_desc

%prep
%setup -q

%build
if [ ! -d %{buildroot}/../../OTHER/ -a -f /opt/testing/bin/rpmlint ]; then
  mkdir -p %{buildroot}/../../OTHER/
fi

%ifarch aarhc64
  export ADDITION_OPTION=" -finline-limit=64 -foptimize-sibling-calls -fno-unwind-tables -fno-exceptions -Os"
%endif
%ifarch %{arm}
  export ADDITION_OPTION=" -finline-limit=64 -foptimize-sibling-calls -fno-unwind-tables -fno-exceptions -Os -mthumb"
%endif
%ifarch %{arm} aarch64
  export CFLAGS="$CFLAGS $ADDITION_OPTION"
  export CXXFLAGS="$CXXFLAGS $ADDITION_OPTION"
  export FFLAGS="$FFLAGS $ADDITION_OPTION"

  export CFLAGS="$(echo $CFLAGS | sed 's/-mfpu=[a-zA-Z0-9-]*/-mfpu=neon/g')"
  export CXXFLAGS="$(echo $CXXFLAGS | sed 's/-mfpu=[a-zA-Z0-9-]*/-mfpu=neon/g')"
  export FFLAGS="$(echo $FFLAGS | sed 's/-mfpu=[a-zA-Z0-9-]*/-mfpu=neon/g')"
%else
  export CFLAGS="$(echo $CFLAGS | sed 's/-Wl,--as-needed//g')"
  export CXXFLAGS="$(echo $CXXFLAGS | sed 's/-Wl,--as-needed//g')"
%endif

export GYP_GENERATORS=ninja

%define OUTPUT_BASE_FOLDER out.tz_v%{chromium_efl_tizen_version}.%{chromium_efl_tizen_profile}.%{_repository}
export GYP_GENERATOR_OUTPUT=$PWD/%{OUTPUT_BASE_FOLDER}
export GYP_GENERATOR_FLAGS="output_dir=${GYP_GENERATOR_OUTPUT}"

#set build mode
%if 0%{?_debug_mode}
%global OUTPUT_FOLDER %{OUTPUT_BASE_FOLDER}/Debug
%else
%global OUTPUT_FOLDER %{OUTPUT_BASE_FOLDER}/Release
%endif

./build/gyp_v8.sh \
%if %{!?_enable_test:0}%{?_enable_test:1}
    -Denable_test=1
%else
    -Denable_test=0
%endif

%ifarch %{arm}
./build/prebuild/ninja.arm %{?_smp_mflags} -C%{OUTPUT_FOLDER}
%else
%ifarch aarch64
./build/prebuild/ninja-linux64 %{?_smp_mflags} -C%{OUTPUT_FOLDER}
%else
./build/prebuild/ninja %{?_smp_mflags} -C%{OUTPUT_FOLDER}
%endif
%endif

%install
install -d %{buildroot}%{_libdir}/pkgconfig
install -d %{buildroot}%{_libdir}/v8
install -d %{buildroot}%{_includedir}/v8/include
install -d %{buildroot}%{_includedir}/v8/include/libplatform
install -m 0755 ./out/Release/*.a %{buildroot}%{_libdir}/v8
install -m 0755 ./out/Release/natives_blob.bin %{buildroot}%{_libdir}/v8
install -m 0755 ./out/Release/snapshot_blob.bin %{buildroot}%{_libdir}/v8
install -m 0644 ./packaging/v8.pc %{buildroot}%{_libdir}/pkgconfig/
install -m 0644 ./include/*.h %{buildroot}%{_includedir}/v8/include
install -m 0644 ./include/libplatform/*.h %{buildroot}%{_includedir}/v8/include/libplatform

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%manifest packaging/v8.manifest
%{_libdir}/v8/libv8*.a
%{_libdir}/v8/natives_blob.bin
%{_libdir}/v8/snapshot_blob.bin

%files devel
%manifest packaging/v8.manifest
%{_libdir}/v8/libv8*.a
%{_libdir}/v8/natives_blob.bin
%{_libdir}/v8/snapshot_blob.bin
%{_libdir}/pkgconfig/v8.pc
%{_includedir}/v8/include
