%{!?configopts: %global configopts %{nil}}

Name: libfabric
Version: 1.4.1
Release: 1%{?dist}
Summary: User-space RDMA Fabric Interfaces

Group: System Environment/Libraries
License: GPLv2 or BSD
Url: http://www.github.com/ofiwg/libfabric
Source: http://www.github.org/ofiwg/%{name}/releases/download/v{%version}/%{name}-%{version}.tar.bz2
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

%description
libfabric provides a user-space API to access high-performance fabric
services, such as RDMA.

%package devel
Summary: Development files for the libfabric library
Group: System Environment/Libraries
Requires: libfabric = %{version}

%description devel
Development files for the libfabric library.

%prep
%setup -q -n %{name}-%{version}

%build
# defaults: with-dlopen and without-valgrind can be over-rode:
%configure %{?_without_dlopen} %{?_with_valgrind} %{configopts}
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%makeinstall installdirs
# remove unpackaged files from the buildroot
rm -f %{buildroot}%{_libdir}/*.la

%clean
rm -rf %{buildroot}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/lib*.so.*
%{_bindir}/fi_info
%{_bindir}/fi_strerror
%{_bindir}/fi_pingpong
%dir %{_libdir}/libfabric/
%doc AUTHORS COPYING README

%files devel
%defattr(-,root,root)
%{_libdir}/libfabric*.so
%{_libdir}/*.a
%{_libdir}/pkgconfig/libfabric.pc
%{_includedir}/*
%{_mandir}/man1/*
%{_mandir}/man3/*
%{_mandir}/man7/*

%changelog
* Fri Jun 26 2015 Open Fabrics Interfaces Working Group <ofiwg@lists.openfabrics.org> 1.1.0rc1
- Release 1.1.0rc1
* Sun May 3 2015 Open Fabrics Interfaces Working Group <ofiwg@lists.openfabrics.org> 1.0.0
- Release 1.0.0
