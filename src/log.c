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
 * Description: memlinkd log
 * Author: Liang Zhang
 * Create: 2024-10-8
 */

#include <stdio.h>
#include "log.h"

#define LOG_BUF_SIZE 256

/**
 * Log file: "/var/log/memlinkd.log"
 *
 * Logging levels (from sys/syslog.h)
 *     LOG_EMERG = 0      system is unusable
 *     LOG_ALERT = 1       action must be taken immediately
 *     LOG_CRIT = 2     critical conditions
 *     LOG_ERR = 3      error conditions
 *     LOG_WARNING = 4   warning conditions
 *     LOG_NOTICE = 5    normal but significant condition
 *     LOG_INFO = 6   informational
 *     LOG_DEBUG = 7    debug-level messages
 */
void Log(int level, const char *funcName, const char *fmt, ...)
{
#define END_LOCATION 2
    char buf[LOG_BUF_SIZE] = { 0 };
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = vsnprintf_s(buf, LOG_BUF_SIZE, LOG_BUF_SIZE - 1, fmt, ap);
    if (rc == -1) {
        syslog(level, "%s", "Fail to print string to buf");
    }
    va_end(ap);
    buf[LOG_BUF_SIZE - END_LOCATION] = '\n';
    buf[LOG_BUF_SIZE - 1] = 0;

    syslog(level, "%s: %s", funcName, buf);
}

void LogExit(void)
{
    closelog();
}

void LogInit(void)
{
    openlog("memlinkd", LOG_PID, 0);
}

