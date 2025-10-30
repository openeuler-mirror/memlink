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
 * Description: memlinkd util
 * Author: Liang Zhang
 * Create: 2024-10-8
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include "log.h"
#include "util.h"

int MutexInit(MemMutexPtr m)
{
    int ret;
    pthread_mutexattr_t attr;
    (void)pthread_mutexattr_init(&attr);
    (void)pthread_mutexattr_settype(&attr, 0);
    ret = pthread_mutex_init(&m->lock, &attr);
    (void)pthread_mutexattr_destroy(&attr);
    if (ret != 0) {
        return -1;
    }
    return 0;
}

void MutexDestroy(MemMutexPtr m)
{
    (void)pthread_mutex_destroy(&m->lock);
}

void MutexLock(MemMutexPtr m)
{
    (void)pthread_mutex_lock(&m->lock);
}

void MutexUnlock(MemMutexPtr m)
{
    (void)pthread_mutex_unlock(&m->lock);
}

int MutexTryLock(MemMutexPtr m)
{
    return pthread_mutex_trylock(&m->lock);
}

void SleepApprox(int seconds)
{
#define US_TO_NS_RATE 1000
#define S_TO_NS_RATE 1000000000
    struct timespec ts;
    struct timeval tv;

    if (gettimeofday(&tv, NULL) == -1) {
        MEM_ERROR("%s failed, errno: %d\n", __func__, errno);
    }
    ts.tv_sec = seconds;
    ts.tv_nsec = -tv.tv_usec * US_TO_NS_RATE;
    while (ts.tv_nsec < 0) {
        ts.tv_sec--;
        ts.tv_nsec += S_TO_NS_RATE;
    }
    (void)nanosleep(&ts, NULL);
}

STATIC char *GetOneLineFromFile(const char *filename, size_t max, size_t *count)
{
    FILE *fp = NULL;
    size_t size;
    char *line = NULL;
    char *canonicalPath = NULL;

    if (max == 0) {
        MEM_ERROR("alloc size 0");
        return NULL;
    }

    line = calloc(max, sizeof(*line));
    if (!line) {
        MEM_ERROR("Can't alloc %d bytes", max);
        return NULL;
    }

    canonicalPath = realpath(filename, NULL);
    if (canonicalPath == NULL) {
        MEM_ERROR("Invalid file path %s", filename);
        free(line);
        return NULL;
    }
    fp = fopen(canonicalPath, "r");
    if (!fp) {
        MEM_ERROR("Can't open file %s, errno: %d", filename, errno);
        free(line);
        free(canonicalPath);
        return NULL;
    }

    size = fread(line, sizeof(char), max - 1, fp);
    if (size == 0) {
        MEM_ERROR("Faile to read from file %s,"
                  " errno: %d\n",
                  filename, errno);
        free(line);
        fclose(fp);
        free(canonicalPath);
        return NULL;
    }

    *count = size;
    if (fp) {
        fclose(fp);
    }
    free(canonicalPath);
    return line;
}

#ifdef CUNIT
#define QEMU_PID_DIR "/tmp/"
#else
#define QEMU_PID_DIR "/var/run/libvirt/qemu/"
#endif

int GetQEMUPidByVMName(const char *vmName)
{
    char pidFilePath[PATH_MAX] = { 0 };
    char *pidStr = NULL;
    int pid;
    size_t i;
    size_t count = 0;
    size_t len;
    int rc;
    char *endptr;

    if (!vmName) {
        return -1;
    }
    rc = snprintf_s(pidFilePath, sizeof(pidFilePath), sizeof(pidFilePath) - 1, "%s%s.pid",
                    QEMU_PID_DIR, vmName);
    if (rc < 0) {
        MEM_ERROR("Failed to print to pid_file_path");
        return -1;
    }
    /* get one line from file, max size is 64 */
    pidStr = GetOneLineFromFile(pidFilePath, 64, &count);
    if (!pidStr || count >= PATH_MAX || count == 0) {
        if (pidStr != NULL) {
            free(pidStr);
        }
        return -1;
    }
    len = strlen(pidStr);
    for (i = 0; i < len; i++) {
        if (!isdigit((unsigned char)pidStr[i])) {
            MEM_ERROR("There is invalid pid value: %s in pid file: %s",
                      pidStr, pidFilePath);
            free(pidStr);
            return -1;
        }
    }
    pid = strtol(pidStr, &endptr, DECIMAL);
    if (*endptr != '\0') {
        free(pidStr);
        MEM_WARN("Invalid ending %s", endptr);
        return -1;
    }
    if (errno == ERANGE) {
        free(pidStr);
        MEM_WARN("Number out of range");
        return -1;
    }

    if (pid < INT_MIN || pid > INT_MAX) {
        free(pidStr);
        MEM_WARN("Number out of int range");
        return -1;
    }
    free(pidStr);
    return pid;
}

