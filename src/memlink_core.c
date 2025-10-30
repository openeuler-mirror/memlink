/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * memlink licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details. 
 * Description: memlinkd core function
 * Author: Liang Zhang
 * Create: 2025-03-30
 */

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "libvirt_helper.h"
#include "util.h"
#include "memlink_core.h"

static MemlinkdThread g_domainEventThread;
static MemlinkdThread g_balloonVmMemThread;

STATIC int CreateEventThread(void)
{
    MemSemaphore readySem;
    int rc = memset_s(&readySem, sizeof(readySem), 0, sizeof(readySem));
    if (rc != 0) {
        MEM_ERROR("Fail to do memset");
        return -1;
    }

    if (SemInit(&readySem, 0) != 0) {
        MEM_ERROR("SemInit failed, and memlinkd exit, %s", strerror(errno));
        return -1;
    }

    if (ThreadCreate(&g_domainEventThread, "event_listen", DomainEventProcessThread,
                     &readySem, MEMLINK_THREAD_DETACHED) < 0) {
        (void)SemDestroy(&readySem);
        return -1;
    }

    /* if event thread can't post signal in init_sem_wait_timeout, we think some errors happen */
    if (SemTimeWait(&readySem, g_initSemWaitTimeout) < 0) {
        MEM_ERROR("event thread has some errors, memlinkd exit");
        g_stopMemlinkd = 1;
        SMP_WMB();
        (void)SemDestroy(&readySem);
        return -1;
    }
    (void)SemDestroy(&readySem);
    return 0;
}

#define UPPER_LIMIT         50
#define LOWER_LIMIT         30
#define LOW_BALLOON_SIZE_KB (300 * 1024)
#define BALLON_TARGET_RESERVED_KBYTES (1024 * 1024)
#define BALLOON_BACK_RESERVED_SIZE_KB (2 * 1024)

STATIC void RecordPressure(VirtualMachineInfoPtr vminfo)
{
#define PERCENT 100
    uint64_t guestUsedMemory;
    uint64_t guestTotalMemory;
    float guestUsagePercent;

    if (CheckVmMemoryStat(vminfo) < 0) {
        return;
    }

    guestTotalMemory = vminfo->memstat.available;
    /* if 'usable' is invalid, use 'unused' instead
       memstat.available come from MemTotal in Guest's /proc/meminfo
       memstat.usable come from MemAvailable in Guest's /proc/meminfo
       memstat.unused come from MemFree in Guest's /proc/meminfo
    */
    if (vminfo->memstat.usable != 0) {
        guestUsedMemory = guestTotalMemory - vminfo->memstat.usable;
    } else {
        guestUsedMemory = guestTotalMemory - vminfo->memstat.unused;
    }
    guestUsagePercent = ((float)guestUsedMemory / guestTotalMemory) * PERCENT;

    if ((vminfo->needBalloonFlag == 0) && !vminfo->ballooning) {
        if (guestUsagePercent > UPPER_LIMIT) {
            vminfo->needBalloonFlag = 1;
        }
    }
}

#define NEED_BALLOON_CHECK_TIMES 20

STATIC void DoBallooning(VirtualMachineInfoPtr vminfo)
{
    uint64_t guestTotalMemory;
    uint64_t guestUsedMemory;
    float guestUsagePercent;

    if (CheckVmMemoryStat(vminfo) < 0) {
        return;
    }

    /* if 'usable' is invalid, use 'unused' instead
       memstat.available come from MemTotal in Guest's /proc/meminfo
       memstat.usable come from MemAvailable in Guest's /proc/meminfo
       memstat.unused come from MemFree in Guest's /proc/meminfo
    */
    guestTotalMemory = vminfo->memstat.available;
    if (vminfo->memstat.usable != 0) {
        guestUsedMemory = guestTotalMemory - vminfo->memstat.usable;
    } else {
        guestUsedMemory = guestTotalMemory - vminfo->memstat.unused;
    }
    guestUsagePercent = ((float)guestUsedMemory / guestTotalMemory) * PERCENT;

    /* check whether need to free memory through balloon */
    if (guestUsagePercent > UPPER_LIMIT) {
        vminfo->needBalloonFlag = 1;
    } else if (guestUsagePercent < LOWER_LIMIT && vminfo->needBalloonFlag > 0) {
        vminfo->needBalloonFlag++;
    }

    /* need to free memory through balloon */
    /* when loop N times, memory keep less than LOWER_LIMIT in N*interval s */
    if (vminfo->needBalloonFlag >= NEED_BALLOON_CHECK_TIMES) {
        vminfo->needBalloonFlag = 0;

        /* we cannot balloon vm's memory too little, or exactly the target */
        /* guest kernel may need reserved a bit memmory, also shock need take into consideration */
        /* 1.5 times may make sense */
        /* 50% may be the max balloon target based on vmware whitepaper */
        vminfo->balloonTarget = (vminfo->memory - guestTotalMemory) + guestUsedMemory;

        vminfo->balloonTarget = MAX((int64_t)(vminfo->balloonTarget * g_balloonTargetUsedPercent / PERCENT),
                                    BALLON_TARGET_RESERVED_KBYTES);
        vminfo->balloonTarget = MAX(vminfo->balloonTarget,
                                    (int64_t)(vminfo->memory * g_balloonTargetMaxTotalPercent / PERCENT));
        vminfo->balloonTarget = MAX(vminfo->balloonTarget, (int64_t)vminfo->memparam.minGuarantee);
        vminfo->balloonTarget = MIN(vminfo->balloonTarget, (int64_t)vminfo->memory);

        if (vminfo->balloonTarget > 0 && SetVmBalloonTarget(vminfo, (uint64_t)(vminfo->balloonTarget)) == 0) {
            vminfo->ballooning = true;
            vminfo->ballooningDown = true;
            MEM_INFO("[%s] ballooning to target: %lluKB", vminfo->domname, vminfo->balloonTarget);
        }
    }
}

