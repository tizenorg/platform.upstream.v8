Name:          v8
Summary:       This is v8 library.
Version:       4.7.83
Release:       0
Group:         Web Framework/Web Engine
License:       LGPL-2.1 or BSD-2-Clause
Source0:       %{name}-%{version}.tar.gz
Source1:       v8.manifest

BuildRequires: python, python-xml
%ifarch armv7l
BuildRequires: python-accel-armv7l-cross-arm
%endif
%ifarch aarch64
BuildRequires: python-accel-aarch64-cross-aarch64
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

%ifarch %{arm} aarch64
  export ADDITION_OPTION=" -finline-limit=64 -foptimize-sibling-calls -fno-unwind-tables -fno-exceptions -Os -mthumb"

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

./build/gyp_v8.sh \
%if %{!?_enable_test:0}%{?_enable_test:1}
    -Denable_test=1
%else
    -Denable_test=0
%endif
./build/prebuild/ninja.arm %{?_smp_mflags} -C out/Release

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
