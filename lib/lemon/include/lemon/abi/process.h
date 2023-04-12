#ifndef LEMON_SYS_ABI_PROCESS_H
#define LEMON_SYS_ABI_PROCESS_H

#include <stdint.h>

#include "types.h"

typedef struct LemonProcessInfo {
    pid_t pid; // Process ID

    uint32_t threadCount; // Process Thread Count

    int32_t uid; // User ID
    int32_t gid; // Group ID

    uint8_t state; // Process State

    char name[256]; // Process Name

    uint64_t runningTime; // Amount of time in seconds that the process has been running
    uint64_t activeUs;
    bool isCPUIdle = false; // Whether or not the process is an idle process

    uint64_t usedMem; // Used memory in KB
} lemon_process_info_t;

// Create a process as a fork of the current process
#define LE_PROCESS_FORK 1
// Set the resulting handle as close on exec
#define LE_PROCESS_CLOEXEC 2
// Do not kill this process when it is closed
#define LE_PROCESS_DETACH 4
// Return a PID instea of a handle (LE_PROCESS_DETACH must be set)
#define LE_PROCESS_PID 8

// Cap the maximum length of any one argument passed to sys_execve
// Prevents the kernel getting caught up trying to find the end of a dodgy string passed by the user
#define SYS_ARGLEN_MAX (0x100000)
#define SYS_ARGC_MAX (0x1000) 

#endif // LEMON_SYS_ABI_PROCESS_H