#define CGROUP_MEMORY_DIR "/sys/fs/cgroup/memory"
#define PATH_MAX_SIZE     4096

static int GetProcCgroupPath(const char *vmName, char *procCgroupPath)
{
    int pid;
    pid = GetQEMUPidByVMName(vmName);
    if (pid < 0) {
        return -1;
    }
#ifdef CUNIT
    if (snprintf_s(procCgroupPath, PATH_MAX_SIZE, PATH_MAX_SIZE - 1, "/proc/%ld/cgroup", (long)getpid()) <= 0) {
        return -1;
    }
#else
    if (snprintf_s(procCgroupPath, PATH_MAX_SIZE, PATH_MAX_SIZE - 1, "/proc/%d/cgroup", pid) <= 0) {
        return -1;
    }
#endif
    return 0;
}

STATIC int GetCgroupFilePath(const char *vmName, const char *type, char *cgroupFilePath)
{
    char procCgroupPath[PATH_MAX_SIZE] = { 0 };
    char controller[PATH_MAX_SIZE] = { 0 };
    FILE *fp = NULL;
    size_t size;
    char *token = NULL;
    char *nextToken = NULL;

    if (GetProcCgroupPath(vmName, procCgroupPath) < 0) {
        return -1;
    };

    fp = fopen(procCgroupPath, "r");
    if (!fp) {
        MEM_ERROR("Can't open file %s, reason: %s", procCgroupPath, strerror(errno));
        return -1;
    }

    do {
        if (!fgets(controller, PATH_MAX_SIZE, fp)) {
            fclose(fp);
            return -1;
        }

        token = strtok_s(controller, ":", &nextToken); /* id */
        token = strtok_s(NULL, ":", &nextToken);       /* controller */
        if (!token) {
            fclose(fp);
            return -1;
        }
    } while (strcmp(token, "memory") != 0);

    token = strtok_s(NULL, ":", &nextToken);
    if (!token) {
        fclose(fp);
        return -1;
    }
    size = strlen(token);
    token[size - 1] = '\0'; /* remove \n */
    if (fp) {
        fclose(fp);
    }

    if (snprintf_s(cgroupFilePath, PATH_MAX_SIZE, PATH_MAX_SIZE - 1, "%s%s/memory.%s_in_bytes",
                   CGROUP_MEMORY_DIR, token, type) <= 0) {
        return -1;
    } else {
        return 0;
    }
}

int ReadCgroupPathU64(const char *path, const uint64_t *val)
{
    FILE *fp = NULL;
    int size;
    int ret = -1;
    char *canonicalPath = NULL;

    if (!(path && val)) {
        MEM_ERROR("path or val is NULL!");
        goto OUT;
    }
    canonicalPath = realpath(path, NULL);
    if (canonicalPath == NULL) {
        MEM_ERROR("Invalid file path %s", path);
        goto OUT;
    }
    fp = fopen(canonicalPath, "r");
    if (!fp) {
        MEM_ERROR("Can't open file %s, reason: %s", path, strerror(errno));
        goto OUT;
    }
    size = fscanf_s(fp, "%llu", val);
    if (size <= 0) {
        MEM_ERROR("Can't read file %s, reason: %s", path, strerror(errno));
        goto OUT;
    }

    ret = 0;

OUT:
    if (fp) {
        fclose(fp);
    }
    if (canonicalPath  != NULL) {
        free(canonicalPath);
    }
    return ret;
}

