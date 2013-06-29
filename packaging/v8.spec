Name:           v8
Version:        3.14.5
Release:        0
Summary:        JavaScript Engine
License:        BSD-3-Clause
Group:          System/Libraries
Url:            http://code.google.com/p/v8
Source0:        %{name}.%{version}.tar.bz2
Source1001: 	v8.manifest
BuildRequires:  gcc-c++
BuildRequires:  lzma
BuildRequires:  python-devel
BuildRequires:  readline-devel
ExclusiveArch:  %{ix86} x86_64 %arm

%global somajor `echo %{version} | cut -f1 -d'.'`
%global sominor `echo %{version} | cut -f2 -d'.'`
%global sobuild `echo %{version} | cut -f3 -d'.'`
%global sover %{somajor}.%{sominor}.%{sobuild}

%ifarch i586 i686 
%global target ia32
%endif
%ifarch x86_64
%global target x64
%endif
%ifarch armv7l armv7hl
%global target arm
%endif

%description
V8 is Google's open source JavaScript engine. V8 is written in C++ and is used
in Google Chrome, the open source browser from Google. V8 implements ECMAScript
as specified in ECMA-262, 3rd edition.

%package -n libv8

Summary:        JavaScript Engine
Group:          Development/Libraries/Other

%description -n libv8
Libraries for v8.

%package devel

Summary:        Development headers and libraries for v8
Group:          Development/Libraries/Other
Requires:       lib%{name} = %{version}

%description devel
Development headers and libraries for v8.

%package private-headers-devel

Summary:        Private Development headers for v8
Group:          Development/Libraries/C and C++
Requires:       %{name}-devel = %{version}

%description private-headers-devel
Special Private Development headers for v8.

%prep
%setup -q
cp %{SOURCE1001} .

%build

env=CCFLAGS:"-fPIC"
make %{target}.release -j3 library=shared snapshots=on soname_version=%{somajor}

%install
mkdir -p %{buildroot}%{_includedir}/v8/x64
mkdir -p %{buildroot}%{_libdir}
install -p include/*.h %{buildroot}%{_includedir}

install -p src/*.h %{buildroot}%{_includedir}/v8
install -p src/x64/*.h %{buildroot}%{_includedir}/v8/x64

install -p out/%{target}.release/lib.target/libv8.so* %{buildroot}%{_libdir}
mkdir -p %{buildroot}%{_bindir}
install -p -m0755 out/%{target}.release/d8 %{buildroot}%{_bindir}

pushd %{buildroot}%{_libdir}
ln -sf libv8.so.%{somajor} libv8.so

chmod -x %{buildroot}%{_includedir}/v8*.h
popd

%post -n libv8 -p /sbin/ldconfig

%postun -n libv8 -p /sbin/ldconfig

%files -n libv8
%manifest %{name}.manifest
%defattr(-,root,root,-)
%license LICENSE
%{_bindir}/d8
%{_libdir}/*.so.*

%files devel
%manifest %{name}.manifest
%defattr(-,root,root,-)
%{_includedir}/*.h
%{_libdir}/*.so


%files private-headers-devel
%manifest %{name}.manifest
%defattr(644,root,root,-)
%{_includedir}/v8/

%changelog
