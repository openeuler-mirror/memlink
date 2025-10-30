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
 * Description: memlinkd driver
 * Author: Liang Zhang
 * Create: 2025-02-17
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <limits.h>

#include "util.h"
#include "log.h"
#include "libvirt_helper.h"
#include "memlink_driver.h"

#define MAX_STATE 100
#define LINE_MAX_CHAR  1024
#define VALUE_MAX_CHAR 20
#define HOST_INFO_POLL_TIME_DEFAULT 1000
#define BALLOON_ENABLE_DEFAULT 1
#define BALLOON_TARGET_USED_PERCENT 130
#define BALLOON_TARGET_MAX_TOTAL_PERCENT 50

#define INIT_SEM_WAIT_TIMEOUT_DEFAULT 600000
#define CFGOPEN_OR_MEMSET_ERROR (-2)

HostMemInfo g_hostMemInfo = { 0 };

VirtualMachineList g_vmlistHead = {
    .domainsListHead = LIST_HEAD_INITIALIZER(domainsListHead),
    .lock.lock = PTHREAD_MUTEX_INITIALIZER,
    .requestLock.lock = PTHREAD_MUTEX_INITIALIZER,
    .count = 0,
};

/* memlink working cycle, overwritten by config file */
int g_hostInfoPollTimeMs = 1000;

/* enable memlink balloon and pause function, overwritten by config file */
int g_balloonEnable = 1;
int g_balloonTargetUsedPercent = 130;
int g_balloonTargetMaxTotalPercent = 50;

/*
 * init_sem_wait_timeout in ms, overwritten by config file.
 * the timeout is used to exit memlink while worst cases happened,
 * such as libvirt didn't return.
 *
 * */
int g_initSemWaitTimeout = 600000;

/**
 * VM Memory Info Interface
 */
static VirtualMachineInfoPtr NewVM(int domid)
{
    VirtualMachineInfoPtr vminfo = NULL;

    vminfo = calloc(1, sizeof(*vminfo));
    if (vminfo == NULL) {
        return NULL;
    }

    vminfo->domid = domid;
    vminfo->ballooning = false;
    vminfo->incompleteBalloon = false;
    /**
    * Trying in BalloonVmMemThread is not harmful while
    * assume that a new vm added to list could be ballooned.
    * There are two situations:
    * 1. A new vm while memlinkd is running
    * 2. Restarting memlinkd while vm has already ran
    */
    vminfo->needBalloonFlag = 1;
    vminfo->logged = false;
    ATOMIC_SET(&vminfo->refcount, 1);
    vminfo->lockall = false;
    vminfo->currentActualBalloon[0] = 0;
    vminfo->currentActualBalloon[1] = 0;
    vminfo->balloonI = 0;
    vminfo->ballooningDown = false;

    return vminfo;
}

static void FreeVM(VirtualMachineInfoPtr vminfo)
{
    if (!vminfo) {
        return;
    }

    if (vminfo->dom) {
        (void)virDomainFree(vminfo->dom);
    }
    if (vminfo->domname) {
        free(vminfo->domname);
        vminfo->domname = NULL;
    }
    free(vminfo);
}

/* free all vminfo(outgoing interfaces) */
void FreeAllVM(void)
{
    VirtualMachineInfoPtr vminfo = NULL;
    VirtualMachineInfoPtr vminfoTmp = NULL;

    MutexLock(&g_vmlistHead.lock);
    for (vminfo = (&g_vmlistHead.domainsListHead)->lh_first;
         vminfo; vminfo = vminfoTmp) {
        vminfoTmp = (vminfo)->entry.le_next;
        VMInfoUnref(vminfo);
    }
    MutexUnlock(&g_vmlistHead.lock);
    return;
}