/* retry_count: we may failed several times for some special file, like memcg.limit */
int WriteStringToFile(const char *path, const char *string, int retryCount)
{
    int fd = -1;
    int ret = -1;
    int nrRetry = retryCount;
    char *canonicalPath = NULL;

    if (path == NULL || string == NULL) {
        MEM_ERROR("Invalid parameters");
        return -1;
    }

    canonicalPath = realpath(path, NULL);
    if (canonicalPath == NULL) {
        MEM_ERROR("Invalid file path %s", path);
        goto OUT;
    }

    /* here, we need to make sure write data to file, not buffer */
    /* so use open/write, not fopen/fprintf */
    fd = open(canonicalPath, O_WRONLY | O_TRUNC);
    if (fd == -1) {
        MEM_ERROR("Can't open file %s, reason: %s", path, strerror(errno));
        goto OUT;
    }

    ret = (int)write(fd, string, strlen(string));
    if (ret >= 0) {
        goto OUT;
    }

    /* Write failed let's retry */
    while (nrRetry != 0) {
        if (write(fd, string, strlen(string)) < 0) {
            nrRetry--;
        } else {
            break;
        }
    }
    if (nrRetry == 0) {
        goto OUT;
    }
    ret = 0;

OUT:
    if (fd >= 0) {
        close(fd);
    }
    if (canonicalPath  != NULL) {
        free(canonicalPath);
    }
    return ret;
}

#define MAX_LONG_SIZE 32

int WriteCgroupPathU64(const char *path, uint64_t val, int retryCount)
{
    char value[MAX_LONG_SIZE] = { 0 };
    int rc;

    rc = sprintf_s(value, MAX_LONG_SIZE, "%llu", val);
    if (rc < 0) {
        MEM_ERROR("Failed to write value to cgroup");
        return -1;
    }

    return WriteStringToFile(path, value, retryCount);
}

int WriteCgroupPathS64(const char *path, int64_t val, int retryCount)
{
    char value[MAX_LONG_SIZE] = { 0 };
    int rc;

    rc = sprintf_s(value, MAX_LONG_SIZE, "%lld", val);
    if (rc < 0) {
        MEM_ERROR("Failed to write value to cgroup");
        return -1;
    }

    return WriteStringToFile(path, value, retryCount);
}

int GetQEMUMemoryUsage(const char *vmName, uint64_t *usage)
{
#define RATE_SHIFT 10
    char *usagePath = NULL;
    int ret = -1;
    int rc;

    usagePath = (char *)malloc(PATH_MAX_SIZE * sizeof(char));
    if (usagePath == NULL) {
        MEM_ERROR("Can't alloc memory for cgroup mem usage path");
        goto OUT;
    }

    rc = memset_s(usagePath, PATH_MAX_SIZE * sizeof(char), 0, PATH_MAX_SIZE * sizeof(char));
    if (rc != 0) {
        MEM_ERROR("Failed to memset usage_path");
        goto OUT;
    }

    if (GetCgroupFilePath(vmName, "usage", usagePath) < 0) {
        goto OUT;
    }

    if (ReadCgroupPathU64(usagePath, usage) < 0) {
        goto OUT;
    }
    *usage = *usage >> RATE_SHIFT; /* bytes --> kbytes */
    ret = 0;

OUT:
    if (usagePath) {
        free(usagePath);
    }
    return ret;
}

int CopyString(char **dst, const char *src)
{
    *dst = NULL;
    if (!src) {
        MEM_WARN("src is null.");
        return -1;
    }
    *dst = strdup(src);
    if (!(*dst)) {
        MEM_ERROR("strdup failed, reason: %s", strerror(errno));
        return -1;
    }
    return 1;
}

#define MEMLINK_ARG_MAX 13072

/* check whether @strLine is contain one of part of @tokens.
 * @tokens is splited into several parts by ":".
 * return 1 on @strLine containning one of part of @tokens.
 * return 0 on @strLine not containning any part of @tokens.
 */
STATIC int ContainOneOfTokens(const char *strLine, size_t count, char *tokens)
{
    const char *ptr = NULL;
    char *nextToken = NULL;
    char *token = NULL;
    size_t i;
    size_t len;

    token = strtok_s(tokens, ":", &nextToken);
    while (token) {
        ptr = strLine;
        i = 0;
        while (i < count) {
            len = strlen(ptr);
            if (len == 0) {
                i++;
                ptr++;
                continue;
            }
            if (strstr(ptr, token)) {
                return 1;
            }
            i += len;
            ptr += len;
        }
        token = strtok_s(NULL, ":", &nextToken);
    }
    return 0;
}

