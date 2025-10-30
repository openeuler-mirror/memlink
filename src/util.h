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
 * Description: header file of memlink util
 * Author: Liang Zhang
 * Create: 2024-10-8
 */

#ifndef MEMLINK_UTIL_H
#define MEMLINK_UTIL_H

#include <securec.h>
#include <semaphore.h>
#ifndef STDINT_H
#define STDINT_H
#include <stdint.h>
#endif

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif

#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#elif EXIT_FAILURE != 1
#undef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

#ifdef STATIC
#undef STATIC
#endif

#ifdef CUNIT
#define STATIC
#define FALSE 0
#define TRUE  1
#else
#define STATIC static
#endif

#define KBTOGB          20
#define KBTOMB          10

int STRPREFIX(const char *a, const char *b);
int STREQ(const char *a, const char *b);
uint64_t MIN(uint64_t a, uint64_t b);
int64_t MAX(int64_t a, int64_t b);

#define MAX_MONITORCOMMAND      256
#define DECIMAL 10


/**
 * ATTRIBUTE_UNUSED:
 *
 * Macro to flag consciously unused parameters to functions
 */
#ifndef ATTRIBUTE_UNUSED
#define ATTRIBUTE_UNUSED __attribute__((__unused__))
#endif

/* Mutex */
typedef struct _MemlinkMutex MemlinkMutex;
typedef MemlinkMutex *MemMutexPtr;
struct _MemlinkMutex {
    pthread_mutex_t lock;
};

struct MemlinkdThread {
    pthread_t thread;
};

typedef struct MemlinkdThread MemlinkdThread;

int MutexInit(MemMutexPtr m);
void MutexDestroy(MemMutexPtr m);
void MutexLock(MemMutexPtr m);
void MutexUnlock(MemMutexPtr m);
int MutexTryLock(MemMutexPtr m);
int ThreadCreate(MemlinkdThread *thread, const char *name,
                 void *(*startRoutine)(void *),
                 void *arg, int mode);
struct MemSemaphore {
    sem_t sem;
};

typedef struct MemSemaphore MemSemaphore;

int SemInit(MemSemaphore *sem, unsigned int init);
int SemPost(MemSemaphore *sem);
int SemTimeWait(MemSemaphore *sem, int ms);
int SemDestroy(MemSemaphore *sem);

int CopyString(char **dst, const char *src);
int CheckVMHasPassthroughDev(const char *vmName);
int CheckVMHasHugePage(const char *vmName);
int CheckVMIsWindows(const char *vmName);
int ReadCgroupPathU64(const char *path, const uint64_t *val);
int WriteCgroupPathU64(const char *path, uint64_t val, int retryCount);
int WriteCgroupPathS64(const char *path, int64_t val, int retryCount);
int GetQEMUMemoryUsage(const char *vmName, uint64_t *usage);
/* others */
void SleepApprox(int seconds);
int WriteValueToLinuxProc(char *path, int value);
int GetFloatValueFromJsonString(const char *str, const char *key, float *value);
int WriteStringToFile(const char *path, const char *string, int retryCount);
int GetQEMUPidByVMName(const char *vmName);

/* host memory state */
typedef enum {
    HOST_MEM_INVALID_STATE = -1,
    HOST_MEM_EXETREME_LOW_STATE = 1,
    HOST_MEM_HARD_LOW_STATE = 2,
    HOST_MEM_SOFT_LOW_STATE = 3,
    HOST_MEM_MID_STATE = 4,
    HOST_MEM_HIGH_STATE = 5,
} HostMemoryState;

#define MEMLINK_THREAD_JOINABLE 0
#define MEMLINK_THREAD_DETACHED 1

/* key path */
#define LIBVIRT_CONN_URL    "qemu:///system"
#define MEMLINKD_CONF_FILE     "/etc/memlinkd.conf"
#define MEMLINKD_TMP_CONF_FILE "/etc/tmp-memlinkd.conf"
#define MEMLINKD_PID_FILE      "/var/run/memlinkd.pid"

#ifdef FOR_AARCH64
#define SMP_MB() ({ asm volatile("dmb ish" ::: "memory"); (void)0; })
#define SMP_RMB() ({ asm volatile("dmb ishld" ::: "memory"); (void)0; })
#define SMP_WMB() ({ asm volatile("dmb ishst" ::: "memory"); (void)0; })
#else
#define SMP_MB() ({ asm volatile("mfence" ::: "memory"); (void)0; })
#define SMP_RMB() ({ asm volatile("lfence" ::: "memory"); (void)0; })
#define SMP_WMB() ({ asm volatile("sfence" ::: "memory"); (void)0; })
#endif
/* __sync_lock_test_and_set() is documented to be an acquire barrier only.  */
#define ATOMIC_XCHG(ptr, i) (SMP_MB(), __sync_lock_test_and_set(ptr, i))

#define ATOMIC_READ__NOCHECK(ptr) \
    __atomic_load_n(ptr, __ATOMIC_RELAXED)

#define ATOMIC_READ(ptr)           \
    ({                             \
        ATOMIC_READ__NOCHECK(ptr); \
    })

#define ATOMIC_SET__NOCHECK(ptr, i) \
    __atomic_store_n(ptr, i, __ATOMIC_RELAXED)

#define ATOMIC_SET(ptr, i)           \
    do {                             \
        ATOMIC_SET__NOCHECK(ptr, i); \
    } while (0)

#define ATOMIC_ADD_FETCH__NOCHECK(ptr, i) \
    __atomic_add_fetch(ptr, i, __ATOMIC_RELAXED)

#define ATOMIC_ADD_FETCH(ptr, i)           \
    do {                             \
        ATOMIC_ADD_FETCH__NOCHECK(ptr, i); \
    } while (0)

#define ATOMIC_SUB_FETCH__NOCHECK(ptr, i) \
    __atomic_sub_fetch(ptr, i, __ATOMIC_RELAXED)

#endif