STATIC int InitAndCheckVM(VirtualMachineInfoPtr vminfo)
{
    const char *name = NULL;
    char *errmsg = NULL;
    int rc;

    vminfo->dom = LookupDomByID(vminfo->domid);
    if (!vminfo->dom) {
        errmsg = GetLastErrMsg();
        MEM_ERROR("Failed to add VM[%d] list becasuse of lookup it failed, reason: %s",
                  vminfo->domid, errmsg ? errmsg : "Unknown");
        return -1;
    }
    name = GetDomainName(vminfo);
    if (name == NULL) {
        return -1;
    }
    if (CopyString(&(vminfo->domname), name) < 0) {
        return -1;
    }

    /* VM with hg managed by hugetlbfs, rather than common memory system, we should pass it */
    rc = CheckVMHasHugePage(vminfo->domname);
    if (rc > 0) {
        MEM_INFO("[%s] is a VM with huge pages, no need to insert vmlist",
                 vminfo->domname);
        return -1;
    }

    /* windows VM is a kind of special VM, will not be ballooned */
    rc = CheckVMIsWindows(vminfo->domname);
    if (rc >= 0) {
        vminfo->isWindows = (rc != 0);
    }

    /* init and get all info of this VM, before add it into vmlist */
    if (UpdateVmInfo(vminfo) < 0) {
        MEM_ERROR("Failed to add VM[%s] to list because can not init VmInfo",
                  vminfo->domname);
        return -1;
    }

    if (g_balloonEnable == 1) {
        SetVmMemoryStatsPeriod(vminfo, true);
    }

    return 0;
}

void FindAddVMToList(int domid)
{
    VirtualMachineInfoPtr vminfo = NULL;
    int ret = -1;

    MutexLock(&g_vmlistHead.lock);
    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        if (vminfo->domid == domid) {
            MutexUnlock(&g_vmlistHead.lock);
            return;
        }
    }
    vminfo = NewVM(domid);
    if (vminfo == NULL) {
        MEM_ERROR("Failed to alloc memory for vminfo");
        goto OUT;
    }
    if (InitAndCheckVM(vminfo) != 0) {
        goto OUT;
    }
    LIST_INSERT_HEAD(&g_vmlistHead.domainsListHead, vminfo, entry);
    g_vmlistHead.count++;
    MEM_INFO("Add [%s]:%d into vmlist, totalVMs: %d",
             vminfo->domname, domid, g_vmlistHead.count);

    ret = 0;

OUT:
    if ((ret != 0) && (vminfo != NULL)) {
        FreeVM(vminfo);
    }
    MutexUnlock(&g_vmlistHead.lock);
    return;
}

static void DeleteVMFromList(VirtualMachineInfoPtr vminfo)
{
    LIST_REMOVE(vminfo, entry);
    g_vmlistHead.count--;
    MEM_INFO("Delete VM[%s] from vmlist, count:%d",
             vminfo->domname, g_vmlistHead.count);
    FreeVM(vminfo);
}

/* Must hold vmlist_head.lock */
void VMInfoRef(VirtualMachineInfoPtr vminfo)
{
    ATOMIC_ADD_FETCH(&vminfo->refcount, 1);
}

/* Must hold vmlist_head.lock */
void VMInfoUnref(VirtualMachineInfoPtr vminfo)
{
    if (ATOMIC_SUB_FETCH__NOCHECK(&vminfo->refcount, 1) == 0) {
        DeleteVMFromList(vminfo);
    }
}

void FindDeleteVMFromList(const char *name)
{
    VirtualMachineInfoPtr vminfo = NULL;

    MutexLock(&g_vmlistHead.lock);
    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        if (strcmp(vminfo->domname, name) == 0) {
            VMInfoUnref(vminfo);
            break;
        }
    }
    MutexUnlock(&g_vmlistHead.lock);
}

VirtualMachineInfoPtr GetRefVMInfoByDomid(int domid)
{
    VirtualMachineInfoPtr vminfo = NULL;

    MutexLock(&g_vmlistHead.lock);
    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        if (vminfo->domid == domid) {
            VMInfoRef(vminfo);
            break;
        }
    }
    MutexUnlock(&g_vmlistHead.lock);

    return vminfo;
}

void PutUnrefVMInfo(VirtualMachineInfoPtr vminfo)
{
    if (!vminfo) {
        return;
    }
    MutexLock(&g_vmlistHead.lock);
    VMInfoUnref(vminfo);
    MutexUnlock(&g_vmlistHead.lock);
}

/* update single vminfo by domid */
int UpdateVmInfo(VirtualMachineInfoPtr vminfo)
{
    int ret = -1;

    if (GetVmInfo(vminfo) < 0) {
        goto OUT;
    }

    ret = 0;
OUT:
    return ret;
}

