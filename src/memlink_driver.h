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
 * Description: header file of memlink driver
 * Author: Liang Zhang
 * Create: 2025-02-17
 */


#ifndef _MEMLINK_DRIVER_H_
#define _MEMLINK_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>
#include <libvirt/libvirt.h>
#include <sys/queue.h>
#include "util.h"

typedef struct SMemlinkCfg MemlinkCfg;
typedef MemlinkCfg *MemlinkCfgPtr;
struct SMemlinkCfg {
    /* poll time for get host info and guest info */
    int hostInfoPollTime;
    int vmInfoPollTime;

    int balloonEnable;
    int balloonTargetUsedPercent;
    int balloonTargetMaxTotalPercent;

    int initSemWaitTimeout;
};

typedef struct SHostMemInfo HostMemInfo;
typedef HostMemInfo *HostMemInfoPtr;
struct SHostMemInfo {
    int64_t total;
    int64_t buffer;
    int64_t cached;
    int64_t swaptotal;
    int64_t swapfree;
    /* Hostmem cgroup info */
    uint64_t limit; /* MEMCG_LIMIT_FILE */
    uint64_t usage; /* MEMCG_USAGE_FILE */

    int16_t state;
    bool baseonCgroup;
};
extern HostMemInfo g_hostMemInfo;

extern int g_hostInfoPollTimeMs;
extern int g_initSemWaitTimeout;
extern int g_balloonEnable;
extern int g_balloonTargetUsedPercent;
extern int g_balloonTargetMaxTotalPercent;

int LoadCfgFile(void);
int GetIntegerValueFromCfg(const char *key);
void SetValueToCfg(const char *key, int value);

char *HostMemStateStr(int state);
int GetHostMemState(void);
uint64_t GetHostAvailableMem(void);
int InitHostMemoryinfo(void);
void InitAdvancedParameters(const MemlinkCfgPtr cfg);

/**
 * VM Memory Info Interface
 */
typedef enum {
    VM_NOSTATE = 0,     /* no state */
    VM_RUNNING = 1,     /* the domain is running */
    VM_BLOCKED = 2,     /* the domain is blocked on resource */
    VM_PAUSED = 3,      /* the domain is paused by user */
    VM_SHUTDOWN = 4,    /* the domain is being shut down */
    VM_SHUTOFF = 5,     /* the domain is shut off */
    VM_CRASHED = 6,     /* the domain is crashed */
    VM_PMSUSPENDED = 7, /* the domain is suspended by guest power management */
} MemlinkVmState;

typedef struct _VmMemoryStats VmMemoryStats;
typedef VmMemoryStats *VmMemoryStatsPtr;
struct _VmMemoryStats {
    uint64_t swapIn;
    uint64_t swapOut;
    uint64_t majorFault;
    uint64_t minorFault;
    uint64_t available;  /* MemTotal in Guest's /proc/meminfo, unit in KiB */
    uint64_t unused;  /* MemFree in Guest's /proc/meminfo, unit in KiB  */
    uint64_t actualBalloon;
    uint64_t rss;
    uint64_t usable;  /* MemAvailable in Guest's /proc/meminfo, unit in KiB  */
    uint64_t lastUpdate;
    uint64_t lastUpdateOld;
};

typedef struct _VMMemParameters VMMemParameters;
typedef VMMemParameters *VMMemParametersPtrs;
struct _VMMemParameters {
    uint64_t autoMem;
    uint64_t weight; /* Represent the priority of the VM */
    uint64_t memShare;
    uint64_t hardlimitFromLibvirt; /* Got from libvirtd, atomic paramter */
    uint64_t softLimit;
    uint64_t minGuarantee;
    uint64_t swapHardLimit;
};

typedef struct _VirtualMachineInfo VirtualMachineInfo;
typedef VirtualMachineInfo *VirtualMachineInfoPtr;
struct _VirtualMachineInfo {
    virDomainPtr dom;

    int domid;
    char *domname;
    int64_t memory;      /* refers to the value of <memory> in XML, unit in KiB  */
    int64_t currentMem;  /* refers to the value of <current_memory> in XML, unit in KiB  */
    uint64_t reservMem;  /* refers to the value of <min_guarantee> in XML, unit in KiB  */
    uint64_t shareMem;   /* internal calculated value based on weight */
    uint64_t balanceMem; /* expect this memory when system is in stable state */

    VmMemoryStats memstat;
    VMMemParameters memparam;
    bool enable_fpr;     /* if Guest support Free-Page-Reporting and enabled */

    int16_t state;

    int64_t balloonTarget;  /* unit in KiB */
    int64_t currentActualBalloon[2]; /* record 2 balloon value to compute balloon rate */
    bool ballooning;
    int needBalloonFlag;
    int balloonI;
    bool ballooningDown;
    bool incompleteBalloon;

    bool isWindows;
    bool lockall; /* all memory locked domain */
    int refcount;
    bool logged; /* 1: logged error of getting inband info already */

    LIST_ENTRY(_VirtualMachineInfo) entry;
};

typedef struct _VirtualMachineList VirtualMachineList;
typedef VirtualMachineList *VirtualMachineListPtr;
struct _VirtualMachineList {
    LIST_HEAD(DomainsListHead, _VirtualMachineInfo) domainsListHead;
    int32_t count;
    MemlinkMutex requestLock;
    MemlinkMutex lock;
};

extern VirtualMachineList g_vmlistHead;

int GetVmInfo(const VirtualMachineInfoPtr vminfo);
void UpdateAllVmInfo(void);
int UpdateVmInfo(VirtualMachineInfoPtr vminfo);
void FreeAllVM(void);
void FindAddVMToList(int domid);
void FindDeleteVMFromList(const char *name);

VirtualMachineInfoPtr GetRefVMInfoByDomid(int domid);
void PutUnrefVMInfo(VirtualMachineInfoPtr vminfo);
void VMInfoRef(VirtualMachineInfoPtr vminfo);
void VMInfoUnref(VirtualMachineInfoPtr vminfo);

#endif