#define BALLOONBACK_LEFT (-2)

STATIC int BalloonCheck(VirtualMachineInfoPtr vminfo)
{
#define LOW_WATER_MARK 1024
#define INDEX_MASK 2
    if (vminfo->incompleteBalloon) {
        MEM_INFO("[%s] balloon is incomplete, should balloon back", vminfo->domname);
        return -1;
    }

    if (vminfo->ballooningDown) {
        /* condition 1: balloon done when less than 1MB error range */
        if (vminfo->memstat.actualBalloon - vminfo->balloonTarget < LOW_WATER_MARK) {
            MEM_INFO("[%s] ballooning to target: %lluKB done", vminfo->domname, vminfo->balloonTarget);
            return -1;
        }
        /* condition 2: stop balloon */
        vminfo->currentActualBalloon[vminfo->balloonI] = vminfo->memstat.actualBalloon;
        vminfo->balloonI = (vminfo->balloonI + 1) % INDEX_MASK; /* compute ballon index, 0 or 1 alternately */
        if (labs(vminfo->currentActualBalloon[0] - vminfo->currentActualBalloon[1]) < LOW_BALLOON_SIZE_KB) {
            MEM_INFO("[%s] stop balloon, due to too slow balloon speed", vminfo->domname);
            /* If balloon is stopped in the midway, which means inflating job of balloon has not been finished.
             * Once the high memory pressure (memory usage is more than 50%) do not come, the free memory in guest
             * will never be released by host. Thus, making needBalloonFlag being one makes the inflating job restart
             * again.
             */
            vminfo->needBalloonFlag = 1;
            return -1;
        }
    } else if (vminfo->memory - vminfo->memstat.actualBalloon < LOW_WATER_MARK) {
        /* check whether balloon back done when less than 1MB range */
        MEM_INFO("[%s] ballooning back to vm memory: %lluKB done", vminfo->domname, vminfo->memory);
        vminfo->ballooning = false;
    } else if (vminfo->memory - vminfo->memstat.actualBalloon <= BALLOON_BACK_RESERVED_SIZE_KB) {
        MEM_INFO("[%s] actualBalloon : %lldKB", vminfo->domname, vminfo->memstat.actualBalloon);
        return BALLOONBACK_LEFT;
    }

    return 0;
}

static void BalloonMainLoop(void)
{
    VirtualMachineInfoPtr vminfo = NULL;
    uint64_t ballonBackTarget = 0;
    int ret;

    /* The Main Procedure */
    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        /* by pass these special VM that could not be balloon */
        if (vminfo->lockall || vminfo->enable_fpr) {
            continue;
        }

        if (!vminfo->ballooning) {
            if (vminfo->state == (int16_t)VIR_DOMAIN_RUNNING) {
                DoBallooning(vminfo);
            }
            continue;
        }
        /* check whether balloon done */
        if (vminfo->memstat.actualBalloon == 0) {
            continue;
        }

        /* check whether balloon done or need to stop balloon if balloon speed is too slow */
        ret = BalloonCheck(vminfo);
        /* if leak of balloon happened at a moment when vm is rebooting,
           it maybe cause a problem that balloon is terminated and vm's currentMemory is not correct.
           so, take two steps to balloon back to memory:
           firstly, balloon back to (memory - 2M) to avoid dev->num_pages equal 0(in virtio_balloon.c of kernel),
           thus, leak of balloon will continue after reboot;
           secondly, balloon back to memory.
        */
        int setBalloonRet;
        if (ret == -1) {
            ballonBackTarget = (uint64_t)(vminfo->memory >= 0 ? vminfo->memory : BALLON_TARGET_RESERVED_KBYTES);
            ballonBackTarget -= BALLOON_BACK_RESERVED_SIZE_KB;
            setBalloonRet = SetVmBalloonTarget(vminfo, ballonBackTarget);
            /* If the VM is failed to balloon back, should try again later */
            if (setBalloonRet < 0) {
                MEM_WARN("step1:failed to set vm [%s] balloon back!", vminfo->domname);
                vminfo->incompleteBalloon = true;
                continue;
            }
            MEM_INFO("step1:[%s] ballooning back to vm memory %lluKB", vminfo->domname, ballonBackTarget);
            vminfo->ballooningDown = false;
            vminfo->balloonI = 0;
            vminfo->currentActualBalloon[0] = vminfo->currentActualBalloon[1] = 0;
            if (vminfo->incompleteBalloon) {
                vminfo->incompleteBalloon = false;
            }
        } else if (ret == BALLOONBACK_LEFT) {
            ballonBackTarget = (uint64_t)(vminfo->memory >= 0 ? vminfo->memory : BALLON_TARGET_RESERVED_KBYTES);
            setBalloonRet = SetVmBalloonTarget(vminfo, ballonBackTarget);
            if (setBalloonRet < 0) {
                MEM_WARN("step2:failed to set vm [%s] balloon back!", vminfo->domname);
            } else {
                MEM_INFO("step2:[%s] ballooning back to vm memory %lluKB", vminfo->domname, ballonBackTarget);
            }
        }
    }
}

