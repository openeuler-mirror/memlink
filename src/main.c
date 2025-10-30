/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 * memlink licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details. 
 * Description: memlinkd main
 * Author: Liang Zhang
 * Create: 2024-09-30
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "util.h"
#include "log.h"
#include "libvirt_helper.h"
#include "memlink_core.h"
#include "memlink_driver.h"

int g_stopMemlinkd = 0;

static int Daemonize(void)
{
#define UMASK 027
    int fd = -1;
    int len;
    char buf[64] = { 0 }; /* record pid, 64 len is enough */
    int ret = -2;

    /* start daemon */
    if (daemon(0, 1) < 0) {
        MEM_ERROR("Can't init daemonize, %s", strerror(errno));
        return ret;
    }

    /* create pid file */
    (void)umask(UMASK);
    fd = open(MEMLINKD_PID_FILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        MEM_ERROR("Can't open %s, %s", MEMLINKD_PID_FILE, strerror(errno));
        return ret;
    }

    /* check if already running */
    if (lockf(fd, F_TLOCK, 0) < 0) {
        MEM_ERROR("Can't lock %s, %s", MEMLINKD_PID_FILE, strerror(errno));
        close(fd);
        return ret;
    }

    if (ftruncate(fd, 0) < 0) {
        MEM_ERROR("Failed to clear pid file '%s': %s",
                  MEMLINKD_PID_FILE, strerror(errno));
        close(fd);
        return -1;
    }

    len = snprintf_s(buf, sizeof(buf), sizeof(buf) - 1, "%ld", (long)getpid());
    if (len <= 0) {
        MEM_ERROR("Failed to format string");
        close(fd);
        return -1;
    }
    if (write(fd, buf, (size_t)len) < 0) {
        MEM_ERROR("Can't write pid to %s, %s", MEMLINKD_PID_FILE, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void SignalHandler(int signalHandle, siginfo_t *info, void *c)
{
    g_stopMemlinkd = signalHandle;
}

static int DaemonSetupSignals(void)
{
    struct sigaction act;
    int rc;

    rc = memset_s(&act, sizeof(act), 0, sizeof(act));
    if (rc != 0) {
        MEM_ERROR("Fail to do memset");
        return -1;
    }
    act.sa_sigaction = SignalHandler;
    act.sa_flags = SA_SIGINFO;
    (void)sigaction(SIGINT, &act, NULL);
    (void)sigaction(SIGHUP, &act, NULL);
    (void)sigaction(SIGTERM, &act, NULL);
    return 0;
}

int main(int argc, char *argv[])
{
#define MS_TO_S_RATE 1000
    int ret = EXIT_FAILURE;
    int fd = -1;

    LogInit();

    fd = Daemonize();
    if (fd == -1) {
        MEM_ERROR("can not daemonize, memlinkd exit");
        goto ERROR2;
    } else if (fd < 0) {
        MEM_ERROR("memlinkd already running, so exit");
        LogExit();
        return ret;
    }

    if (DaemonSetupSignals() < 0) {
        MEM_ERROR("can not setup signals, memlinkd exit");
        goto ERROR2;
    }

    if (LoadCfgFile() < 0) {
        MEM_ERROR("can not load config file, memlinkd exit");
        goto ERROR2;
    }

    if (CoreInit() < 0) {
        MEM_ERROR("can not init memlinkd, memlinkd exit");
        goto ERROR1;
    }

    while (g_stopMemlinkd == 0) {
        if ((ATOMIC_XCHG(&g_libvirtChange, 0) != 0)) {
            UpdateAllVmInfo();
        }

        SleepApprox(g_hostInfoPollTimeMs / MS_TO_S_RATE);
    }
    ret = EXIT_SUCCESS;
    MEM_INFO("Revice signal %d, stop memlinkd", g_stopMemlinkd);

ERROR1:
    CoreExit();
ERROR2:
    g_stopMemlinkd = 1;
    LogExit();
    /* clear pid file */
    (void)unlink(MEMLINKD_PID_FILE);
    if (fd != -1) {
        close(fd);
    }

    return ret;
}

