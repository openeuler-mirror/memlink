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
 * Description: libvirt helper function
 * Author: Liang Zhang
 * Create: 2024-10-23
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include "log.h"
#include "util.h"
#include "libvirt_helper.h"

#define ENABLED_FPR_FLAG 0x1

/* if LibvirtConn will be used in other thread, should apply a lock */
virConnectPtr g_libvirtConn = NULL;
int g_libvirtChange = 0;

struct DomainEventData {
    int event;
    int id;
    virConnectDomainEventGenericCallback cb;
    const char *name;
};

#define ARRAY_CARDINALITY(array)      (sizeof(array) / sizeof(*(array)))
#define DOMAIN_EVENT(event, callback) \
    {                                                          \
        event, -1, VIR_DOMAIN_EVENT_CALLBACK(callback), #event \
    }

STATIC void ConnectLibvirt(void)
{
    if (g_libvirtConn != NULL && (virConnectIsAlive(g_libvirtConn) != 1)) {
        MEM_INFO("Libvirt connect is not alive; Try to connect libvirtd again");
        (void)virConnectClose(g_libvirtConn);
        g_libvirtConn = NULL;
    }
    while (g_libvirtConn == NULL) {
        g_libvirtConn = virConnectOpen(LIBVIRT_CONN_URL);
        if (g_libvirtConn == NULL) {
            MEM_ERROR("Failed to open a connect to libvirtd!");
            SleepApprox(1);
        }
    }
    MEM_INFO("Connect to libvirtd successfully");
}

/*
 * find a domain by VM name
 */
virDomainPtr LookupDomByID(int domid)
{
    virDomainPtr dom = NULL;

    if (g_libvirtConn) {
        dom = virDomainLookupByID(g_libvirtConn, domid);
    }

    return dom;
}

bool CheckDomainIsAlive(const VirtualMachineInfoPtr vminfo)
{
    int ret;
    virErrorPtr errMsg = NULL;

    if (!vminfo) {
        return false;
    }
    ret = virDomainIsActive(vminfo->dom);
    if (ret < 0) {
        errMsg = virGetLastError();
        MEM_INFO("Failed to query domain state, errno: %d, error msg: %s",
                 errMsg ? errMsg->code : errno,
                 (errMsg && errMsg->message) ? errMsg->message : "Unkown");
        ret = 0;
    }

    return ret != 0;
}

char *GetLastErrMsg(void)
{
    virErrorPtr error = NULL;

    error = virGetLastError();
    if (error) {
        return error->message;
    }

    return NULL;
}

STATIC void CollectAddCurrentVMs(void)
{
    int ret = -1;
    int domNum = 0;
    int i = 0;
    int *domids = NULL;
    int rc;

    if (g_libvirtConn) {
        domNum = virConnectNumOfDomains(g_libvirtConn);
    }

    if (domNum < 0) {
        MEM_ERROR("Failed to get number of active domains.");
        return;
    }
    if (domNum == 0) {
        return;
    }
    domids = (int *)malloc(sizeof(int) * domNum);
    if (domids == NULL) {
        MEM_ERROR("Failed to malloc.");
        return;
    }
    rc = memset_s(domids, sizeof(int) * domNum, 0, sizeof(int) * domNum);
    if (rc != 0) {
        MEM_ERROR("Fail to set arrays");
        goto OUT;
    }

    if (g_libvirtConn) {
        ret = virConnectListDomains(g_libvirtConn, domids, domNum);
    }

    if (ret < 0) {
        MEM_ERROR("Failed to get domains from libvirtd");
        goto OUT;
    }
    for (i = 0; i < ret; i++) {
        FindAddVMToList(domids[i]);
    }

OUT:
    free(domids);
    return;
}

const char *GetDomainName(const VirtualMachineInfoPtr vminfo)
{
    const char *name = NULL;
    char *errmsg = NULL;

    name = virDomainGetName(vminfo->dom);
    if (!name) {
        errmsg = GetLastErrMsg();
        MEM_ERROR("Failed to get the name of domid %d, error reason :%s",
                  vminfo->domid, errmsg ? errmsg : "Unknown");
    }

    return name;
}