static void UpdateAllVmInfoAndMemoryStats(void)
{
    MutexLock(&g_vmlistHead.lock);

    GetAllDomainsStats();

    MutexUnlock(&g_vmlistHead.lock);
}

STATIC void *BalloonVmMemThread(void *data ATTRIBUTE_UNUSED)
{
    VirtualMachineInfoPtr vminfo = NULL;

    while (g_stopMemlinkd == 0) {

        UpdateAllVmInfoAndMemoryStats();

        /* we need not balloon mem when host memory is not very low, so check here! */
        MutexLock(&g_vmlistHead.lock);

        /* firstly, update VMs information */
        LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
            if (CheckVmMemoryStat(vminfo) < 0) {
                continue;
            };
            /* if vm is snapshotted when it's in ballooning, it can't balloon back after snapshot restore,
             * which causes currentMemory lower than memory.
             * To avoid it, we should make this vm to balloon back.
             */
            if ((!vminfo->ballooning) && (vminfo->memory > vminfo->currentMem)) {
                vminfo->ballooning = true;
                vminfo->incompleteBalloon = true;
            }
        }

        /* check are there some VMs are in half of ballooning */
        LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
            if (vminfo->ballooning) {
                break;
            }
        }

        /* record vm has ever been in pressure */
        LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
            if (vminfo->state == (int16_t)VIR_DOMAIN_RUNNING) {
                RecordPressure(vminfo);
            }
        }

        BalloonMainLoop();

        MutexUnlock(&g_vmlistHead.lock);
        /* Sleep 2 seconds between every two loop */
        SleepApprox(2);
    }
    return NULL;
}

/**
 * memlinkd core functions
 */
int CoreInit(void)
{
    /* Create a thread to listen libvirt event */
    if (CreateEventThread() < 0) {
        return -1;
    }

    /* Create a thread to periodly balloon free VM memory back */
    if (g_balloonEnable == 1) {
        if (ThreadCreate(&g_balloonVmMemThread, "balloon_vm_mem",
                         BalloonVmMemThread,
                         NULL, MEMLINK_THREAD_DETACHED) < 0) {
            return -1;
        }
    }

    return 0;
}

#define SET_BALLOON_MAX 10

STATIC void BalloonBackVmMem(void)
{
    VirtualMachineInfoPtr vminfo = NULL;
    uint64_t ballonTarget = 0;

    MutexLock(&g_vmlistHead.lock);
    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        /* if vm is ballooning when stop memlinkd service, should balloon back */
        if (vminfo->ballooning) {
            ballonTarget = (uint64_t)(vminfo->memory >= 0 ? vminfo->memory : BALLON_TARGET_RESERVED_KBYTES);
            for (int i = 0; i < SET_BALLOON_MAX; i++) {
                if (SetVmBalloonTarget(vminfo, ballonTarget) == 0) {
                    MEM_INFO("[%s] ballooning back to vm memory %lluKB before exit", vminfo->domname, ballonTarget);
                    break;
                } else {
                    MEM_WARN("[%s] failed to balloon back before exit!", vminfo->domname);
                }
            }
        }
        /* also, stop updating the memory statistics */
        SetVmMemoryStatsPeriod(vminfo, false);
    }
    MutexUnlock(&g_vmlistHead.lock);
}

void CoreExit(void)
{
    if (g_balloonEnable == 1) {
        BalloonBackVmMem();
    }

    /* release all tmp resources */
    FreeAllVM();
}
