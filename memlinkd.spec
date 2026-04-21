#needsrootforbuild
%global systemctl_bin /usr/bin/systemctl

Summary: UVP memory overcommit services for euleros
Name: memlinkd
Version: 1.0
Release: 209
License: MulanPSL2
ExclusiveArch:  %ix86 x86_64 aarch64
Group: System Environment/Daemons
Source0: memlinkd.tar.bz2

BuildRequires : libvirt libvirt-devel libboundscheck systemd-units
BuildRequires : gnutls gnutls-devel cmake CUnit-devel
Requires      : libvirt libboundscheck systemd-units
Requires(post): systemd-units

%description
UVP memory overcommit services for euleros

%prep
%setup -q -n src

%build
rm -rf build
mkdir -p build
cd build
%ifarch aarch64
    cmake .. -DFIT_FOR_AARCH64:STRING=TRUE
%else
    cmake ..
%endif
make clean
make VERBOSE=1

%install
mkdir -p %{buildroot}%{_sbindir}
mkdir -p %{buildroot}%{_unitdir}
mkdir -p %{buildroot}/etc/sysmonitor/process/

install -p -m 644 memlinkd.service %{buildroot}%{_unitdir}/
install -p -m 640 memlinkd.conf  %{buildroot}/etc/memlinkd.conf
install -p -m 550 ./build/memlinkd %{buildroot}/usr/sbin/
install -p -m 600 ./sysmonitor/memlinkd-daemon %{buildroot}/etc/sysmonitor/process/memlinkd-daemon

%files
%{_sbindir}/memlinkd
%{_unitdir}/memlinkd.service
%config(noreplace) /etc/memlinkd.conf
/etc/sysmonitor/process/memlinkd-daemon

%post
if [ "$1" -eq 1 ]; then
    systemctl disable memlinkd.service &> /dev/null
fi

systemctl daemon-reload &> /dev/null

%preun
if [ "$1" -eq 0 ]; then
    systemctl disable memlinkd.service &> /dev/null
    systemctl stop memlinkd.service &> /dev/null
fi

%postun
if [ "$1" -ge 1 ]; then
    systemctl try-restart memlinkd.service &> /dev/null
fi

%changelog
* Tue Apr 21 2026 Leizongkun<leizongkun@huawei.com> - 1.0-209
- Type:feature
- CVE:NA
- SUG:NA
- DESC:Add integration test suite for memlinkd

* Tue Aug 26 2025 Leizongkun<leizongkun@huawei.com> - 1.0-208
- Type:feature
- CVE:NA
- SUG:NA
- DESC:Add init code of memlink