/*
 * update all vminfo: should not be called in mutil thread. (outgoing interfaces)
*/
void UpdateAllVmInfo(void)
{
    VirtualMachineInfoPtr vminfo = NULL;

    MutexLock(&g_vmlistHead.lock);
    LIST_FOREACH(vminfo, &g_vmlistHead.domainsListHead, entry) {
        if (UpdateVmInfo(vminfo) < 0) {
            continue;
        }
    }
    MutexUnlock(&g_vmlistHead.lock);
}

static void TrimNewLine(char *string)
{
    char *end = string + strlen(string) - 1;
    while (end > string && (*end == ' ' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
}

static bool CheckAllDigits(const char *string)
{
    while (*string == ' ') {
        string++;
    }

    if (*string == '\0') {
        return false;
    }

    if (*string == '-') {
        string++;
        if (*string < '0' || *string > '9') {
            return false;
        }
    }

    while (*string != '\0') {
        if (!isdigit((unsigned char)*string)) {
            return false;
        }
        string++;
    }
    return true;
}

int GetIntegerValueFromCfg(const char *key)
{
    FILE *fp = NULL;
    char line[LINE_MAX_CHAR] = { 0 };
    int value = -1;
    char *endptr;

    fp = fopen(MEMLINKD_CONF_FILE, "r");
    if (fp == NULL) {
        MEM_ERROR("Can't open config file");
        return CFGOPEN_OR_MEMSET_ERROR;
    }
    while (fgets(line, LINE_MAX_CHAR, fp) != NULL) {
        if (strncmp(line, key, strlen(key)) == 0) {
            if (strstr(line, "=")) {
                char *p = strstr(line, "=") + 1;
                TrimNewLine(p);
                if (!CheckAllDigits(p)) {
                    MEM_WARN("Get integer value from wrong format '%s' in config file", line);
                } else {
                    value = strtol(p, &endptr, DECIMAL);
                    if (*endptr != '\0') {
                        MEM_WARN("Invalid ending %s", endptr);
                        fclose(fp);
                        return CFGOPEN_OR_MEMSET_ERROR;
                    }
                    if (errno == ERANGE) {
                        MEM_WARN("Number out of range");
                        fclose(fp);
                        return CFGOPEN_OR_MEMSET_ERROR;
                    }

                    if (value < INT_MIN || value > INT_MAX) {
                        MEM_WARN("Number out of int range");
                        fclose(fp);
                        return CFGOPEN_OR_MEMSET_ERROR;
                    }
                    break;
                }
            } else {
                MEM_WARN("Get interger value from wrong format '%s' in config file", line);
            }
        }
        if (memset_s(line, LINE_MAX_CHAR, 0, LINE_MAX_CHAR) != 0) {
            MEM_ERROR("Interger fail to memset strings");
            if (fp) {
                fclose(fp);
            }
            return CFGOPEN_OR_MEMSET_ERROR;
        }
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return value;
}

/* copy file @fpIn to file @fpOut.
 * if exist @key, it will change the value to @valueChanged
 * @return: return 0 on success; return -1 on error
 */
static int CopyToFile(FILE *fpIn, FILE *fpOut, const char *key, int valueChanged)
{
    char line[LINE_MAX_CHAR] = { 0 };
    char tmpkey[LINE_MAX_CHAR] = { 0 };
    int tmpValue = 0;
    int rc = 0;

    while (fgets(line, LINE_MAX_CHAR, fpIn) != NULL) {
        if (line[0] == '#' || line[0] == '\n' || !strstr(line, key)) {
            if (fputs(line, fpOut) < 0) {
                MEM_ERROR("Fail to puts string");
                return -1;
            }
            if (memset_s(line, LINE_MAX_CHAR, 0, LINE_MAX_CHAR) != 0) {
                MEM_ERROR("Fail to memset strings");
                return -1;
            }
            continue;
        }
        rc = sscanf_s(line, "%[^=]=%d\n", tmpkey, LINE_MAX_CHAR, &tmpValue);
        if (rc < 0) {
            MEM_ERROR("Failed to parse key");
            return -1;
        }
        if (strcmp(tmpkey, key) == 0) {
            tmpValue = valueChanged;
        }
        if (fprintf(fpOut, "%s=%d\n", tmpkey, tmpValue) < 0) {
            return -1;
        }
        if (memset_s(line, LINE_MAX_CHAR, 0, LINE_MAX_CHAR) != 0) {
            MEM_ERROR("Fail to memset strings");
            return -1;
        }
    }

    return 0;
}

void SetValueToCfg(const char *key, int value)
{
    FILE *fpIn = NULL;
    FILE *fpOut = NULL;

    int ret = 0;

    fpIn = fopen(MEMLINKD_CONF_FILE, "r");
    if (fpIn == NULL) {
        MEM_ERROR("Can't open config file");
        goto END;
    }

    fpOut = fopen(MEMLINKD_TMP_CONF_FILE, "w+");
    if (fpOut == NULL) {
        MEM_ERROR("can't open tmp config file");
        goto END;
    }

    if (CopyToFile(fpIn, fpOut, key, value) != 0) {
        goto END;
    }
    ret = 1;

END:
    if (fpIn != NULL) {
        fclose(fpIn);
    }
    if (fpOut != NULL) {
        fclose(fpOut);
    }
    if (ret != 0) {
        if (rename(MEMLINKD_TMP_CONF_FILE, MEMLINKD_CONF_FILE) != 0) {
            MEM_ERROR("can't overwrite config file");
        }
    }
}

#define GET_INT_VALUE_FROM_CFG(str, key, value, tmp) do { \
    tmp = GetIntegerValueFromCfg(str); \
    if ((tmp) == CFGOPEN_OR_MEMSET_ERROR) { \
        goto CLEANUP; \
    } else if ((tmp) == -1) { \
        MEM_WARN("Get value from cfg failed. Default value %d is used for %s", value, str); \
        key = value; \
    } else { \
        key = tmp; \
    } \
} while (0)

int LoadCfgFile(void)
{
#define MIN_POLL_TIME 100
#define MAX_POLL_TIME 6000
#define MIN_TIMEOUT 10000
#define MAX_TIMEOUT 3600000
#define MAX_USED_PERCENT 10000
#define MAX_PERCENT 100
#define BALLOON_TARGET_PERCENT_MIN  120

    MemlinkCfgPtr cfg = NULL;
    int tmpInt;
    int ret = -1;

    cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL) {
        MEM_ERROR("alloc config pointer failed");
        return ret;
    }

    GET_INT_VALUE_FROM_CFG("host_info_poll_time", cfg->hostInfoPollTime, HOST_INFO_POLL_TIME_DEFAULT, tmpInt);
    GET_INT_VALUE_FROM_CFG("init_sem_wait_timeout", cfg->initSemWaitTimeout, INIT_SEM_WAIT_TIMEOUT_DEFAULT, tmpInt);
    GET_INT_VALUE_FROM_CFG("balloon_enable", cfg->balloonEnable, BALLOON_ENABLE_DEFAULT, tmpInt);
    GET_INT_VALUE_FROM_CFG("balloon_target_used_percent", cfg->balloonTargetUsedPercent,
                           BALLOON_TARGET_USED_PERCENT, tmpInt);
    GET_INT_VALUE_FROM_CFG("balloon_target_max_total_percent", cfg->balloonTargetMaxTotalPercent,
                           BALLOON_TARGET_MAX_TOTAL_PERCENT, tmpInt);

    ret = 0;
    /* host poll time */
    if (cfg->hostInfoPollTime >= MIN_POLL_TIME && cfg->hostInfoPollTime <= MAX_POLL_TIME) {
        g_hostInfoPollTimeMs = cfg->hostInfoPollTime;
    }

    /* init_sem_wait_timeout, timeout time is 10s */
    if (cfg->initSemWaitTimeout >= MIN_TIMEOUT && cfg->initSemWaitTimeout <= MAX_TIMEOUT) {
        g_initSemWaitTimeout = cfg->initSemWaitTimeout;
    }

    if (cfg->balloonEnable >= 0 && cfg->balloonEnable <= 1) {
        g_balloonEnable = cfg->balloonEnable;
    }

    if (cfg->balloonTargetUsedPercent >= BALLOON_TARGET_PERCENT_MIN &&
        cfg->balloonTargetUsedPercent <= MAX_USED_PERCENT) {
        g_balloonTargetUsedPercent = cfg->balloonTargetUsedPercent;
    }

    if (cfg->balloonTargetMaxTotalPercent >= 0 && cfg->balloonTargetMaxTotalPercent <= MAX_PERCENT) {
        g_balloonTargetMaxTotalPercent = cfg->balloonTargetMaxTotalPercent;
    }

    MEM_INFO("Load config file ok!");

CLEANUP:
    free(cfg);
    return ret;
}

#undef GET_INT_VALUE_FROM_CFG
