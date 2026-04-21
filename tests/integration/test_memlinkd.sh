#!/bin/bash
# memlinkd integration test script

MEMLINKD_BIN="/usr/sbin/memlinkd"
MEMLINKD_CONF="/etc/memlinkd.conf"
MEMLINKD_SERVICE="memlinkd"

PASSED=0
FAILED=0

pass() { echo "  [PASS] $1"; ((PASSED++)); }
fail() { echo "  [FAIL] $1"; ((FAILED++)); }
info() { echo "  [INFO] $1"; }

# Test 1: Check binary
test_binary() {
    echo -e "\n[TEST] Check memlinkd binary"
    if [ -x "$MEMLINKD_BIN" ]; then
        pass "$MEMLINKD_BIN exists"
    else
        fail "$MEMLINKD_BIN not found"
    fi
}

# Test 2: Check config
test_config() {
    echo -e "\n[TEST] Check config file"
    if [ -f "$MEMLINKD_CONF" ]; then
        pass "$MEMLINKD_CONF exists"
        info "Content:"
        grep -v "^#" "$MEMLINKD_CONF" | grep -v "^$"
    else
        fail "$MEMLINKD_CONF not found"
    fi
}

# Test 3: Check libvirtd
test_libvirtd() {
    echo -e "\n[TEST] Check libvirtd"
    if systemctl is-active --quiet libvirtd 2>/dev/null; then
        pass "libvirtd is running"
    else
        fail "libvirtd is not running"
    fi
}

# Test 4: Check VM
test_vm() {
    echo -e "\n[TEST] Check running VMs"
    if ! command -v virsh &>/dev/null; then
        fail "virsh not available"; return
    fi
    local count
    count=$(virsh list --name 2>/dev/null | wc -l)
    if [ "$count" -gt 0 ]; then
        pass "Found $count running VM(s)"
        virsh list 2>/dev/null
    else
        fail "No running VMs"
    fi
}

# Test 5: Service start/stop
test_service() {
    echo -e "\n[TEST] Service start/stop"
    if [ ! -x "$MEMLINKD_BIN" ] || [ "$EUID" -ne 0 ]; then
        info "Skip (need root and memlinkd installed)"; return
    fi
    systemctl stop $MEMLINKD_SERVICE 2>/dev/null || true
    sleep 1

    systemctl start $MEMLINKD_SERVICE 2>/dev/null || { fail "Start failed"; return; }
    sleep 2

    if systemctl is-active --quiet $MEMLINKD_SERVICE 2>/dev/null; then
        pass "Service started"
    else
        fail "Service not active"; return
    fi

    systemctl stop $MEMLINKD_SERVICE 2>/dev/null || true
    sleep 1
    if ! systemctl is-active --quiet $MEMLINKD_SERVICE 2>/dev/null; then
        pass "Service stopped"
    else
        fail "Stop failed"
    fi
}

# Summary
echo "========================================"
echo "  memlinkd Integration Test"
echo "========================================"

test_binary
test_config
test_libvirtd
test_vm
test_service

echo -e "\n========================================"
echo "Passed: $PASSED  Failed: $FAILED"
echo "========================================"