STATIC int CheckString(const char *vmName, const char *str)
{
#define MAX_BUF_LEN 100
    long pid;
    char processProc[PATH_MAX] = { 0 };
    char *strLine = NULL;
    char buf[MAX_BUF_LEN]; /* 100 is max len of str */
    size_t count = 0;
    int rc;
    int ret = -1;

    pid = GetQEMUPidByVMName(vmName);
    if (pid <= 0) {
        goto CLEANUP;
    }

#ifdef CUNIT
    rc = sprintf_s(processProc, sizeof(processProc), "/proc/%ld/cmdline", (long)getpid());
#else
    rc = sprintf_s(processProc, sizeof(processProc), "/proc/%ld/cmdline", pid);
#endif
    if (rc < 0) {
        MEM_ERROR("Failed to print process_proc");
        goto CLEANUP;
    }

    strLine = GetOneLineFromFile(processProc, MEMLINK_ARG_MAX, &count);
    if (!strLine) {
        goto CLEANUP;
    }

    if (strlen(str) >= MAX_BUF_LEN) {
        MEM_ERROR("The length of str is too long");
        goto CLEANUP;
    }
    rc = strncpy_s(buf, sizeof(buf), str, strlen(str));
    if (rc != 0) {
        MEM_ERROR("Failed to copy string");
        goto CLEANUP;
    }

    ret = ContainOneOfTokens(strLine, count, buf);
CLEANUP:
    free(strLine);
    return ret;
}

int CheckVMHasPassthroughDev(const char *vmName)
{
    /*
     * Hopes there are no VM's name including 'vfio-pci' or 'pci-assign' string,
     * Or maybe we shoule check '-device vfio-pci' or '-device pci-assign' to
     * Avoid this insane cases. Or any better ways to find VM has passthrough device ?
     */
    return CheckString(vmName, "vfio-pci:pci-assign");
}

int CheckVMHasHugePage(const char *vmName)
{
    return CheckString(vmName, "prealloc");
}

int CheckVMIsWindows(const char *vmName)
{
    return CheckString(vmName, "hv_relaxed");
}

int ThreadCreate(MemlinkdThread *thread, const char *name,
                 void *(*startRoutine)(void *),
                 void *arg, int mode)
{
    pthread_attr_t attr;
    sigset_t set, oldset;
    int err;

    err = pthread_attr_init(&attr);
    if (err != 0) {
        MEM_ERROR("pthread_attr_init failed\n");
        return -1;
    }
    if (mode == MEMLINK_THREAD_DETACHED) {
        err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (err != 0) {
            MEM_ERROR("pthread_attr_setdetachstate error\n");
            (void)pthread_attr_destroy(&attr);
            return -1;
        }
    }
    /* Leave signal handling to the iothread.  */
    (void)sigfillset(&set);
    (void)pthread_sigmask(SIG_SETMASK, &set, &oldset);
    err = pthread_create(&thread->thread, &attr, startRoutine, arg);
    if (err != 0) {
        MEM_ERROR("pthread_create failed\n");
        (void)pthread_attr_destroy(&attr);
        return -1;
    }

    if (name != NULL) {
        (void)pthread_setname_np(thread->thread, name);
    }

    (void)pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    (void)pthread_attr_destroy(&attr);
    return 0;
}

int SemInit(MemSemaphore *sem, unsigned int init)
{
    int rc;

    rc = sem_init(&sem->sem, 0, init);
    if (rc < 0) {
        MEM_ERROR("%s failed, errno: %d\n", __func__, errno);
        return -1;
    }
    return 0;
}

int SemPost(MemSemaphore *sem)
{
    int rc;

    rc = sem_post(&sem->sem);
    if (rc < 0) {
        MEM_ERROR("%s failed, errno: %d\n", __func__, errno);
        return -1;
    }
    return rc;
}