int GetVmInfo(const VirtualMachineInfoPtr vminfo)
{
    int ret;
    virDomainInfo info;
    char *errmsg = NULL;

    if (!vminfo) {
        return -1;
    }

    ret = virDomainGetInfo(vminfo->dom, &info);
    if (ret < 0) {
        errmsg = GetLastErrMsg();
        MEM_ERROR("Domain [%s] get info failed!, reason: %s",
                  vminfo->domname, errmsg ? errmsg : "Unknown");
        return ret;
    }
    vminfo->memory = (int64_t)info.maxMem;
    vminfo->currentMem = (int64_t)info.memory;
    vminfo->state = info.state;
    return 0;
}

void SetVmMemoryStatsPeriod(const VirtualMachineInfoPtr vminfo, bool updateStats)
{
#define ENABLE_STATS_UPDATE_PERIOD 2
#define DISABLE_STATS_UPDATE_PERIOD 0

    int ret;
    int period;
    char *errmsg = NULL;

    if (!vminfo) {
        return;
    }

    if (updateStats) {
        period = ENABLE_STATS_UPDATE_PERIOD;
    } else {
        period = DISABLE_STATS_UPDATE_PERIOD;
    }

    ret = virDomainSetMemoryStatsPeriod(vminfo->dom, period, VIR_DOMAIN_AFFECT_CURRENT);
    if (ret < 0) {
        errmsg = GetLastErrMsg();
        MEM_WARN("Domain [%s] set balloon stats collection interval failed!, reason: %s",
                 vminfo->domname, errmsg ? errmsg : "Unknown");
    }
}

static VirtualMachineInfoPtr GetVMByDom(virDomainPtr dom)
{
    VirtualMachineInfoPtr vminfo = NULL;

    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        if (vminfo->domid == virDomainGetID(dom)) {
            return vminfo;
        }
    }

    return NULL;
}

static void GetOneDomainStats(virDomainStatsRecordPtr record)
{
    int i;
    VirtualMachineInfoPtr vminfo = NULL;
    MEM_DEBUG("domname: %s\n", virDomainGetName(record->dom));
    vminfo = GetVMByDom(record->dom);
    if (!vminfo) {
        return;
    }

    for (i = 0; i < record->nparams; i++) {
        if (strcmp(record->params[i].field, "state.state") == 0) {
            vminfo->state = (int16_t)record->params[i].value.i;
        } else if (strcmp(record->params[i].field, "balloon.current") == 0) {
            vminfo->currentMem = (int64_t)record->params[i].value.ul;
            vminfo->memstat.actualBalloon = (uint64_t)record->params[i].value.ul;
        } else if (strcmp(record->params[i].field, "balloon.maximum") == 0) {
            vminfo->memory = (int64_t)record->params[i].value.ul;
        } else if (strcmp(record->params[i].field, "balloon.available") == 0) {
            vminfo->memstat.available = (uint64_t)record->params[i].value.ul;
        } else if (strcmp(record->params[i].field, "balloon.usable") == 0) {
            vminfo->memstat.usable = (uint64_t)record->params[i].value.ul;
        } else if (strcmp(record->params[i].field, "balloon.unused") == 0) {
            vminfo->memstat.unused = (uint64_t)record->params[i].value.ul;
        } else if (strcmp(record->params[i].field, "balloon.rss") == 0) {
            vminfo->memstat.rss = (uint64_t)record->params[i].value.ul;
        } else if (strcmp(record->params[i].field, "balloon.disk_caches") == 0) {
            if ((record->params[i].value.ul & ENABLED_FPR_FLAG) > 0) {
                vminfo->enable_fpr = true;
            }
        }
    }
}

