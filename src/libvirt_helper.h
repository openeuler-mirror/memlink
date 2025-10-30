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
 * Description: header file of libvirt helper function
 * Author: Liang Zhang
 * Create: 2024-10-23
 */

#ifndef _LIBVIRT_HELPER_H_
#define _LIBVIRT_HELPER_H_

#include <libvirt/virterror.h>
#include "memlink_driver.h"

extern int g_libvirtChange;
extern int g_stopMemlinkd;

char *GetLastErrMsg(void);

void SetVmMemoryStatsPeriod(const VirtualMachineInfoPtr vminfo, bool updateStats);
int GetVmMemoryStat(const VirtualMachineInfoPtr vminfo);
void GetAllDomainsStats(void);
int CheckVmMemoryStat(const VirtualMachineInfoPtr vminfo);
const char *GetDomainName(const VirtualMachineInfoPtr vminfo);
virDomainPtr LookupDomByID(int domid);
bool CheckDomainIsAlive(const VirtualMachineInfoPtr vminfo);

void *DomainEventProcessThread(void *data);
void ProcessDomainEvent(int domid, const char *name, int event, int detail);

int SetVmBalloonTarget(const VirtualMachineInfoPtr vminfo, uint64_t newTarget);
#endif