int SemTimeWait(MemSemaphore *sem, int ms)
{
#define MS_TO_NS_RATE 1000000
#define MS_TO_US_RATE 1000
    int rc;
    struct timeval tv;
    struct timespec ts;

    if (gettimeofday(&tv, NULL) == -1) {
        MEM_ERROR("%s failed, errno: %d\n", __func__, errno);
        return -1;
    }
    ts.tv_nsec = tv.tv_usec * MS_TO_US_RATE + (ms % MS_TO_US_RATE) * MS_TO_NS_RATE;
    ts.tv_sec = tv.tv_sec + ms / MS_TO_US_RATE;
    if (ts.tv_nsec >= S_TO_NS_RATE) {
        ts.tv_sec++;
        ts.tv_nsec -= S_TO_NS_RATE;
    }

    do {
        rc = sem_timedwait(&sem->sem, &ts);
    } while (rc == -1 && errno == EINTR);
    if (rc == -1 && errno == ETIMEDOUT) {
        return -1;
    }

    return 0;
}

int SemDestroy(MemSemaphore *sem)
{
    int rc;

    rc = sem_destroy(&sem->sem);
    if (rc < 0) {
        MEM_ERROR("%s failed, errno: %d\n", __func__, errno);
        return -1;
    }
    return rc;
}

/*
 * The string is like {'MEMORY': {'total': '268876.26', 'used': '2986.73', 'usage': '1.11',
 * 'free': '268494.32', 'buffer': '14.24', 'cached': '158.01',
 * 'swap': [{'swaptotal': '1024.00', 'swapused': '0.00', 'swapfree': '1024.00'}]}}
 */
#define LINE_MAX_CHAR 1024

int GetFloatValueFromJsonString(const char *str, const char *key, float *value)
{
#define PREFIX_LEN 2
    char *keyBegin = NULL;
    char *keyEnd = NULL;
    char strTemp[LINE_MAX_CHAR] = { 0 };
    char keyTemp[LINE_MAX_CHAR] = { 0 };
    size_t i = 0;
    size_t j = 0;
    size_t len;

    if (!str || !key || strlen(str) <= strlen(key) || strlen(str) >= LINE_MAX_CHAR) {
        return -1;
    }

    len = strlen(str);
    for (i = 0; i < len; i++) {
        if (str[i] != ' ') {
            strTemp[j] = str[i];
            j++;
        }
    }

    strTemp[j] = '\0';
    if (snprintf_s(keyTemp, LINE_MAX_CHAR, LINE_MAX_CHAR - 1, "'%s'", key) < 0) {
        return -1;
    }

    keyBegin = strstr(strTemp, keyTemp);
    if (keyBegin == NULL) {
        MEM_ERROR("Try to get '%s' from wrong format of '%s'", key, str);
        return -1;
    }

    keyBegin += strlen(keyTemp); /* Jump the key */
    if (strncmp(keyBegin, ":\'", PREFIX_LEN) != 0) {
        MEM_ERROR("Try to get '%s' from wrong format of '%s'", key, str);
        return -1;
    }
    keyBegin += PREFIX_LEN; /* Jump ":'" */

    *value = strtof(keyBegin, &keyEnd);
    if (*keyEnd != '\'') { /* Check again */
        MEM_ERROR("Wrong format of '%s'", str);
        return -1;
    }

    return 0;
}

int WriteValueToLinuxProc(char *path, int value)
{
    char cmd[MAX_MONITORCOMMAND] = { 0 };
    int ret;
    int rc;
    rc = snprintf_s(cmd, sizeof(cmd), sizeof(cmd) - 1, "%d", value);
    if (rc < 0) {
        MEM_ERROR("Failed to print cmd");
        return -1;
    }

    /* write to file, retry 10 times if failed */
    ret = WriteStringToFile(path, cmd, 10);
    if (ret >= 0) {
        MEM_INFO("Successful while writting %d to %s", value, path);
    }
    return ret;
}

int STRPREFIX(const char *a, const char *b)
{
    return (strncmp(a, b, strlen(b)) == 0);
}

int STREQ(const char *a, const char *b)
{
    return (strcmp(a, b) == 0);
}

uint64_t MIN(uint64_t a, uint64_t b)
{
    return (a < b) ? a : b;
}

int64_t MAX(int64_t a, int64_t b)
{
    return (a > b) ? a : b;
}