static void ClearAllDomainStats(void)
{
    VirtualMachineInfoPtr vminfo = NULL;
    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        vminfo->memstat.actualBalloon = 0;
        vminfo->memstat.available = 0;
        vminfo->memstat.usable = 0;
        vminfo->memstat.unused = 0;
        vminfo->memstat.rss = 0;
        vminfo->enable_fpr = false;
    }
}

void GetAllDomainsStats(void)
{
    unsigned int stats = 0;
    unsigned int flags = VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT;
    virDomainStatsRecordPtr *records = NULL;
    virDomainStatsRecordPtr *next;
    char *errmsg = NULL;

    if (g_libvirtConn == NULL) {
        return;
    }

    ClearAllDomainStats();

    if (virConnectGetAllDomainStats(g_libvirtConn, stats, &records, flags) < 0) {
        errmsg = GetLastErrMsg();
        MEM_ERROR("Get all domain stats failed!, reason: %s", errmsg ? errmsg : "Unknown");
        goto CLEANUP;
    }

    next = records;
    while (*next) {
        GetOneDomainStats(*next);
        next++;
    }

CLEANUP:
    virDomainStatsRecordListFree(records);
}

int GetVmMemoryStat(const VirtualMachineInfoPtr vminfo)
{
    int ret = -1;
    int i = 0;
    int retNrStats = -1;
    char *errmsg = NULL;
    virDomainMemoryStatStruct stats[(int)VIR_DOMAIN_MEMORY_STAT_NR];

    if (!vminfo) {
        goto CLEANUP;
    }

    if (vminfo->dom) {
        retNrStats = virDomainMemoryStats(vminfo->dom, stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
        if (retNrStats < 0) {
            errmsg = GetLastErrMsg();
            MEM_ERROR("Domain [%s] get memory stat failed!, reason: %s", vminfo->domname,
                      errmsg ? errmsg : "Unknown");
            goto CLEANUP;
        }
    } else {
        MEM_ERROR("Dom is null but vminfo not, something went wrong!");
        goto CLEANUP;
    }

    vminfo->memstat.actualBalloon = 0;
    vminfo->memstat.available = 0;
    vminfo->memstat.usable = 0;
    vminfo->memstat.unused = 0;
    vminfo->memstat.rss = 0;
    vminfo->enable_fpr = false;

    for (i = 0; i < retNrStats; i++) {
        switch (stats[i].tag) {
            case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
                vminfo->memstat.actualBalloon = (uint64_t)stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_AVAILABLE:
                vminfo->memstat.available = (uint64_t)stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_USABLE:
                vminfo->memstat.usable = (uint64_t)stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_UNUSED:
                vminfo->memstat.unused = (uint64_t)stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_RSS:
                vminfo->memstat.rss = (uint64_t)stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_DISK_CACHES:
                if ((stats[i].val & ENABLED_FPR_FLAG) > 0) {
                    vminfo->enable_fpr = true;
                }
                break;
            default:
                break;
        }
    }
    ret = 0;
CLEANUP:
    return ret;
}

int CheckVmMemoryStat(const VirtualMachineInfoPtr vminfo)
{
    int ret = -1;

    if (!vminfo) {
        goto CLEANUP;
    }

    /* Check if actualBalloon and available are valid */
    if (vminfo->memstat.actualBalloon == 0 || vminfo->memstat.available == 0) {
        goto CLEANUP;
    }

    /* Check if at least one of usable and unused is valid */
    if (vminfo->memstat.usable == 0 && vminfo->memstat.unused == 0) {
        goto CLEANUP;
    }
    ret = 0;
CLEANUP:
    return ret;
}

/**
 * domain event handler, like lifecycle
 */
void ProcessDomainEvent(int domid, const char *name, int event, int detail ATTRIBUTE_UNUSED)
{
    switch ((virDomainEventType)event) {
        case VIR_DOMAIN_EVENT_STARTED:
            FindAddVMToList(domid);
            break;
        case VIR_DOMAIN_EVENT_STOPPED:
            FindDeleteVMFromList(name);
            break;
        case VIR_DOMAIN_EVENT_DEFINED:
            break;
        default:
            break;
    }
}

STATIC int DomainEventCallback(virConnectPtr conn ATTRIBUTE_UNUSED,
                               virDomainPtr dom,
                               int event,
                               int detail,
                               void *opaque ATTRIBUTE_UNUSED)
{
    const char *name;

    if (dom == NULL) {
        MEM_WARN("dom is null");
        return -1;
    }

    name = virDomainGetName(dom);
    if (name) {
        MEM_INFO("Domain [%s] event is %d", name, event);
        ProcessDomainEvent((int)virDomainGetID(dom), name, event, detail);
        return 0;
    }
    MEM_ERROR("can not get dom name when DomainEventCallback");
    return -1;
}

struct DomainEventData g_domainEvents[] = {
    DOMAIN_EVENT((int)VIR_DOMAIN_EVENT_ID_LIFECYCLE, DomainEventCallback),
};

STATIC int DomainEventsRegister(void)
{
    size_t i;
    for (i = 0; i < ARRAY_CARDINALITY(g_domainEvents); i++) {
        struct DomainEventData *event = g_domainEvents + i;

        event->id = virConnectDomainEventRegisterAny(g_libvirtConn, NULL,
                                                     event->event,
                                                     event->cb,
                                                     NULL,
                                                     NULL);

        if (event->id < 0) {
            MEM_ERROR("Failed to register event '%s'", event->name);
            return -1;
        }
    }
    return 0;
}

STATIC void DomainEventsDeregister()
{
    size_t i;
    for (i = 0; i < ARRAY_CARDINALITY(g_domainEvents); i++) {
        if (g_domainEvents[i].id >= 0) {
            (void)virConnectDomainEventDeregisterAny(g_libvirtConn, g_domainEvents[i].id);
            g_domainEvents[i].id = -1;
        }
    }
}

void *DomainEventProcessThread(void *data)
{
    MemSemaphore *readySem = (MemSemaphore *)data;
    bool initFlag = false;

    if (virEventRegisterDefaultImpl() < 0) {
        virErrorPtr err = virGetLastError();
        MEM_ERROR("Failed to register event implementation: %s",
                  err && err->message ? err->message : "Unknown error");
        exit(EXIT_FAILURE); // exit thread
    }

    while (g_stopMemlinkd == 0) {
        ConnectLibvirt();
        if (DomainEventsRegister() != 0) {
            goto CLEANUP;
        }

        /* Build vmlist, collect all current VMs into vmlist */
        MutexLock(&g_vmlistHead.requestLock);
        FreeAllVM();
        CollectAddCurrentVMs();
        MutexUnlock(&g_vmlistHead.requestLock);

        /* Notify that we are ready for domain event */
        SMP_RMB();
        if (!initFlag && g_stopMemlinkd != 1) {
            (void)SemPost(readySem);
            initFlag = true;
        } else {
            ATOMIC_SET(&g_libvirtChange, 1);
        }

        MEM_INFO("EventThread running");

        while (virConnectIsAlive(g_libvirtConn) != 0) {
            if (virEventRunDefaultImpl() < 0) {
                virErrorPtr err = virGetLastError();
                MEM_ERROR("Failed to run event loop: %s",
                          err && err->message ? err->message : "Unknown error");
                SleepApprox(1);
            }
        }

        MEM_INFO("EventThread exit, because of lose connection with libvirtd");

CLEANUP:
        DomainEventsDeregister();
        SleepApprox(1);
    }
    return NULL;
}

int SetVmBalloonTarget(const VirtualMachineInfoPtr vminfo, uint64_t newTarget)
{
    int ret;
    char *errmsg = NULL;

    ret = virDomainSetMemoryFlags(vminfo->dom, newTarget, VIR_DOMAIN_AFFECT_LIVE);
    if (ret < 0) {
        errmsg = GetLastErrMsg();
        MEM_ERROR("Domain %d set memory failed!, reason: %s",
                  vminfo->domid, errmsg ? errmsg : "Unknown");
    }

    return ret;
}

