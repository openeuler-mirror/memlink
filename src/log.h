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
 * Description: inspection message alarm program
 * Author: header file of memlinkd log
 * Create: 2024-10-8
 */

#ifndef _MEM_LOG_
#define _MEM_LOG_

#include <syslog.h>
#include <stdarg.h>
#include "util.h"

void LogInit(void);
void LogExit(void);
void Log(int level, const char *funcName, const char *fmt, ...);

#define MEM_DEBUG(...) \
    Log(LOG_DEBUG, __func__, __VA_ARGS__)

#define MEM_INFO(...) \
    Log(LOG_INFO, __func__, __VA_ARGS__)

#define MEM_WARN(...) \
    Log(LOG_WARNING, __func__, __VA_ARGS__)

#define MEM_ERROR(...) \
    Log(LOG_ERR, __func__, __VA_ARGS__)

#endif
