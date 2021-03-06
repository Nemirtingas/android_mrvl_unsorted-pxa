/****************************************************************************
*
*    Copyright (c) 2005 - 2015 by Vivante Corp.  All rights reserved.
*
*    The material in this file is confidential and contains trade secrets
*    of Vivante Corporation. This is proprietary information owned by
*    Vivante Corporation. No part of this work may be disclosed,
*    reproduced, copied, transmitted, or used in any way for any purpose,
*    without the express written permission of Vivante Corporation.
*
*****************************************************************************/


/**
**  @file
**  OS object for hal user layers.
**
*/

#include "gc_hal_user_linux.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <dirent.h>

#include <signal.h>
#include <asm/sigcontext.h>       /* for sigcontext */
#ifdef ANDROID
#include <elf.h>
#include <cutils/properties.h>
#endif

#define _GC_OBJ_ZONE    gcvZONE_OS

char const * const GALDeviceName[] =
{
    "/dev/galcore",
    "/dev/graphics/galcore"
};

#define MAX_RETRY_IOCTL_TIMES 10000

/*
** GCC supports sync_* built-in functions since 4.1.2
** For those gcc variation whose verion is newer than 4.1.2 but
** still doesn't support sync_*, build driver with
** gcdBUILTIN_ATOMIC_FUNCTIONS = 0 to override version check.
*/
#ifndef gcdBUILTIN_ATOMIC_FUNCTIONS
#if (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__ >= 40102)
#define gcdBUILTIN_ATOMIC_FUNCTIONS 1
#else
#define gcdBUILTIN_ATOMIC_FUNCTIONS 0
#endif
#endif

#ifndef gcdBUILT_FOR_VALGRIND
#define gcdBUILT_FOR_VALGRIND 0
#endif

#define gcmGETPROCESSID() \
    getpid()

#ifdef ANDROID
#define gcmGETTHREADID() \
    (gctUINT32) gettid()
#else
long int syscall(long int number, ...);
#   define gcmGETTHREADID() \
        syscall(SYS_gettid)
#endif

/*******************************************************************************
***** Version Signature *******************************************************/

#ifdef ANDROID
const char * _GAL_PLATFORM = "\n\0$PLATFORM$Android$\n";
#else
const char * _GAL_PLATFORM = "\n\0$PLATFORM$Linux$\n";
#endif

/******************************************************************************\
***************************** gcoOS Object Structure ***************************
\******************************************************************************/

typedef struct _gcsDRIVER_ARGS
{
    gctUINT64   InputBuffer;
    gctUINT64   InputBufferSize;
    gctUINT64   OutputBuffer;
    gctUINT64   OutputBufferSize;
}
gcsDRIVER_ARGS;

struct _gcoOS
{
    /* Object. */
    gcsOBJECT               object;

    /* Context. */
    gctPOINTER              context;

    /* Heap. */
    gcoHEAP                 heap;

    /* Base address. */
    gctUINT32               baseAddress;

#if VIVANTE_PROFILER
    gctUINT64               startTick;
#endif

    /* Handle to the device. */
    int                     device;

#if VIVANTE_PROFILER
    gctUINT32               allocCount;
    gctSIZE_T               allocSize;
    gctSIZE_T               maxAllocSize;
    gctUINT32               freeCount;
    gctSIZE_T               freeSize;

#if gcdGC355_MEM_PRINT
    /* For a single collection. */
    gctINT32                oneSize;
    gctBOOL                 oneRecording;
#endif
#endif
};

/******************************************************************************\
*********************************** Globals ************************************
\******************************************************************************/

static pthread_key_t gcProcessKey;

gcsPLS gcPLS = gcPLS_INITIALIZER;

/******************************************************************************\
****************************** Internal Functions ******************************
\******************************************************************************/
#if gcmIS_DEBUG(gcdDEBUG_TRACE) || gcdGC355_MEM_PRINT
static void _ReportDB(
    void
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcoOS_ZeroMemory(&iface, sizeof(iface));

    iface.command = gcvHAL_DATABASE;
    iface.u.Database.processID = (gctUINT32)(gctUINTPTR_T)gcoOS_GetCurrentProcessID();
    iface.u.Database.validProcessID = gcvTRUE;

    /* Call kernel service. */
    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

    if ((iface.u.Database.vidMem.counters.bytes     != 0) ||
        (iface.u.Database.nonPaged.counters.bytes   != 0) ||
        (iface.u.Database.contiguous.counters.bytes != 0))
    {
        gcmTRACE(gcvLEVEL_ERROR, "\n");
        gcmTRACE(gcvLEVEL_ERROR, "******* MEMORY LEAKS DETECTED *******\n");
    }

    if (iface.u.Database.vidMem.counters.bytes != 0)
    {
        gcmTRACE(gcvLEVEL_ERROR, "\n");
        gcmTRACE(gcvLEVEL_ERROR, "vidMem.bytes      = %d\n", iface.u.Database.vidMem.counters.bytes);
        gcmTRACE(gcvLEVEL_ERROR, "vidMem.maxBytes   = %d\n", iface.u.Database.vidMem.counters.maxBytes);
        gcmTRACE(gcvLEVEL_ERROR, "vidMem.totalBytes = %d\n", iface.u.Database.vidMem.counters.totalBytes);
    }

    if (iface.u.Database.nonPaged.counters.bytes != 0)
    {
        gcmTRACE(gcvLEVEL_ERROR, "\n");
        gcmTRACE(gcvLEVEL_ERROR, "nonPaged.bytes      = %d\n", iface.u.Database.nonPaged.counters.bytes);
        gcmTRACE(gcvLEVEL_ERROR, "nonPaged.maxBytes   = %d\n", iface.u.Database.nonPaged.counters.maxBytes);
        gcmTRACE(gcvLEVEL_ERROR, "nonPaged.totalBytes = %d\n", iface.u.Database.nonPaged.counters.totalBytes);
    }

    if (iface.u.Database.contiguous.counters.bytes != 0)
    {
        gcmTRACE(gcvLEVEL_ERROR, "\n");
        gcmTRACE(gcvLEVEL_ERROR, "contiguous.bytes      = %d\n", iface.u.Database.contiguous.counters.bytes);
        gcmTRACE(gcvLEVEL_ERROR, "contiguous.maxBytes   = %d\n", iface.u.Database.contiguous.counters.maxBytes);
        gcmTRACE(gcvLEVEL_ERROR, "contiguous.totalBytes = %d\n", iface.u.Database.contiguous.counters.totalBytes);
    }

    gcmPRINT("05) Video memory - current: %lld \n", iface.u.Database.vidMem.counters.bytes);
    gcmPRINT("06) Video memory - maximum: %lld \n", iface.u.Database.vidMem.counters.maxBytes);
    gcmPRINT("07) Video memory - total: %lld \n", iface.u.Database.vidMem.counters.totalBytes);
OnError:;
}
#endif

#if gcdDUMP || gcdDUMP_API || gcdDUMP_2D
static void
_SetDumpFileInfo(
    )
{
    gceSTATUS status = gcvSTATUS_TRUE;

#if gcdDUMP || gcdDUMP_2D
    #define DUMP_FILE_PREFIX   "hal"
#else
    #define DUMP_FILE_PREFIX   "api"
#endif

#if defined(ANDROID)
    /* The default key of dump key for match use. Only useful in android. Do not change it! */
    #define DEFAULT_DUMP_KEY        "allprocesses"

    if (gcmIS_SUCCESS(gcoOS_StrCmp(gcdDUMP_KEY, DEFAULT_DUMP_KEY)))
        status = gcvSTATUS_TRUE;
    else
        status = gcoOS_DetectProcessByName(gcdDUMP_KEY);
#endif

    if (status == gcvSTATUS_TRUE)
    {
        char dump_file[128];
        gctUINT offset = 0;

        /* Customize filename as needed. */
        gcmVERIFY_OK(gcoOS_PrintStrSafe(dump_file,
                     gcmSIZEOF(dump_file),
                     &offset,
                     "%s%s_dump_pid-%d_tid-%d_%s.log",
                     gcdDUMP_PATH,
                     DUMP_FILE_PREFIX,
                     gcoOS_GetCurrentProcessID(),
                     gcmGETTHREADID(),
                     gcdDUMP_KEY));

        gcoOS_SetDebugFile(dump_file);
        gcoDUMP_SetDumpFlag(gcvTRUE);
    }
}
#endif

/******************************************************************************\
**************************** OS Construct/Destroy ******************************
\******************************************************************************/
static gceSTATUS
_DestroyOs(
    IN gcoOS Os
    )
{
    gceSTATUS status;

    gcmPROFILE_DECLARE_ONLY(gctUINT64 ticks);

    gcmHEADER();

    if (gcPLS.os != gcvNULL)
    {

        gcmPROFILE_QUERY(gcPLS.os->startTick, ticks);
        gcmPROFILE_ONLY(gcmTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_OS,
                                      "Total ticks during gcoOS life: %llu",
                                      ticks));

        if (gcPLS.os->heap != gcvNULL)
        {
            gcoHEAP heap = gcPLS.os->heap;

#if VIVANTE_PROFILER
            /* End profiler. */
            gcoHEAP_ProfileEnd(heap, "gcoOS_HEAP");
#endif

            /* Mark the heap as gone. */
            gcPLS.os->heap = gcvNULL;

            /* Destroy the heap. */
            gcmONERROR(gcoHEAP_Destroy(heap));
        }

        /* Close the handle to the kernel service. */
        if (gcPLS.os->device != -1)
        {
#if gcmIS_DEBUG(gcdDEBUG_TRACE) || gcdGC355_MEM_PRINT
            _ReportDB();
#endif

            close(gcPLS.os->device);
            gcPLS.os->device = -1;
        }

#if VIVANTE_PROFILER && gcdGC355_MEM_PRINT
        /* End profiler. */
        gcoOS_ProfileEnd(gcPLS.os, gcvNULL);
#endif

        /* Mark the gcoOS object as unknown. */
        gcPLS.os->object.type = gcvOBJ_UNKNOWN;

        /* Free the gcoOS structure. */
        free(gcPLS.os);

        /* Reset PLS object. */
        gcPLS.os = gcvNULL;
    }

    /* Success. */
    gcmFOOTER_KILL();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

static gceSTATUS
_ConstructOs(
    IN gctPOINTER Context,
    OUT gcoOS * Os
    )
{
    gcoOS os = gcPLS.os;
    gcsHAL_INTERFACE iface;
    gceSTATUS status;
    gctUINT i;

    gcmPROFILE_DECLARE_ONLY(gctUINT64 freq);

    gcmHEADER_ARG("Context=0x%x", Context);

    if (os == gcvNULL)
    {
        /* Allocate the gcoOS structure. */
        os = malloc(gcmSIZEOF(struct _gcoOS));
        if (os == gcvNULL)
        {
            gcmONERROR(gcvSTATUS_OUT_OF_MEMORY);
        }

        /* Initialize the gcoOS object. */
        os->object.type = gcvOBJ_OS;
        os->context     =  Context;
        os->heap        =  gcvNULL;
        os->baseAddress =  0;
        os->device      = -1;

        /* Set the object pointer to PLS. */
        gcmASSERT(gcPLS.os == gcvNULL);
        gcPLS.os = os;

        /* Attempt to open the device. */
        for (i = 0; i < gcmCOUNTOF(GALDeviceName); i += 1)
        {
            gcmTRACE_ZONE(
                gcvLEVEL_VERBOSE, gcvZONE_OS,
                "%s(%d): Attempting to open device %s.",
                __FUNCTION__, __LINE__, GALDeviceName[i]
                );

            os->device = open(GALDeviceName[i], O_RDWR);

            if (os->device != -1)
            {
                gcmTRACE_ZONE(
                    gcvLEVEL_VERBOSE, gcvZONE_OS,
                    "%s(%d): Opened device %s.",
                    __FUNCTION__, __LINE__, GALDeviceName[i]
                    );

                break;
            }

            gcmTRACE_ZONE(
                gcvLEVEL_VERBOSE, gcvZONE_OS,
                "%s(%d): Failed to open device %s, errno=%s.",
                __FUNCTION__, __LINE__, GALDeviceName[i], strerror(errno)
                );
        }

        if (i == gcmCOUNTOF(GALDeviceName))
        {
            gcmTRACE(
                gcvLEVEL_ERROR,
                "%s(%d): Failed to open device, errno=%s.",
                __FUNCTION__, __LINE__, strerror(errno)
                );

            gcmONERROR(gcvSTATUS_DEVICE);
        }

        /* Construct heap. */
        status = gcoHEAP_Construct(gcvNULL, gcdHEAP_SIZE, &os->heap);

        if (gcmIS_ERROR(status))
        {
            gcmTRACE_ZONE(
                gcvLEVEL_WARNING, gcvZONE_OS,
                "%s(%d): Could not construct gcoHEAP (%d).",
                __FUNCTION__, __LINE__, status
                );

            os->heap = gcvNULL;
        }
#if VIVANTE_PROFILER
        else
        {
            /* Start profiler. */
            gcoHEAP_ProfileStart(os->heap);
        }
#endif

        /* Query base address. */
        iface.command = gcvHAL_GET_BASE_ADDRESS;

        /* Call kernel driver. */
        status = gcoOS_DeviceControl(
            gcvNULL,
            IOCTL_GCHAL_INTERFACE,
            &iface, gcmSIZEOF(iface),
            &iface, gcmSIZEOF(iface)
            );

        if (gcmIS_SUCCESS(status))
        {
            os->baseAddress = iface.u.GetBaseAddress.baseAddress;

            gcmTRACE_ZONE(
                gcvLEVEL_INFO, gcvZONE_OS,
                "%s(%d): baseAddress is 0x%08X.",
                __FUNCTION__, __LINE__, os->baseAddress
                );
        }
        else
        {
            gcmTRACE_ZONE(
                gcvLEVEL_WARNING, gcvZONE_OS,
                "%s(%d): Setting default baseAddress of 0.",
                __FUNCTION__, __LINE__
                );
        }

        /* Get profiler start tick. */
        gcmPROFILE_INIT(freq, os->startTick);
    }

#if VIVANTE_PROFILER
        /* Start profiler. */
    gcoOS_ProfileStart(os);
#endif

    /* Return pointer to the gcoOS object. */
    if (Os != gcvNULL)
    {
        *Os = os;
    }

    /* Success. */
    gcmFOOTER_ARG("*Os=0x%x", os);
    return gcvSTATUS_OK;

OnError:
    /* Roll back. */
    gcmVERIFY_OK(_DestroyOs(gcvNULL));
    gcmFOOTER();
    return status;
}

/******************************************************************************\
************************* Process/Thread Local Storage *************************
\******************************************************************************/

static void __attribute__((destructor)) _ModuleDestructor(void);

static gceSTATUS
_MapMemory(
    IN gctPHYS_ADDR Physical,
    IN gctSIZE_T NumberOfBytes,
    OUT gctPOINTER * Logical
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Physical=0x%x NumberOfBytes=%lu", Physical, NumberOfBytes);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(NumberOfBytes > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Logical != gcvNULL);

    /* Call kernel API to unmap the memory. */
    iface.command              = gcvHAL_MAP_MEMORY;
    iface.u.MapMemory.physical = gcmPTR2INT32(Physical);
    iface.u.MapMemory.bytes    = NumberOfBytes;

    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

    /* Return logical address. */
    *Logical = gcmUINT64_TO_PTR(iface.u.MapMemory.logical);

    /* Success. */
    gcmFOOTER_ARG("*Logical=0x%x", *Logical);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

static gceSTATUS
_UnmapMemory(
    IN gctPHYS_ADDR Physical,
    IN gctSIZE_T NumberOfBytes,
    IN gctPOINTER Logical
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Physical=0x%x NumberOfBytes=%lu Logical=0x%x",
                  Physical, NumberOfBytes, Logical);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(NumberOfBytes > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Logical != gcvNULL);

    /* Call kernel API to unmap the memory. */
    iface.command                = gcvHAL_UNMAP_MEMORY;
    iface.u.UnmapMemory.physical = gcmPTR2INT32(Physical);
    iface.u.UnmapMemory.bytes    = NumberOfBytes;
    iface.u.UnmapMemory.logical  = gcmPTR_TO_UINT64(Logical);

    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, sizeof(iface),
        &iface, sizeof(iface)
        ));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

static void
_OpenGalLib(
    gcsTLS_PTR TLS
    )
{
#if gcdSTATIC_LINK
    return;
#else
    gcsTLS_PTR  tls;
    gctSTRING   path;
    gctSTRING   env_path = gcvNULL;
    gctSTRING   one_env_path;
    gctSTRING   full_path = gcvNULL;
    gctINT32    len;
    gctHANDLE   handle = gcvNULL;

    gcmHEADER_ARG("TLS=0x%x", TLS);

    tls = (gcsTLS_PTR) TLS;
    gcmASSERT(tls != gcvNULL);

    do {
        gcoOS_GetEnv(gcvNULL, "LD_LIBRARY_PATH", &path);
        if (path != gcvNULL)
        {
            len = strlen(path) + 1;
            env_path = malloc(len);
            full_path = malloc(len+10);
            if (env_path == NULL || full_path == NULL)
            {
                break;
            }
            strncpy(env_path, path, len);
            one_env_path = strtok(env_path, ":");
            while (one_env_path != NULL)
            {
                snprintf(full_path, len+10, "%s/libGAL.so", one_env_path);
#if defined(ANDROID)
                handle = dlopen(full_path, RTLD_NOW);
#else
                handle = dlopen(full_path, RTLD_NOW | RTLD_NODELETE);
#endif
                if (handle != gcvNULL)
                {
                    break;
                }
                one_env_path = strtok(NULL, ":");
            }
        }
        if (handle == gcvNULL)
        {
#if defined(ANDROID)
            handle = dlopen("/system/lib/libGAL.so", RTLD_NOW);
#else
            handle = dlopen("/usr/lib/libGAL.so", RTLD_NOW | RTLD_NODELETE);
            if (handle == gcvNULL)
            {
                handle = dlopen("/lib/libGAL.so", RTLD_NOW | RTLD_NODELETE);
            }
#endif
        }

    } while (gcvFALSE);

    if (env_path != gcvNULL)
        free(env_path);
    if (full_path != gcvNULL)
        free(full_path);

    if (handle != gcvNULL)
    {
        tls->handle = handle;
    }

    gcmFOOTER_NO();
    return;
#endif
}

static void
_CloseGalLib(
    gcsTLS_PTR TLS
    )
{
    gcsTLS_PTR tls;

    gcmHEADER_ARG("TLS=0x%x", TLS);

    tls = (gcsTLS_PTR) TLS;
    gcmASSERT(tls != gcvNULL);

    if (tls->handle != gcvNULL)
    {
        gcoOS_FreeLibrary(gcvNULL, tls->handle);
        tls->handle = gcvNULL;
    }

    gcmFOOTER_NO();
}

static void
_PLSDestructor(
    void
    )
{
    gcmHEADER();

    if (gcPLS.destructor != gcvNULL)
    {
        gcPLS.destructor(&gcPLS);
        gcPLS.destructor = gcvNULL;
    }

    if (gcPLS.contiguousLogical != gcvNULL)
    {
        gcmVERIFY_OK(_UnmapMemory(
            gcPLS.contiguousPhysical,
            gcPLS.contiguousSize,
            gcPLS.contiguousLogical
            ));

        gcPLS.contiguousLogical = gcvNULL;
    }

    if (gcPLS.externalLogical != gcvNULL)
    {
        gcmVERIFY_OK(_UnmapMemory(
            gcPLS.externalPhysical,
            gcPLS.externalSize,
            gcPLS.externalLogical
            ));

        gcPLS.externalLogical = gcvNULL;
    }

    if (gcPLS.internalLogical != gcvNULL)
    {
        gcmVERIFY_OK(_UnmapMemory(
            gcPLS.internalPhysical,
            gcPLS.internalSize,
            gcPLS.internalLogical
            ));

        gcPLS.internalLogical = gcvNULL;
    }

    gcmVERIFY_OK(gcoOS_DeleteMutex(gcPLS.os, gcPLS.accessLock));
    gcPLS.accessLock = gcvNULL;

    gcmVERIFY_OK(gcoOS_AtomDestroy(gcPLS.os, gcPLS.reference));
    gcPLS.reference = gcvNULL;

    if (gcPLS.hal != gcvNULL)
    {
        gcmVERIFY_OK(gcoHAL_DestroyEx(gcPLS.hal));
        gcPLS.hal = gcvNULL;
    }

    gcmVERIFY_OK(_DestroyOs(gcPLS.os));

    pthread_key_delete(gcProcessKey);

    gcmFOOTER_NO();
}

static void
_TLSDestructor(
    gctPOINTER TLS
    )
{
    gcsTLS_PTR tls;
    gctINT reference = 0;
#if gcdMULTI_GPU_AFFINITY
    gctUINT api;
#endif

    gcmHEADER_ARG("TLS=0x%x", TLS);

    tls = (gcsTLS_PTR) TLS;
    gcmASSERT(tls != gcvNULL);

    pthread_setspecific(gcProcessKey, tls);

    if (tls->copied)
    {
        /* Zero out all information if this TLS was copied. */
        gcoOS_ZeroMemory(tls, gcmSIZEOF(gcsTLS));
    }

    if (tls->destructor != gcvNULL)
    {
        tls->destructor(tls);
        tls->destructor = gcvNULL;
    }

#if gcdENABLE_3D
    /* DON'T destroy tls->engine3D, which belongs to app context
    */
#endif

#if gcdENABLE_2D
    if (tls->engine2D != gcvNULL)
    {
        gcmVERIFY_OK(gco2D_Destroy(tls->engine2D));
        tls->engine2D = gcvNULL;
    }
#endif

    if (tls->defaultHardware != gcvNULL)
    {
        gceHARDWARE_TYPE type = tls->currentType;
#if gcdMULTI_GPU_AFFINITY
        gcmVERIFY_OK(gcoHARDWARE_GetCurrentAPI(&api));

        if (api == gcvAPI_OPENCL)
        {
            tls->currentType = gcvHARDWARE_OCL;
        }
        else
#else
        {
            tls->currentType = gcvHARDWARE_3D;
        }
#endif
        gcmTRACE_ZONE(
            gcvLEVEL_VERBOSE, gcvZONE_HARDWARE,
            "%s(%d): destroying default hardware object 0x%08X.",
            __FUNCTION__, __LINE__, tls->defaultHardware
            );

        gcmVERIFY_OK(gcoHARDWARE_Destroy(tls->defaultHardware, gcvTRUE));

        tls->defaultHardware = gcvNULL;
        tls->currentHardware = gcvNULL;
        tls->currentType = type;
    }

    if (tls->hardware2D != gcvNULL)
    {
        gceHARDWARE_TYPE type = tls->currentType;
        tls->currentType = gcvHARDWARE_2D;

        gcmTRACE_ZONE(
            gcvLEVEL_VERBOSE, gcvZONE_HARDWARE,
            "%s(%d): destroying hardware object 0x%08X.",
            __FUNCTION__, __LINE__, tls->hardware2D
            );

#if gcdENABLE_2D
        gcmVERIFY_OK(gcoHARDWARE_Destroy(tls->hardware2D, gcvTRUE));
#endif
        tls->hardware2D = gcvNULL;
        tls->currentType = type;
    }

#if gcdENABLE_VG
        if (tls->engineVG != gcvNULL)
        {
#if GC355_PROFILER
            gcmVERIFY_OK(gcoVG_Destroy(tls->engineVG,gcvNULL,0,0,0));
#else
            gcmVERIFY_OK(gcoVG_Destroy(tls->engineVG));
#endif
            tls->engineVG = gcvNULL;
        }

        if (tls->vg != gcvNULL)
        {
            gceHARDWARE_TYPE type = tls->currentType;
            tls->currentType = gcvHARDWARE_VG;

            gcmTRACE_ZONE(
                gcvLEVEL_VERBOSE, gcvZONE_HARDWARE,
                "%s(%d): destroying hardware object 0x%08X.",
                __FUNCTION__, __LINE__, tls->vg
                );

            gcmVERIFY_OK(gcoVGHARDWARE_Destroy(tls->vg));
            tls->vg = gcvNULL;
            tls->currentType = type;

        }
#endif

    if (gcPLS.threadID && gcPLS.threadID != gcmGETTHREADID() && !gcPLS.exiting)
    {
        _CloseGalLib(tls);
    }

    if (gcPLS.reference != gcvNULL)
    {
        /* Decrement the reference. */
        gcmVERIFY_OK(gcoOS_AtomDecrement(gcPLS.os,
                                         gcPLS.reference,
                                         &reference));

        /* Check if there are still more references. */
        if (reference ==  1)
        {
            /* If all threads exit, destruct PLS */
            _PLSDestructor();
        }
    }

    gcmVERIFY_OK(gcoOS_FreeMemory(gcvNULL, tls));

    pthread_setspecific(gcProcessKey, gcvNULL);

    gcmFOOTER_NO();
}

static void
_InitializeProcess(
    void
    )
{
    /* Install thread destructor. */
    pthread_key_create(&gcProcessKey, _TLSDestructor);
}

void _ResetPLS(void)
{
    gcPLS.os = gcvNULL;
    gcPLS.hal = gcvNULL;
    gcPLS.contiguousSize = 0;
    gcPLS.processID = 0;
    gcPLS.reference = 0;
#if gcdENABLE_3D
	gcPLS.patchID   = gcvPATCH_NOTINIT;
#endif
}

void _AtForkChild(
    void
    )
{
    _ResetPLS();

    pthread_key_delete(gcProcessKey);
    pthread_key_create(&gcProcessKey, _TLSDestructor);
}

static void _ModuleConstructor(
    void
    )
{
    gceSTATUS status;
    int result;
    static pthread_once_t onceControl = PTHREAD_ONCE_INIT;

    gcmHEADER();

    /* Each process gets its own objects. */
    gcmASSERT(gcPLS.os  == gcvNULL);
    gcmASSERT(gcPLS.hal == gcvNULL);

    gcmASSERT(gcPLS.internalLogical   == gcvNULL);
    gcmASSERT(gcPLS.externalLogical   == gcvNULL);
    gcmASSERT(gcPLS.contiguousLogical == gcvNULL);

#if (defined(ANDROID) && (ANDROID_SDK_VERSION > 16)) || (!defined(ANDROID) && !defined(NO_PTHREAD_AT_FORK))
    result = pthread_atfork(gcvNULL, gcvNULL, _AtForkChild);

    if (result != 0)
    {
        gcmTRACE(
            gcvLEVEL_ERROR,
            "%s(%d): pthread_atfork returned %d",
            __FUNCTION__, __LINE__, result
            );

        gcmONERROR(gcvSTATUS_OUT_OF_RESOURCES);
    }
#endif


    /* Call _InitializeProcess function only one time for the process. */
    result = pthread_once(&onceControl, _InitializeProcess);

    if (result != 0)
    {
        gcmTRACE(
            gcvLEVEL_ERROR,
            "%s(%d): pthread_once returned %d",
            __FUNCTION__, __LINE__, result
            );

        gcmONERROR(gcvSTATUS_OUT_OF_MEMORY);
    }

    /* Construct OS object. */
    gcmONERROR(_ConstructOs(gcvNULL, gcvNULL));

    /* Construct gcoHAL object. */
    gcmONERROR(gcoHAL_ConstructEx(gcvNULL, gcvNULL, &gcPLS.hal));

    /* Construct PLS reference atom. */
    gcmONERROR(gcoOS_AtomConstruct(gcPLS.os, &gcPLS.reference));

    /* Increment PLS reference for main thread. */
    gcmONERROR(gcoOS_AtomIncrement(gcPLS.os, gcPLS.reference, gcvNULL));

    /* Query the video memory sizes. */
    gcmONERROR(gcoOS_QueryVideoMemory(
        gcPLS.os,
        &gcPLS.internalPhysical,
        &gcPLS.internalSize,
        &gcPLS.externalPhysical,
        &gcPLS.externalSize,
        &gcPLS.contiguousPhysical,
        &gcPLS.contiguousSize
        ));

    /* Map internal video memory. */
    if (gcPLS.internalSize != 0)
    {
        gcmONERROR(_MapMemory(
             gcPLS.internalPhysical,
             gcPLS.internalSize,
            &gcPLS.internalLogical
            ));
    }

    /* Map external video memory. */
    if (gcPLS.externalSize != 0)
    {
        gcmONERROR(_MapMemory(
             gcPLS.externalPhysical,
             gcPLS.externalSize,
            &gcPLS.externalLogical
            ));
    }

    /* Map contiguous video memory. */
    if (gcPLS.contiguousSize != 0)
    {
        gcmONERROR(_MapMemory(
             gcPLS.contiguousPhysical,
             gcPLS.contiguousSize,
            &gcPLS.contiguousLogical
            ));
    }

    /* Record the process and thread that calling this constructor function */
    gcPLS.processID = gcmGETPROCESSID();
    gcPLS.threadID = gcmGETTHREADID();

    /* Construct access lock */
    gcmONERROR(gcoOS_CreateMutex(gcPLS.os, &gcPLS.accessLock));

#if gcdDUMP_2D
    gcmONERROR(gcoOS_CreateMutex(gcPLS.os, &dumpMemInfoListMutex));
#endif

    gcmFOOTER_ARG(
        "gcPLS.os=0x%08X, gcPLS.hal=0x%08X"
        " internal=0x%08X external=0x%08X contiguous=0x%08X",
        gcPLS.os, gcPLS.hal,
        gcPLS.internalLogical, gcPLS.externalLogical, gcPLS.contiguousLogical
        );

    return;

OnError:

    if (gcPLS.accessLock != gcvNULL)
    {
        /* Destroy access lock */
        gcmVERIFY_OK(gcoOS_DeleteMutex(gcPLS.os, gcPLS.accessLock));
    }

    if (gcPLS.reference != gcvNULL)
    {
        /* Destroy the reference. */
        gcmVERIFY_OK(gcoOS_AtomDestroy(gcPLS.os, gcPLS.reference));
    }

    gcmFOOTER();
}

static void
_ModuleDestructor(
    void
    )
{
    gctINT reference = 0;
    gcmHEADER();

    if (gcPLS.reference != gcvNULL)
    {
        gcPLS.exiting = gcvTRUE;

        /* Decrement the reference for main thread. */
        gcmVERIFY_OK(gcoOS_AtomDecrement(gcPLS.os,
                                         gcPLS.reference,
                                         &reference));

        if (reference == 1)
        {
            /* If all threads exit, destruct PLS. */
            _PLSDestructor();
        }
        else
        {
            gcoOS_FreeThreadData();
        }
    }

    gcmFOOTER_NO();
}

/******************************************************************************\
********************************* gcoOS API Code *******************************
\******************************************************************************/

/*******************************************************************************
 **
 ** gcoOS_GetPLSValue
 **
 ** Get value associated with the given key.
 **
 ** INPUT:
 **
 **     gcePLS_VALUE key
 **         key to look up.
 **
 ** OUTPUT:
 **
 **     None
 **
 ** RETURN:
 **
 **     gctPOINTER
 **         Pointer to object associated with key.
 */
gctPOINTER
gcoOS_GetPLSValue(
    IN gcePLS_VALUE key
    )
{
    switch (key)
    {
        case gcePLS_VALUE_EGL_DISPLAY_INFO :
            return gcPLS.eglDisplayInfo;

        case gcePLS_VALUE_EGL_SURFACE_INFO :
            return gcPLS.eglSurfaceInfo;

        case gcePLS_VALUE_EGL_CONFIG_FORMAT_INFO :
            return (gctPOINTER) gcPLS.eglConfigFormat;

        case gcePLS_VALUE_EGL_DESTRUCTOR_INFO :
            return (gctPOINTER) gcPLS.destructor;
    }

    return gcvNULL;
}

/*******************************************************************************
 **
 ** gcoOS_SetPLSValue
 **
 ** Associated object represented by 'value' with the given key.
 **
 ** INPUT:
 **
 **     gcePLS_VALUE key
 **         key to associate.
 **
 **     gctPOINTER value
 **         value to associate with key.
 **
 ** OUTPUT:
 **
 **     None
 **
 */
void
gcoOS_SetPLSValue(
    IN gcePLS_VALUE key,
    IN gctPOINTER value
    )
{
    switch (key)
    {
        case gcePLS_VALUE_EGL_DISPLAY_INFO :
            gcPLS.eglDisplayInfo = value;
            return;

        case gcePLS_VALUE_EGL_SURFACE_INFO :
            gcPLS.eglSurfaceInfo = value;
            return;

        case gcePLS_VALUE_EGL_CONFIG_FORMAT_INFO :
            gcPLS.eglConfigFormat = (gceSURF_FORMAT)(gctUINTPTR_T)value;
            return;

        case gcePLS_VALUE_EGL_DESTRUCTOR_INFO :
            gcPLS.destructor = (gctPLS_DESTRUCTOR) value;
            return;
    }
}

/*******************************************************************************
 **
 ** gcoOS_GetTLS
 **
 ** Get access to the thread local storage.
 **
 ** INPUT:
 **
 **     Nothing.
 **
 ** OUTPUT:
 **
 **     gcsTLS_PTR * TLS
 **         Pointer to a variable that will hold the pointer to the TLS.
 */
static pthread_mutex_t plsMutex = PTHREAD_MUTEX_INITIALIZER;

gceSTATUS
gcoOS_GetTLS(
    OUT gcsTLS_PTR * TLS
    )
{
    gceSTATUS status;
    gcsTLS_PTR tls;
    int res;

    gcmHEADER_ARG("TLS=%p", TLS);

    tls = (gcsTLS_PTR) pthread_getspecific(gcProcessKey);

    if (!gcPLS.processID)
    {
        pthread_mutex_lock(&plsMutex);

        if (!gcPLS.processID)
        {
            _ModuleConstructor();

            tls = NULL;
        }

        pthread_mutex_unlock(&plsMutex);
    }

    if (tls == NULL)
    {
        gcmONERROR(gcoOS_AllocateMemory(
            gcvNULL, gcmSIZEOF(gcsTLS), (gctPOINTER *) &tls
            ));

        gcoOS_ZeroMemory(
            tls, gcmSIZEOF(gcsTLS)
            );

        /* The default hardware type is 2D */
        tls->currentType = gcvHARDWARE_2D;

        res = pthread_setspecific(gcProcessKey, tls);

        if (res != 0)
        {
            gcmTRACE(
                gcvLEVEL_ERROR,
                "%s(%d): pthread_setspecific returned %d",
                __FUNCTION__, __LINE__, res
                );

            gcmONERROR(gcvSTATUS_GENERIC_IO);
        }

        if (gcPLS.threadID && gcPLS.threadID != gcmGETTHREADID())
            _OpenGalLib(tls);

        if (gcPLS.reference != gcvNULL)
        {
            /* Increment PLS reference. */
            gcmONERROR(gcoOS_AtomIncrement(gcPLS.os, gcPLS.reference, gcvNULL));
        }

#if gcdDUMP || gcdDUMP_API || gcdDUMP_2D
        _SetDumpFileInfo();
#endif
    }

    * TLS = tls;

    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (tls != gcvNULL)
    {
        gcmVERIFY_OK(gcoOS_FreeMemory(gcvNULL, (gctPOINTER) tls));
    }

    * TLS = gcvNULL;

    gcmFOOTER();
    return status;
}

/*
 *  gcoOS_CopyTLS
 *
 *  Copy the TLS from a source thread and mark this thread as a copied thread, so the destructor won't free the resources.
 *
 *  NOTE: Make sure the "source thread" doesn't get kiiled while this thread is running, since the objects will be taken away. This
 *  will be fixed in a future version of the HAL when reference counters will be used to keep track of object usage (automatic
 *  destruction).
 */
gceSTATUS gcoOS_CopyTLS(IN gcsTLS_PTR Source)
{
    gceSTATUS   status;
    gcsTLS_PTR  tls;

    gcmHEADER();

    /* Verify the arguyments. */
    gcmDEBUG_VERIFY_ARGUMENT(Source != gcvNULL);

    /* Get the thread specific data. */
    tls = pthread_getspecific(gcProcessKey);

    if (tls != gcvNULL)
    {
        /* We cannot copy if the TLS has already been initialized. */
        gcmONERROR(gcvSTATUS_INVALID_REQUEST);
    }

    /* Allocate memory for the TLS. */
    gcmONERROR(gcoOS_AllocateMemory(gcvNULL, gcmSIZEOF(gcsTLS), (gctPOINTER *) &tls));

    /* Set the thread specific data. */
    pthread_setspecific(gcProcessKey, tls);

    if (gcPLS.threadID && gcPLS.threadID != gcmGETTHREADID())
        _OpenGalLib(tls);

    if (gcPLS.reference != gcvNULL)
    {
        /* Increment PLS reference. */
        gcmONERROR(gcoOS_AtomIncrement(gcPLS.os, gcPLS.reference, gcvNULL));
    }

    /* Mark this TLS as copied. */
    tls->copied = gcvTRUE;

    /* Copy the TLS information. */
    tls->currentType    = Source->currentType;
    tls->defaultHardware = Source->defaultHardware;
    tls->currentHardware = gcvNULL;
    tls->hardware2D     = Source->hardware2D;
#if gcdENABLE_VG
    tls->vg             = Source->vg;
    tls->engineVG       = Source->engineVG;
#endif
    tls->context        = Source->context;
    tls->destructor     = gcvNULL;
#if gcdENABLE_3D
    tls->engine3D       = Source->engine3D;
#endif
#if gcdENABLE_2D
    tls->engine2D       = Source->engine2D;
#endif

#if gcdDUMP || gcdDUMP_API || gcdDUMP_2D
    _SetDumpFileInfo();
#endif

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}


/*******************************************************************************
**
**  gcoOS_LockPLS
**
**  Lock mutext before access PLS if needed
**
**  INPUT:
**
**      Nothing.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_LockPLS(
    void
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gcmHEADER();
    if (gcPLS.accessLock)
    {
        status = gcoOS_AcquireMutex(gcPLS.os, gcPLS.accessLock, gcvINFINITE);

    }
    gcmFOOTER_ARG("Lock PLS ret=%d", status);

    return status;
}

/*******************************************************************************
**
**  gcoOS_UnLockPLS
**
**  Release mutext after access PLS if needed
**
**  INPUT:
**
**      Nothing.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_UnLockPLS(
    void
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gcmHEADER();
    if (gcPLS.accessLock)
    {
        status = gcoOS_ReleaseMutex(gcPLS.os, gcPLS.accessLock);

    }
    gcmFOOTER_ARG("Release PLS ret=%d", status);

    return status;
}


/*******************************************************************************
**
**  gcoOS_FreeThreadData
**
**  Destroy the objects associated with the current thread.
**
**  INPUT:
**
**      Nothing.
**
**  OUTPUT:
**
**      Nothing.
*/
void
gcoOS_FreeThreadData(
    void
    )
{
    gcsTLS_PTR tls;

    tls = (gcsTLS_PTR) pthread_getspecific(gcProcessKey);

    if (tls != NULL)
    {
        if (gcPLS.processID != (gctUINT32)(gctUINTPTR_T)gcmGETPROCESSID())
        {
            /* This process is not the one called construct function.
             * It maybe created by fork or clone, just return in this case . */
            return;
        }

        _TLSDestructor((gctPOINTER) tls);
    }
}

/*******************************************************************************
 **
 ** gcoOS_Construct
 **
 ** Construct a new gcoOS object. Empty function only for compatibility.
 **
 ** INPUT:
 **
 **     gctPOINTER Context
 **         Pointer to an OS specific context.
 **
 ** OUTPUT:
 **
 **     Nothing.
 **
 */
gceSTATUS
gcoOS_Construct(
    IN gctPOINTER Context,
    OUT gcoOS * Os
    )
{
    gceSTATUS status;
    gcsTLS_PTR tls;

    gcmONERROR(gcoOS_GetTLS(&tls));

    /* Return gcoOS object for compatibility to prevent any failure in applications. */
    *Os = gcPLS.os;

    return gcvSTATUS_OK;

OnError:
    return status;
}

/*******************************************************************************
 **
 ** gcoOS_Destroy
 **
 ** Destroys an gcoOS object. Empty function only for compatibility.
 **
 ** ARGUMENTS:
 **
 **     gcoOS Os
 **         Pointer to the gcoOS object that needs to be destroyed.
 **
 ** OUTPUT:
 **
 **     Nothing.
 **
 */
gceSTATUS
gcoOS_Destroy(
    IN gcoOS Os
    )
{
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_QueryVideoMemory
**
**  Query the amount of video memory.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to a gcoOS object.
**
**  OUTPUT:
**
**      gctPHYS_ADDR * InternalAddress
**          Pointer to a variable that will hold the physical address of the
**          internal memory.  If 'InternalAddress' is gcvNULL, no information
**          about the internal memory will be returned.
**
**      gctSIZE_T * InternalSize
**          Pointer to a variable that will hold the size of the internal
**          memory.  'InternalSize' cannot be gcvNULL if 'InternalAddress' is
**          not gcvNULL.
**
**      gctPHYS_ADDR * ExternalAddress
**          Pointer to a variable that will hold the physical address of the
**          external memory.  If 'ExternalAddress' is gcvNULL, no information
**          about the external memory will be returned.
**
**      gctSIZE_T * ExternalSize
**          Pointer to a variable that will hold the size of the external
**          memory.  'ExternalSize' cannot be gcvNULL if 'ExternalAddress' is
**          not gcvNULL.
**
**      gctPHYS_ADDR * ContiguousAddress
**          Pointer to a variable that will hold the physical address of the
**          contiguous memory.  If 'ContiguousAddress' is gcvNULL, no
**          information about the contiguous memory will be returned.
**
**      gctSIZE_T * ContiguousSize
**          Pointer to a variable that will hold the size of the contiguous
**          memory.  'ContiguousSize' cannot be gcvNULL if 'ContiguousAddress'
**          is not gcvNULL.
*/
gceSTATUS
gcoOS_QueryVideoMemory(
    IN gcoOS Os,
    OUT gctPHYS_ADDR * InternalAddress,
    OUT gctSIZE_T * InternalSize,
    OUT gctPHYS_ADDR * ExternalAddress,
    OUT gctSIZE_T * ExternalSize,
    OUT gctPHYS_ADDR * ContiguousAddress,
    OUT gctSIZE_T * ContiguousSize
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER();

    /* Call kernel HAL to query video memory. */
    iface.command = gcvHAL_QUERY_VIDEO_MEMORY;

    /* Call kernel service. */
    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &iface, gcmSIZEOF(iface),
                                   &iface, gcmSIZEOF(iface)));

    if (InternalAddress != gcvNULL)
    {
        /* Verify arguments. */
        gcmDEBUG_VERIFY_ARGUMENT(InternalSize != gcvNULL);

        /* Save internal memory size. */
        *InternalAddress = gcmINT2PTR(iface.u.QueryVideoMemory.internalPhysical);
        *InternalSize    = (gctSIZE_T)iface.u.QueryVideoMemory.internalSize;
    }

    if (ExternalAddress != gcvNULL)
    {
        /* Verify arguments. */
        gcmDEBUG_VERIFY_ARGUMENT(ExternalSize != gcvNULL);

        /* Save external memory size. */
        *ExternalAddress = gcmINT2PTR(iface.u.QueryVideoMemory.externalPhysical);
        *ExternalSize    = (gctSIZE_T)iface.u.QueryVideoMemory.externalSize;
    }

    if (ContiguousAddress != gcvNULL)
    {
        /* Verify arguments. */
        gcmDEBUG_VERIFY_ARGUMENT(ContiguousSize != gcvNULL);

        /* Save contiguous memory size. */
        *ContiguousAddress = gcmINT2PTR(iface.u.QueryVideoMemory.contiguousPhysical);
        *ContiguousSize    = (gctSIZE_T)iface.u.QueryVideoMemory.contiguousSize;
    }

    /* Success. */
    gcmFOOTER_ARG("*InternalAddress=0x%08x *InternalSize=%lu "
                  "*ExternalAddress=0x%08x *ExternalSize=%lu "
                  "*ContiguousAddress=0x%08x *ContiguousSize=%lu",
                  gcmOPT_VALUE(InternalAddress), gcmOPT_VALUE(InternalSize),
                  gcmOPT_VALUE(ExternalAddress), gcmOPT_VALUE(ExternalSize),
                  gcmOPT_VALUE(ContiguousAddress),
                  gcmOPT_VALUE(ContiguousSize));
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_GetBaseAddress
**
**  Get the base address for the physical memory.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to the gcoOS object.
**
**  OUTPUT:
**
**      gctUINT32_PTR BaseAddress
**          Pointer to a variable that will receive the base address.
*/
gceSTATUS
gcoOS_GetBaseAddress(
    IN gcoOS Os,
    OUT gctUINT32_PTR BaseAddress
    )
{
    gceHARDWARE_TYPE type = gcvHARDWARE_INVALID;

    gcmHEADER();

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(BaseAddress != gcvNULL);

    gcmVERIFY_OK(gcoHAL_GetHardwareType(gcvNULL, &type));

    /* Return base address. */
    if (type == gcvHARDWARE_VG)
    {
        *BaseAddress = 0;
    }
    else
    {
        *BaseAddress = gcPLS.os->baseAddress;
    }

    /* Success. */
    gcmFOOTER_ARG("*BaseAddress=0x%08x", *BaseAddress);
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_Allocate
**
**  Allocate memory from the user heap.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctSIZE_T Bytes
**          Number of bytes to allocate.
**
**  OUTPUT:
**
**      gctPOINTER * Memory
**          Pointer to a variable that will hold the pointer to the memory
**          allocation.
*/
gceSTATUS
gcoOS_Allocate(
    IN gcoOS Os,
    IN gctSIZE_T Bytes,
    OUT gctPOINTER * Memory
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Bytes=%lu", Bytes);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Bytes > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL);

    if ((gcPLS.os != gcvNULL) && (gcPLS.os->heap != gcvNULL))
    {
        gcmONERROR(gcoHEAP_Allocate(gcPLS.os->heap, Bytes, Memory));
    }
    else
    {
        gcmONERROR(gcoOS_AllocateMemory(gcPLS.os, Bytes, Memory));
    }

    /* Success. */
    gcmFOOTER_ARG("*Memory=0x%x", *Memory);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_GetMemorySize
**
**  Get allocated memory from the user heap.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctPOINTER  Memory
**          Pointer to the memory
**          allocation.
**
**  OUTPUT:
**
**      gctPOINTER MemorySize
**          Pointer to a variable that will hold the pointer to the memory
**          size.
*/
gceSTATUS
gcoOS_GetMemorySize(
    IN gcoOS Os,
    IN gctPOINTER Memory,
    OUT gctSIZE_T_PTR MemorySize
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Memory=0x%x", Memory);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(MemorySize != gcvNULL);

    /* Free the memory. */
    if ((gcPLS.os != gcvNULL) && (gcPLS.os->heap != gcvNULL))
    {
        gcmONERROR(gcoHEAP_GetMemorySize(gcPLS.os->heap, Memory, MemorySize));
    }
    else
    {
        *MemorySize = 0;
    }

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
 **
 ** gcoOS_Free
 **
 ** Free allocated memory from the user heap.
 **
 ** INPUT:
 **
 **     gcoOS Os
 **         Pointer to an gcoOS object.
 **
 **     gctPOINTER Memory
 **         Pointer to the memory allocation that needs to be freed.
 **
 ** OUTPUT:
 **
 **     Nothing.
 */
gceSTATUS
gcoOS_Free(
    IN gcoOS Os,
    IN gctPOINTER Memory
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Memory=0x%x", Memory);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL);

    /* Free the memory. */
    if ((gcPLS.os != gcvNULL) && (gcPLS.os->heap != gcvNULL))
    {
        gcmONERROR(gcoHEAP_Free(gcPLS.os->heap, Memory));
    }
    else
    {
        gcmONERROR(gcoOS_FreeMemory(gcPLS.os, Memory));
    }

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_AllocateSharedMemory
**
**  Allocate memory that can be used in both user and kernel.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctSIZE_T Bytes
**          Number of bytes to allocate.
**
**  OUTPUT:
**
**      gctPOINTER * Memory
**          Pointer to a variable that will hold the pointer to the memory
**          allocation.
*/
gceSTATUS
gcoOS_AllocateSharedMemory(
    IN gcoOS Os,
    IN gctSIZE_T Bytes,
    OUT gctPOINTER * Memory
    )
{
    return gcoOS_Allocate(Os, Bytes, Memory);
}

/*******************************************************************************
 **
 ** gcoOS_FreeSharedMemory
 **
 ** Free allocated memory.
 **
 ** INPUT:
 **
 **     gcoOS Os
 **         Pointer to an gcoOS object.
 **
 **     gctPOINTER Memory
 **         Pointer to the memory allocation that needs to be freed.
 **
 ** OUTPUT:
 **
 **     Nothing.
 */
gceSTATUS
gcoOS_FreeSharedMemory(
    IN gcoOS Os,
    IN gctPOINTER Memory
    )
{
    return gcoOS_Free(Os, Memory);
}

/*******************************************************************************
 **
 ** gcoOS_AllocateMemory
 **
 ** Allocate memory from the user heap.
 **
 ** INPUT:
 **
 **     gcoOS Os
 **         Pointer to an gcoOS object.
 **
 **     gctSIZE_T Bytes
 **         Number of bytes to allocate.
 **
 ** OUTPUT:
 **
 **     gctPOINTER * Memory
 **         Pointer to a variable that will hold the pointer to the memory
 **         allocation.
 */
gceSTATUS
gcoOS_AllocateMemory(
    IN gcoOS Os,
    IN gctSIZE_T Bytes,
    OUT gctPOINTER * Memory
    )
{
    gceSTATUS status;
    gctPOINTER memory;

    gcmHEADER_ARG("Bytes=%lu", Bytes);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Bytes > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL);

    /* Allocate the memory. */
#if VIVANTE_PROFILER
    memory = malloc(Bytes + gcmSIZEOF(gctSIZE_T));
#else
    memory = malloc(Bytes);
#endif

    if (memory == gcvNULL)
    {
        /* Out of memory. */
        gcmONERROR(gcvSTATUS_OUT_OF_MEMORY);
    }

#if VIVANTE_PROFILER
    {
        gcoOS os = (gcPLS.os != gcvNULL) ? gcPLS.os : Os;

        if (os != gcvNULL)
        {
            ++ (os->allocCount);
            os->allocSize += Bytes;
            if (os->allocSize > os->maxAllocSize)
            {
                os->maxAllocSize = os->allocSize;
            }

#if gcdGC355_MEM_PRINT
            if (os->oneRecording == 1)
            {
                os->oneSize += (gctINT32)Bytes;
            }
#endif
        }

        /* Return pointer to the memory allocation. */
        *(gctSIZE_T *) memory = Bytes;
        *Memory = (gctPOINTER) ((gctSIZE_T *) memory + 1);
    }
#else
    /* Return pointer to the memory allocation. */
    *Memory = memory;
#endif

    /* Success. */
    gcmFOOTER_ARG("*Memory=0x%x", *Memory);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
 **
 ** gcoOS_FreeMemory
 **
 ** Free allocated memory from the user heap.
 **
 ** INPUT:
 **
 **     gcoOS Os
 **         Pointer to an gcoOS object.
 **
 **     gctPOINTER Memory
 **         Pointer to the memory allocation that needs to be freed.
 **
 ** OUTPUT:
 **
 **     Nothing.
 */
gceSTATUS
gcoOS_FreeMemory(
    IN gcoOS Os,
    IN gctPOINTER Memory
    )
{
    gcmHEADER_ARG("Memory=0x%x", Memory);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL);

    /* Free the memory allocation. */
#if VIVANTE_PROFILER
    {
        gcoOS os = (gcPLS.os != gcvNULL) ? gcPLS.os : Os;

        if (os != gcvNULL)
        {
#if gcdGC355_MEM_PRINT
            if (os->oneRecording == 1)
            {
                os->oneSize -= (gctINT32)(*((gctSIZE_T *) Memory - 1));
            }
#endif
            os->allocSize -= *((gctSIZE_T *) Memory - 1);
            os->freeSize += *((gctSIZE_T *) Memory - 1);
            free((gctSIZE_T *) Memory - 1);
            ++ (os->freeCount);
        }
    }
#else
    free(Memory);
#endif

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
 **
 ** gcoOS_DeviceControl
 **
 ** Perform a device I/O control call to the kernel API.
 **
 ** INPUT:
 **
 **     gcoOS Os
 **         Pointer to an gcoOS object.
 **
 **     gctUINT32 IoControlCode
 **         I/O control code to execute.
 **
 **     gctPOINTER InputBuffer
 **         Pointer to the input buffer.
 **
 **     gctSIZE_T InputBufferSize
 **         Size of the input buffer in bytes.
 **
 **     gctSIZE_T outputBufferSize
 **         Size of the output buffer in bytes.
 **
 ** OUTPUT:
 **
 **     gctPOINTER OutputBuffer
 **         Output buffer is filled with the data returned from the kernel HAL
 **         layer.
 */
gceSTATUS
gcoOS_DeviceControl(
    IN gcoOS Os,
    IN gctUINT32 IoControlCode,
    IN gctPOINTER InputBuffer,
    IN gctSIZE_T InputBufferSize,
    OUT gctPOINTER OutputBuffer,
    IN gctSIZE_T OutputBufferSize
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE_PTR inputBuffer;
    gcsHAL_INTERFACE_PTR outputBuffer;
    gcsDRIVER_ARGS args;
    gcsTLS_PTR tls;
    gctPOINTER logical = gcvNULL;
    gctUINT32 interrupt_count = 0;

    gcmHEADER_ARG("IoControlCode=%u InputBuffer=0x%x "
                  "InputBufferSize=%lu OutputBuffer=0x%x OutputBufferSize=%lu",
                  IoControlCode, InputBuffer, InputBufferSize,
                  OutputBuffer, OutputBufferSize);

    if (gcPLS.os == gcvNULL)
    {
        gcmONERROR(gcvSTATUS_DEVICE);
    }

    /* Cast the interface. */
    inputBuffer  = (gcsHAL_INTERFACE_PTR) InputBuffer;
    outputBuffer = (gcsHAL_INTERFACE_PTR) OutputBuffer;

    /* Set current hardware type */
    if (gcPLS.processID)
    {
        gcmONERROR(gcoOS_GetTLS(&tls));
        inputBuffer->hardwareType = tls->currentType;
    }
    else
    {
        inputBuffer->hardwareType = gcvHARDWARE_2D;
    }

    switch (inputBuffer->command)
    {
    case gcvHAL_MAP_MEMORY:
        logical = mmap(
            gcvNULL, inputBuffer->u.MapMemory.bytes,
            PROT_READ | PROT_WRITE, MAP_SHARED,
            gcPLS.os->device, (off_t) 0
            );

        if (logical != MAP_FAILED)
        {
            inputBuffer->u.MapMemory.logical = gcmPTR_TO_UINT64(logical);
            inputBuffer->status = gcvSTATUS_OK;
            gcmFOOTER_NO();
            return gcvSTATUS_OK;
        }
        break;

    case gcvHAL_UNMAP_MEMORY:
        munmap(gcmUINT64_TO_PTR(inputBuffer->u.UnmapMemory.logical), inputBuffer->u.UnmapMemory.bytes);

        inputBuffer->status = gcvSTATUS_OK;
        gcmFOOTER_NO();
        return gcvSTATUS_OK;

    default:
        /* This has to be here so that GCC does not complain. */
        break;
    }

    /* Call kernel. */
    args.InputBuffer      = gcmPTR_TO_UINT64(InputBuffer);
    args.InputBufferSize  = InputBufferSize;
    args.OutputBuffer     = gcmPTR_TO_UINT64(OutputBuffer);
    args.OutputBufferSize = OutputBufferSize;

    while (ioctl(gcPLS.os->device, IoControlCode, &args) < 0)
    {
        if (errno != EINTR)
        {
            gcmTRACE(gcvLEVEL_ERROR, "ioctl failed; errno=%s\n", strerror(errno));
            gcmONERROR(gcvSTATUS_GENERIC_IO);
        }
        /* Retry MAX_RETRY_IOCTL_TIMES times at most when receiving interrupt */
        else if (++interrupt_count == MAX_RETRY_IOCTL_TIMES)
        {
            gcmTRACE(gcvLEVEL_ERROR, "ioctl failed; too many interrupt\n");
            gcmONERROR(gcvSTATUS_GENERIC_IO);
        }
    }

    /* Get the status. */
    status = outputBuffer->status;

    /* Eliminate gcmONERROR on gcoOS_WaitSignal timeout errors. */
#if gcmIS_DEBUG(gcdDEBUG_CODE)
    if ((inputBuffer->command == gcvHAL_USER_SIGNAL) &&
        (inputBuffer->u.UserSignal.command == gcvUSER_SIGNAL_WAIT))
    {
        if (status == gcvSTATUS_TIMEOUT)
        {
            goto OnError;
        }
    }
#endif

    /* Test for API error. */
    gcmONERROR(status);

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
 **
 ** gcoOS_AllocateNonPagedMemory
 **
 ** Allocate non-paged memory from the kernel.
 **
 ** INPUT:
 **
 **     gcoOS Os
 **         Pointer to an gcoOS object.
 **
 **      gctBOOL InUserSpace
 **          gcvTRUE to mape the memory into the user space.
 **
 **      gctSIZE_T * Bytes
 **          Pointer to the number of bytes to allocate.
 **
 ** OUTPUT:
 **
 **     gctSIZE_T * Bytes
 **         Pointer to a variable that will receive the aligned number of bytes
 **          allocated.
 **
 **     gctPHYS_ADDR * Physical
 **         Pointer to a variable that will receive the physical addresses of
 **          the allocated pages.
 **
 **     gctPOINTER * Logical
 **         Pointer to a variable that will receive the logical address of the
 **         allocation.
 */
gceSTATUS
gcoOS_AllocateNonPagedMemory(
    IN gcoOS Os,
    IN gctBOOL InUserSpace,
    IN OUT gctSIZE_T * Bytes,
    OUT gctPHYS_ADDR * Physical,
    OUT gctPOINTER * Logical
    )
{
    gcsHAL_INTERFACE iface;
    gceSTATUS status;

    gcmHEADER_ARG("InUserSpace=%d *Bytes=%lu",
                  InUserSpace, gcmOPT_VALUE(Bytes));

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Bytes != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Physical != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Logical != gcvNULL);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_ALLOCATE_NON_PAGED_MEMORY;
    iface.u.AllocateNonPagedMemory.bytes = *Bytes;

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

    /* Return allocated memory. */
    *Bytes    = (gctSIZE_T)iface.u.AllocateNonPagedMemory.bytes;
    *Physical = gcmINT2PTR(iface.u.AllocateNonPagedMemory.physical);
    *Logical  = gcmUINT64_TO_PTR(iface.u.AllocateNonPagedMemory.logical);

    /* Success. */
    gcmFOOTER_ARG("*Bytes=%lu *Physical=0x%x *Logical=0x%x",
                  *Bytes, *Physical, *Logical);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
 **
 ** gcoOS_FreeNonPagedMemory
 **
 ** Free non-paged memory from the kernel.
 **
 ** INPUT:
 **
 **     gcoOS Os
 **         Pointer to an gcoOS object.
 **
 **      gctBOOL InUserSpace
 **          gcvTRUE to mape the memory into the user space.
 **
 **      gctSIZE_T * Bytes
 **          Pointer to the number of bytes to allocate.
 **
 ** OUTPUT:
 **
 **     gctSIZE_T * Bytes
 **         Pointer to a variable that will receive the aligned number of bytes
 **          allocated.
 **
 **     gctPHYS_ADDR * Physical
 **         Pointer to a variable that will receive the physical addresses of
 **          the allocated pages.
 **
 **     gctPOINTER * Logical
 **         Pointer to a variable that will receive the logical address of the
 **         allocation.
 */
gceSTATUS
gcoOS_FreeNonPagedMemory(
    IN gcoOS Os,
    IN gctSIZE_T Bytes,
    IN gctPHYS_ADDR Physical,
    IN gctPOINTER Logical
    )
{
    gcsHAL_INTERFACE iface;
    gceSTATUS status;

    gcmHEADER_ARG("Bytes=%lu Physical=0x%x Logical=0x%x",
                  Bytes, Physical, Logical);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_FREE_NON_PAGED_MEMORY;
    iface.u.FreeNonPagedMemory.bytes    = Bytes;
    iface.u.FreeNonPagedMemory.physical = gcmPTR2INT32(Physical);
    iface.u.FreeNonPagedMemory.logical  = gcmPTR_TO_UINT64(Logical);

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, sizeof(iface),
        &iface, sizeof(iface)
        ));

OnError:
    /* Return status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_AllocateContiguous
**
**  Allocate contiguous memory from the kernel.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctBOOL InUserSpace
**          gcvTRUE to map the memory into the user space.
**
**      gctSIZE_T * Bytes
**          Pointer to the number of bytes to allocate.
**
**  OUTPUT:
**
**      gctSIZE_T * Bytes
**          Pointer to a variable that will receive the aligned number of bytes
**          allocated.
**
**      gctPHYS_ADDR * Physical
**          Pointer to a variable that will receive the physical addresses of
**          the allocated memory.
**
**      gctPOINTER * Logical
**          Pointer to a variable that will receive the logical address of the
**          allocation.
*/
gceSTATUS
gcoOS_AllocateContiguous(
    IN gcoOS Os,
    IN gctBOOL InUserSpace,
    IN OUT gctSIZE_T * Bytes,
    OUT gctPHYS_ADDR * Physical,
    OUT gctPOINTER * Logical
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("InUserSpace=%d *Bytes=%lu",
                  InUserSpace, gcmOPT_VALUE(Bytes));

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Bytes != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Physical != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Logical != gcvNULL);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_ALLOCATE_CONTIGUOUS_MEMORY;
    iface.u.AllocateContiguousMemory.bytes = *Bytes;

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

    /* Return allocated number of bytes. */
    *Bytes = (gctSIZE_T) iface.u.AllocateContiguousMemory.bytes;

    /* Return physical address. */
    *Physical = gcmINT2PTR(iface.u.AllocateContiguousMemory.physical);

    /* Return logical address. */
    *Logical = gcmUINT64_TO_PTR(iface.u.AllocateContiguousMemory.logical);

    /* Success. */
    gcmFOOTER_ARG("*Bytes=%lu *Physical=0x%x *Logical=0x%x",
                  *Bytes, *Physical, *Logical);
    return gcvSTATUS_OK;

OnError:
    gcmTRACE(
        gcvLEVEL_ERROR,
        "%s(%d): failed to allocate %lu bytes",
        __FUNCTION__, __LINE__, *Bytes
        );

    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_FreeContiguous
**
**  Free contiguous memory from the kernel.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctPHYS_ADDR Physical
**          The physical addresses of the allocated pages.
**
**      gctPOINTER Logical
**          The logical address of the allocation.
**
**      gctSIZE_T Bytes
**          Number of bytes allocated.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_FreeContiguous(
    IN gcoOS Os,
    IN gctPHYS_ADDR Physical,
    IN gctPOINTER Logical,
    IN gctSIZE_T Bytes
    )
{
    gcsHAL_INTERFACE iface;
    gceSTATUS status;

    gcmHEADER_ARG("Physical=0x%x Logical=0x%x Bytes=%lu",
                  Physical, Logical, Bytes);

    do
    {
        /* Initialize the gcsHAL_INTERFACE structure. */
        iface.command = gcvHAL_FREE_CONTIGUOUS_MEMORY;
        iface.u.FreeContiguousMemory.bytes    = Bytes;
        iface.u.FreeContiguousMemory.physical = gcmPTR2INT32(Physical);
        iface.u.FreeContiguousMemory.logical  = gcmPTR_TO_UINT64(Logical);

        /* Call kernel driver. */
        gcmERR_BREAK(gcoOS_DeviceControl(
            gcvNULL,
            IOCTL_GCHAL_INTERFACE,
            &iface, gcmSIZEOF(iface),
            &iface, gcmSIZEOF(iface)
            ));

        /* Success. */
        gcmFOOTER_NO();
        return gcvSTATUS_OK;
    }
    while (gcvFALSE);

    /* Return the status. */
    gcmFOOTER();
    return status;
}

/* VIV: These three functions are only used to be comptible with Freescale Android,
   VIV: do NOT use them in other places. */
/*******************************************************************************
**
**  gcoOS_AllocateVideoMemory (OBSOLETE, do NOT use it)
**
**  Allocate contiguous video memory from the kernel.
**
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctBOOL InUserSpace
**          gcvTRUE to map the memory into the user space.
**
**      gctBOOL InCacheable
**          gcvTRUE to allocate the cacheable memory.
**
**
**      gctSIZE_T * Bytes
**          Pointer to the number of bytes to allocate.
**
**  OUTPUT:
**
**      gctSIZE_T * Bytes
**          Pointer to a variable that will receive the aligned number of bytes
**          allocated.
**
**      gctUINT32 * Physical
**          Pointer to a variable that will receive the physical addresses of
**          the allocated memory.
**
**      gctPOINTER * Logical
**          Pointer to a variable that will receive the logical address of the
**          allocation.
**
**      gctPOINTER * Handle
**          Pointer to a variable that will receive the node handle of the
**          allocation.
*/
gceSTATUS
gcoOS_AllocateVideoMemory(
    IN gcoOS Os,
    IN gctBOOL InUserSpace,
    IN gctBOOL InCacheable,
    IN OUT gctSIZE_T * Bytes,
    OUT gctUINT32 * Physical,
    OUT gctPOINTER * Logical,
    OUT gctPOINTER * Handle
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;
    gceHARDWARE_TYPE type;
    gctUINT32 flag = 0;

    gcmHEADER_ARG("InUserSpace=%d *Bytes=%lu",
                  InUserSpace, gcmOPT_VALUE(Bytes));

    /* Verify the arguments. */
    gcmVERIFY_ARGUMENT(Bytes != gcvNULL);
    gcmVERIFY_ARGUMENT(Physical != gcvNULL);
    gcmVERIFY_ARGUMENT(Logical != gcvNULL);

    gcmVERIFY_OK(gcoHAL_GetHardwareType(gcvNULL, &type));
    gcoHAL_SetHardwareType(gcvNULL, gcvHARDWARE_3D);

    flag |= gcvALLOC_FLAG_CONTIGUOUS;

    if (InCacheable)
    {
        flag |= gcvALLOC_FLAG_CACHEABLE;
    }

    iface.command     = gcvHAL_ALLOCATE_LINEAR_VIDEO_MEMORY;
    iface.u.AllocateLinearVideoMemory.bytes     = *Bytes;

    iface.u.AllocateLinearVideoMemory.alignment = 64;
    iface.u.AllocateLinearVideoMemory.pool      = gcvPOOL_DEFAULT;
    iface.u.AllocateLinearVideoMemory.type      = gcvSURF_BITMAP;
    iface.u.AllocateLinearVideoMemory.flag      = flag;

     /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

    /* Return allocated number of bytes. */
    *Bytes = iface.u.AllocateLinearVideoMemory.bytes;

    /* Return the handle of allocated Node. */
    *Handle = gcmUINT64_TO_PTR(iface.u.AllocateLinearVideoMemory.node);

    /* Fill in the kernel call structure. */
    iface.command = gcvHAL_LOCK_VIDEO_MEMORY;
    iface.u.LockVideoMemory.node = iface.u.AllocateLinearVideoMemory.node;
    iface.u.LockVideoMemory.cacheable = InCacheable;

    /* Call the kernel. */
    gcmONERROR(gcoOS_DeviceControl(
                gcvNULL,
                IOCTL_GCHAL_INTERFACE,
                &iface, sizeof(iface),
                &iface, sizeof(iface)
                ));

    /* Success? */
    gcmONERROR(iface.status);

    /* Return physical address. */
    *Physical = iface.u.LockVideoMemory.physicalAddress;

    /* Return logical address. */
    *Logical = gcmUINT64_TO_PTR(iface.u.LockVideoMemory.memory);

    gcoHAL_SetHardwareType(gcvNULL, type);

    /* Success. */
    gcmFOOTER_ARG("*Bytes=%lu *Physical=0x%x *Logical=0x%x",
                  *Bytes, *Physical, *Logical);
    return gcvSTATUS_OK;

OnError:
    gcmTRACE(
        gcvLEVEL_ERROR,
        "%s(%d): failed to allocate %lu bytes",
        __FUNCTION__, __LINE__, *Bytes
        );

    gcoHAL_SetHardwareType(gcvNULL, type);

    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_FreeVideoMemory (OBSOLETE, do NOT use it)
**
**  Free contiguous video memory from the kernel.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctPOINTER  Handle
**          Pointer to a variable that indicate the node of the
**          allocation.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_FreeVideoMemory(
    IN gcoOS Os,
    IN gctPOINTER Handle
    )
{
    gcsHAL_INTERFACE iface;
    gceHARDWARE_TYPE type;
    gceSTATUS status;

    gcmHEADER_ARG("Os=0x%x Handle=0x%x",
                  Os, Handle);
    gcmVERIFY_OK(gcoHAL_GetHardwareType(gcvNULL, &type));
    gcoHAL_SetHardwareType(gcvNULL, gcvHARDWARE_3D);

    do
    {
        gcuVIDMEM_NODE_PTR Node = (gcuVIDMEM_NODE_PTR)Handle;

        /* Free first to set freed flag since asynchroneous unlock is needed for the contiguous memory. */
        iface.command = gcvHAL_RELEASE_VIDEO_MEMORY;
        iface.u.ReleaseVideoMemory.node = gcmPTR_TO_UINT64(Node);

        /* Call kernel driver. */
        gcmONERROR(gcoOS_DeviceControl(
            gcvNULL,
            IOCTL_GCHAL_INTERFACE,
            &iface, gcmSIZEOF(iface),
            &iface, gcmSIZEOF(iface)
            ));

        /* Unlock the video memory node in asynchronous mode. */
        iface.command = gcvHAL_UNLOCK_VIDEO_MEMORY;
        iface.u.UnlockVideoMemory.node = gcmPTR_TO_UINT64(Node);
        iface.u.UnlockVideoMemory.type = gcvSURF_BITMAP;
        iface.u.UnlockVideoMemory.asynchroneous = gcvTRUE;

         /* Call kernel driver. */
        gcmONERROR(gcoOS_DeviceControl(
            gcvNULL,
            IOCTL_GCHAL_INTERFACE,
            &iface, gcmSIZEOF(iface),
            &iface, gcmSIZEOF(iface)
            ));

        /* Success? */
        gcmONERROR(iface.status);

        /* Do we need to schedule an event for the unlock? */
        if (iface.u.UnlockVideoMemory.asynchroneous)
        {
            iface.u.UnlockVideoMemory.asynchroneous = gcvFALSE;
            gcmONERROR(gcoHAL_ScheduleEvent(gcvNULL, &iface));
            gcmONERROR(gcoHAL_Commit(gcvNULL, gcvFALSE));
        }

        gcoHAL_SetHardwareType(gcvNULL, type);

        /* Success. */
        gcmFOOTER_NO();

        return gcvSTATUS_OK;
    }
    while (gcvFALSE);

OnError:
       gcmTRACE(
               gcvLEVEL_ERROR,
               "%s(%d): failed to free handle %lu",
               __FUNCTION__, __LINE__, Handle
               );

    gcoHAL_SetHardwareType(gcvNULL, type);

    /* Return the status. */
    gcmFOOTER();
    return status;
}

/* Lock video memory. */
gceSTATUS
gcoOS_LockVideoMemory(
    IN gcoOS Os,
    IN gctPOINTER Handle,
    IN gctBOOL InUserSpace,
    IN gctBOOL InCacheable,
    OUT gctUINT32 * Physical,
    OUT gctPOINTER * Logical
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;
    gceHARDWARE_TYPE type;
    gcuVIDMEM_NODE_PTR Node;

    gcmHEADER_ARG("Handle=%d,InUserSpace=%d InCacheable=%d",
                  Handle, InUserSpace, InCacheable);

    /* Verify the arguments. */
    gcmVERIFY_ARGUMENT(Handle != gcvNULL);
    gcmVERIFY_ARGUMENT(Physical != gcvNULL);
    gcmVERIFY_ARGUMENT(Logical != gcvNULL);

    Node = (gcuVIDMEM_NODE_PTR)Handle;

    gcmVERIFY_OK(gcoHAL_GetHardwareType(gcvNULL, &type));
    gcoHAL_SetHardwareType(gcvNULL, gcvHARDWARE_3D);

    /* Fill in the kernel call structure. */
    iface.command = gcvHAL_LOCK_VIDEO_MEMORY;
    iface.u.LockVideoMemory.node = gcmPTR_TO_UINT64(Node);
    iface.u.LockVideoMemory.cacheable = InCacheable;

    /* Call the kernel. */
    gcmONERROR(gcoOS_DeviceControl(
                gcvNULL,
                IOCTL_GCHAL_INTERFACE,
                &iface, sizeof(iface),
                &iface, sizeof(iface)
                ));

    /* Success? */
    gcmONERROR(iface.status);

    /* Return physical address. */
    *Physical = iface.u.LockVideoMemory.physicalAddress;

    /* Return logical address. */
    *Logical = gcmUINT64_TO_PTR(iface.u.LockVideoMemory.memory);

    gcoHAL_SetHardwareType(gcvNULL, type);

    /* Success. */
    gcmFOOTER_ARG("*Physical=0x%x *Logical=0x%x",
                  *Physical, *Logical);
    return gcvSTATUS_OK;

OnError:
    gcmTRACE(
        gcvLEVEL_ERROR,
        "%s(%d): failed to lock handle %x",
        __FUNCTION__, __LINE__, Handle
        );

    gcoHAL_SetHardwareType(gcvNULL, type);

    /* Return the status. */
    gcmFOOTER();
    return status;

}


/*******************************************************************************
**
**  gcoOS_Open
**
**  Open or create a file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctCONST_STRING FileName
**          File name of file to open or create.
**
**      gceFILE_MODE Mode
**          Mode to open file with:
**
**              gcvFILE_CREATE      - Overwite any existing file.
**              gcvFILE_APPEND      - Append to an exisiting file or create a
**                                    new file if there is no exisiting file.
**              gcvFILE_READ        - Open an existing file for read only.
**              gcvFILE_CREATETEXT  - Overwite any existing text file.
**              gcvFILE_APPENDTEXT  - Append to an exisiting text file or create
**                                    a new text file if there is no exisiting
**                                    file.
**              gcvFILE_READTEXT    - Open an existing text file fir read only.
**
**  OUTPUT:
**
**      gctFILE * File
**          Pointer to a variable receivig the handle to the opened file.
*/
gceSTATUS
gcoOS_Open(
    IN gcoOS Os,
    IN gctCONST_STRING FileName,
    IN gceFILE_MODE Mode,
    OUT gctFILE * File
    )
{
    static gctCONST_STRING modes[] =
    {
        "wb",
        "ab",
        "rb",
        "w",
        "a",
        "r",
    };
    FILE * file;

    gcmHEADER_ARG("FileName=%s Mode=%d",
                  gcmOPT_STRING(FileName), Mode);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(File != gcvNULL);

    /* Open the file. */
    file = fopen(FileName, modes[Mode]);

    if (file == gcvNULL)
    {
        /* Error. */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
        return gcvSTATUS_GENERIC_IO;
    }

    /* Return handle to file. */
    *File = (gctFILE) file;

    /* Success. */
    gcmFOOTER_ARG("*File=0x%x", *File);
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_Close
**
**  Close a file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctFILE File
**          Pointer to an open file object.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_Close(
    IN gcoOS Os,
    IN gctFILE File
    )
{
    gcmHEADER_ARG("File=0x%x", File);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(File != gcvNULL);

    /* Close the file. */
    fclose((FILE *) File);

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_Read
**
**  Read data from an open file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctFILE File
**          Pointer to an open file object.
**
**      gctSIZE_T ByteCount
**          Number of bytes to read from the file.
**
**      gctCONST_POINTER Data
**          Pointer to the data to read from the file.
**
**  OUTPUT:
**
**      gctSIZE_T * ByteRead
**          Pointer to a variable receiving the number of bytes read from the
**          file.
*/
gceSTATUS
gcoOS_Read(
    IN gcoOS Os,
    IN gctFILE File,
    IN gctSIZE_T ByteCount,
    IN gctPOINTER Data,
    OUT gctSIZE_T * ByteRead
    )
{
    size_t byteRead;

    gcmHEADER_ARG("File=0x%x ByteCount=%lu Data=0x%x",
                  File, ByteCount, Data);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(File != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(ByteCount > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Data != gcvNULL);

    /* Read the data from the file. */
    byteRead = fread(Data, 1, ByteCount, (FILE *) File);

    if (ByteRead != gcvNULL)
    {
        *ByteRead = (gctSIZE_T) byteRead;
    }

    /* Success. */
    gcmFOOTER_ARG("*ByteRead=%lu", gcmOPT_VALUE(ByteRead));
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_Write
**
**  Write data to an open file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctFILE File
**          Pointer to an open file object.
**
**      gctSIZE_T ByteCount
**          Number of bytes to write to the file.
**
**      gctCONST_POINTER Data
**          Pointer to the data to write to the file.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_Write(
    IN gcoOS Os,
    IN gctFILE File,
    IN gctSIZE_T ByteCount,
    IN gctCONST_POINTER Data
    )
{
    size_t byteWritten;

    gcmHEADER_ARG("File=0x%x ByteCount=%lu Data=0x%x",
                  File, ByteCount, Data);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(File != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(ByteCount > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Data != gcvNULL);

    /* Write the data to the file. */
    byteWritten = fwrite(Data, 1, ByteCount, (FILE *) File);

    if (byteWritten == ByteCount)
    {
        /* Success. */
        gcmFOOTER_NO();
        return gcvSTATUS_OK;
    }
    else
    {
        /* Error */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
        return gcvSTATUS_GENERIC_IO;
    }
}

/* Flush data to a file. */
gceSTATUS
gcoOS_Flush(
    IN gcoOS Os,
    IN gctFILE File
    )
{
    gcmHEADER_ARG("File=0x%x", File);

    fflush((FILE *) File);

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gcoOS_FscanfI(
    IN gcoOS Os,
    IN gctFILE File,
    IN gctCONST_STRING Format,
    OUT gctUINT *result
    )
{
    gctINT ret;
    gcmHEADER_ARG("File=0x%x Format=0x%x", File, Format);

    /* Verify the arguments. */
    gcmVERIFY_ARGUMENT(File != gcvNULL);
    gcmVERIFY_ARGUMENT(Format != gcvNULL);

    /* Format the string. */
    ret = fscanf(File, Format, result);

    if (!ret)
    {
        gcmFOOTER_NO();
        return gcvSTATUS_GENERIC_IO;
    }

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gcoOS_DupFD(
    IN gcoOS Os,
    IN gctINT FD,
    OUT gctINT * FD2
    )
{
    int fd;
    gceSTATUS status;

    gcmHEADER_ARG("FD=%d", FD);
    fd = dup(FD);

    if (fd < 0)
    {
        status = gcvSTATUS_OUT_OF_RESOURCES;
        gcmFOOTER();
        return status;
    }

    *FD2 = fd;
    gcmFOOTER_ARG("FD2=%d", FD2);
    return gcvSTATUS_OK;
}

gceSTATUS
gcoOS_CloseFD(
    IN gcoOS Os,
    IN gctINT FD
    )
{
    int err;
    gceSTATUS status;
    gcmHEADER_ARG("FD=%d", FD);

    err = close(FD);

    if (err < 0)
    {
        status = gcvSTATUS_GENERIC_IO;
        gcmFOOTER();
        return status;
    }

    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/* Create an endpoint for communication. */
gceSTATUS
gcoOS_Socket(
    IN gcoOS Os,
    IN gctINT Domain,
    IN gctINT Type,
    IN gctINT Protocol,
    OUT gctINT * SockFd
    )
{
    gctINT fd;

    gcmHEADER_ARG("Domain=%d Type=%d Protocol=%d",
                  Domain, Type, Protocol);

    /* Create a socket. */
    fd = socket(Domain, Type, Protocol);

    if (fd >= 0)
    {
        /* Return socket descriptor. */
        *SockFd = fd;

        /* Success. */
        gcmFOOTER_ARG("*SockFd=%d", *SockFd);
        return gcvSTATUS_OK;
    }
    else
    {
        /* Error. */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
        return gcvSTATUS_GENERIC_IO;
    }
}

/* Close a socket. */
gceSTATUS
gcoOS_WaitForSend(
    IN gcoOS Os,
    IN gctINT SockFd,
    IN gctINT Seconds,
    IN gctINT MicroSeconds
    )
{
    gcmHEADER_ARG("SockFd=%d Seconds=%d MicroSeconds=%d",
                  SockFd, Seconds, MicroSeconds);

    struct timeval tv;
    fd_set writefds;
    int ret;

    /* Linux select() will overwrite the struct on return */
    tv.tv_sec  = Seconds;
    tv.tv_usec = MicroSeconds;

    FD_ZERO(&writefds);
    FD_SET(SockFd, &writefds);

    ret = select(SockFd + 1, NULL, &writefds, NULL, &tv);

    if (ret == 0)
    {
        /* Timeout. */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_TIMEOUT);
        return gcvSTATUS_TIMEOUT;
    }
    else if (ret == -1)
    {
        /* Error. */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
        return gcvSTATUS_GENERIC_IO;
    }
    else
    {
        int error = 0;
        socklen_t len = sizeof(error);

        /* Get error code. */
        getsockopt(SockFd, SOL_SOCKET, SO_ERROR, (char*) &error, &len);

        if (! error)
        {
            /* Success. */
            gcmFOOTER_NO();
            return gcvSTATUS_OK;
        }
    }

    /* Error */
    gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
    return gcvSTATUS_GENERIC_IO;
}

/* Close a socket. */
gceSTATUS
gcoOS_CloseSocket(
    IN gcoOS Os,
    IN gctINT SockFd
    )
{
    gcmHEADER_ARG("SockFd=%d", SockFd);

    /* Close the socket file descriptor. */
    gcoOS_WaitForSend(gcvNULL, SockFd, 600, 0);
    close(SockFd);

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/* Initiate a connection on a socket. */
gceSTATUS
gcoOS_Connect(
    IN gcoOS Os,
    IN gctINT SockFd,
    IN gctCONST_POINTER HostName,
    IN gctUINT Port
    )
{
    gctINT rc;
    gctINT addrLen;
    struct sockaddr sockAddr;
    struct sockaddr_in *sockAddrIn;
    struct in_addr *inAddr;

    gcmHEADER_ARG("SockFd=0x%x HostName=0x%x Port=%d",
                  SockFd, HostName, Port);

    /* Get server address. */
    sockAddrIn = (struct sockaddr_in *) &sockAddr;
    sockAddrIn->sin_family = AF_INET;
    inAddr = &sockAddrIn->sin_addr;
    inAddr->s_addr = inet_addr(HostName);

    /* If it is a numeric host name, convert it now */
    if (inAddr->s_addr == INADDR_NONE)
    {
#if !gcdSTATIC_LINK
        struct hostent *hostEnt;
        struct in_addr *arrayAddr;

        /* It is a real name, we solve it */
        if ((hostEnt = gethostbyname(HostName)) == NULL)
        {
            /* Error */
            gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
            return gcvSTATUS_GENERIC_IO;
        }
        arrayAddr = (struct in_addr *) *(hostEnt->h_addr_list);
        inAddr->s_addr = arrayAddr[0].s_addr;
#endif /*gcdSTATIC_LINK*/
    }

    sockAddrIn->sin_port = htons((gctUINT16) Port);

    /* Currently, for INET only. */
    addrLen = sizeof(struct sockaddr);

    /*{
    gctINT arg = 1;
    ioctl(SockFd, FIONBIO, &arg);
    }*/

    /* Close the file descriptor. */
    rc = connect(SockFd, &sockAddr, addrLen);

    if (rc)
    {
        int err = errno;

        if (err == EINPROGRESS)
        {
            gceSTATUS status;

            /* Connect is not complete.  Wait for it. */
            status = gcoOS_WaitForSend(gcvNULL, SockFd, 600, 0);

            gcmFOOTER();
            return status;
        }

        /* Error */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
        return gcvSTATUS_GENERIC_IO;
    }

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/* Shut down part of connection on a socket. */
gceSTATUS
gcoOS_Shutdown(
    IN gcoOS Os,
    IN gctINT SockFd,
    IN gctINT How
    )
{
    gcmHEADER_ARG("SockFd=%d How=%d", SockFd, How);

    /* Shut down connection. */
    shutdown(SockFd, How);

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/* Send a message on a socket. */
gceSTATUS
gcoOS_Send(
    IN gcoOS Os,
    IN gctINT SockFd,
    IN gctSIZE_T ByteCount,
    IN gctCONST_POINTER Data,
    IN gctINT Flags
    )
{
    gctINT byteSent;

    gcmHEADER_ARG("SockFd=0x%x ByteCount=%lu Data=0x%x Flags=%d",
                  SockFd, ByteCount, Data, Flags);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(ByteCount > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Data != gcvNULL);

    /* Write the data to the file. */
    /*gcoOS_WaitForSend(gcvNULL, SockFd, 0, 50000);*/
    byteSent = send(SockFd, Data, ByteCount, Flags);

    if (byteSent == (gctINT) ByteCount)
    {
        /* Success. */
        gcmFOOTER_NO();
        return gcvSTATUS_OK;
    }
    else
    {
        /* Error */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
        return gcvSTATUS_GENERIC_IO;
    }
}

#ifdef ANDROID
#define MAX_ENV_ITEM 20
typedef struct _Property_Table {
    gctCHAR property[PROPERTY_VALUE_MAX];
    gctCHAR name[PROPERTY_KEY_MAX];
} Property_Table;
Property_Table pt[MAX_ENV_ITEM];

#endif

/* Get environment variable value. */
gceSTATUS
gcoOS_GetEnv(
    IN gcoOS Os,
    IN gctCONST_STRING VarName,
    OUT gctSTRING * Value
    )
{
#ifdef ANDROID
    static gctINT index=0;
    gctCHAR property[PROPERTY_VALUE_MAX];
    gctSIZE_T nlen=0,plen=0;
#endif
    gcmHEADER_ARG("VarName=%s", gcmOPT_STRING(VarName));
    if (Value == gcvNULL)
    {
        gcmFOOTER_ARG("Value=%s", "NULL");
        return gcvSTATUS_OK;
    }
#ifdef ANDROID
    if (property_get(VarName, property, gcvNULL) != 0)
    {

        gctINT loopi;
        gctINT found = -1 ;
        for(loopi = 0; loopi < MAX_ENV_ITEM;loopi++)
        {
            if(!pt[loopi].name ) break;
            if(gcoOS_StrCmp(pt[loopi].name, VarName) == gcvSTATUS_OK) /*Found name*/
            {
                if(gcoOS_StrCmp(pt[loopi].property, property) != gcvSTATUS_OK ) /*Value changed*/
                {
                    plen = gcoOS_StrLen(property, gcvNULL);
                    gcoOS_StrCopySafe(pt[loopi].property, plen + 1, property);
                }
                *Value = pt[loopi].property;
                found = 1;
                break;
            }
        }
        if(found == -1) /*Not exist*/
        {
            nlen = gcoOS_StrLen(VarName, gcvNULL);
            plen = gcoOS_StrLen(property, gcvNULL);
            gcoOS_StrCopySafe(pt[index].name, nlen + 1, VarName);
            gcoOS_StrCopySafe(pt[index].property, plen + 1, property);
            *Value = pt[index].property;
            index++;
            if (index >= MAX_ENV_ITEM)
            {
                index=0; /*cover old variable*/
            }
        }
    }
    else
    {
    *Value = gcvNULL;
     }
#else
    *Value = getenv(VarName);
#endif
    /* Success. */
    gcmFOOTER_ARG("*Value=%s", gcmOPT_STRING(*Value));
    return gcvSTATUS_OK;
}

/* Set environment variable value. */
gceSTATUS gcoOS_SetEnv(
    IN gcoOS Os,
    IN gctCONST_STRING VarName,
    IN gctSTRING Value
    )
{
#ifdef ANDROID
    return gcvSTATUS_OK;
#endif
    gcmHEADER_ARG("VarName=%s", gcmOPT_STRING(VarName));


    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/* Get current working directory. */
gceSTATUS
gcoOS_GetCwd(
    IN gcoOS Os,
    IN gctINT SizeInBytes,
    OUT gctSTRING Buffer
    )
{
    gcmHEADER_ARG("SizeInBytes=%d", SizeInBytes);

    if (getcwd(Buffer, SizeInBytes))
    {
        gcmFOOTER_ARG("Buffer=%s", Buffer);
        return gcvSTATUS_NOT_SUPPORTED;
    }
    else
    {
        gcmFOOTER_ARG("status=%d", gcvSTATUS_NOT_SUPPORTED);
        return gcvSTATUS_NOT_SUPPORTED;
    }
}

/* Get file status info. */
gceSTATUS
gcoOS_Stat(
    IN gcoOS Os,
    IN gctCONST_STRING FileName,
    OUT gctPOINTER Buffer
    )
{
    gcmHEADER_ARG("FileName=%s", gcmOPT_STRING(FileName));

    if (stat(FileName, Buffer) == 0)
    {
        gcmFOOTER_NO();
        return gcvSTATUS_OK;
    }
    else
    {
        gcmFOOTER_ARG("status=%d", gcvSTATUS_NOT_SUPPORTED);
        return gcvSTATUS_NOT_SUPPORTED;
    }
}

/*******************************************************************************
**
**  gcoOS_GetPos
**
**  Get the current position of a file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctFILE File
**          Pointer to an open file object.
**
**  OUTPUT:
**
**      gctUINT32 * Position
**          Pointer to a variable receiving the current position of the file.
*/
gceSTATUS
gcoOS_GetPos(
    IN gcoOS Os,
    IN gctFILE File,
    OUT gctUINT32 * Position
    )
{
    gcmHEADER_ARG("File=0x%x", File);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(File != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Position != gcvNULL);

    /* Get the current file position. */
    *Position = ftell((FILE *) File);

    /* Success. */
    gcmFOOTER_ARG("*Position=%u", *Position);
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_SetPos
**
**  Set position for a file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctFILE File
**          Pointer to an open file object.
**
**      gctUINT32 Position
**          Absolute position of the file to set.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_SetPos(
    IN gcoOS Os,
    IN gctFILE File,
    IN gctUINT32 Position
    )
{
    gcmHEADER_ARG("File=0x%x Position=%u", File, Position);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(File != gcvNULL);

    /* Set file position. */
    fseek((FILE *) File, Position, SEEK_SET);

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_Seek
**
**  Set position for a file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctFILE File
**          Pointer to an open file object.
**
**      gctUINT32 Offset
**          Offset added to the position specified by Whence.
**
**      gceFILE_WHENCE Whence
**          Mode that specify how to add the offset to the position:
**
**              gcvFILE_SEEK_SET    - Relative to the start of the file.
**              gcvFILE_SEEK_CUR    - Relative to the current position.
**              gcvFILE_SEEK_END    - Relative to the end of the file.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_Seek(
    IN gcoOS Os,
    IN gctFILE File,
    IN gctUINT32 Offset,
    IN gceFILE_WHENCE Whence
    )
{
    gctINT result = 0;

    gcmHEADER_ARG("File=0x%x Offset=%u Whence=%d",
                  File, Offset, Whence);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(File != gcvNULL);

    /* Set file position. */
    switch (Whence)
    {
    case gcvFILE_SEEK_SET:
        result = fseek((FILE *) File, Offset, SEEK_SET);
        break;

    case gcvFILE_SEEK_CUR:
        result = fseek((FILE *) File, Offset, SEEK_CUR);
        break;

    case gcvFILE_SEEK_END:
        result = fseek((FILE *) File, Offset, SEEK_END);
        break;
    }

    if (result == 0)
    {
        /* Success. */
        gcmFOOTER_NO();
        return gcvSTATUS_OK;
    }
    else
    {
        /* Error */
        gcmFOOTER_ARG("status=%d", gcvSTATUS_GENERIC_IO);
        return gcvSTATUS_GENERIC_IO;
    }
}

/*******************************************************************************
**
**  gcoOS_CreateThread
**
**  Create a new thread.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**  OUTPUT:
**
**      gctPOINTER * Thread
**          Pointer to a variable that will hold a pointer to the thread.
*/
gceSTATUS
gcoOS_CreateThread(
    IN gcoOS Os,
    IN gcTHREAD_ROUTINE Worker,
    IN gctPOINTER Argument,
    OUT gctPOINTER * Thread
    )
{
    pthread_t thread;

    gcmHEADER_ARG("Worker=0x%x Argument=0x%x", Worker, Argument);

    /* Validate the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Thread != gcvNULL);

    if (pthread_create(&thread, gcvNULL, Worker, Argument) != 0)
    {
        gcmFOOTER_ARG("status=%d", gcvSTATUS_OUT_OF_RESOURCES);
        return gcvSTATUS_OUT_OF_RESOURCES;
    }

    *Thread = (gctPOINTER) thread;

    /* Success. */
    gcmFOOTER_ARG("*Thread=0x%x", *Thread);
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_CloseThread
**
**  Close a thread.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctPOINTER Thread
**          Pointer to the thread to be deleted.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_CloseThread(
    IN gcoOS Os,
    IN gctPOINTER Thread
    )
{
    gcmHEADER_ARG("Thread=0x%x", Thread);

    /* Validate the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Thread != gcvNULL);

    pthread_join((pthread_t) Thread, gcvNULL);

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_CreateMutex
**
**  Create a new mutex.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**  OUTPUT:
**
**      gctPOINTER * Mutex
**          Pointer to a variable that will hold a pointer to the mutex.
*/
gceSTATUS
gcoOS_CreateMutex(
    IN gcoOS Os,
    OUT gctPOINTER * Mutex
    )
{
    gceSTATUS status;
    pthread_mutex_t* mutex;
    pthread_mutexattr_t   mta;

    gcmHEADER();

    /* Validate the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Mutex != gcvNULL);

    /* Allocate memory for the mutex. */
    gcmONERROR(gcoOS_Allocate(
        gcvNULL, gcmSIZEOF(pthread_mutex_t), (gctPOINTER *) &mutex
        ));

    pthread_mutexattr_init(&mta);

    pthread_mutexattr_settype(&mta, PTHREAD_MUTEX_RECURSIVE);

    /* Initialize the mutex. */
    pthread_mutex_init(mutex, &mta);

    /* Return mutex to caller. */
    *Mutex = (gctPOINTER) mutex;

    /* Success. */
    gcmFOOTER_ARG("*Mutex = 0x%x", *Mutex);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_DeleteMutex
**
**  Delete a mutex.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctPOINTER Mutex
**          Pointer to the mutex to be deleted.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_DeleteMutex(
    IN gcoOS Os,
    IN gctPOINTER Mutex
    )
{
    pthread_mutex_t *mutex;

    gcmHEADER_ARG("Mutex=0x%x", Mutex);

    /* Validate the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Mutex != gcvNULL);

    /* Cast the pointer. */
    mutex = (pthread_mutex_t *) Mutex;

    /* Destroy the mutex. */
    pthread_mutex_destroy(mutex);

    /* Free the memory. */
    gcmVERIFY_OK(gcmOS_SAFE_FREE(gcvNULL, mutex));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_AcquireMutex
**
**  Acquire a mutex.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctPOINTER Mutex
**          Pointer to the mutex to be acquired.
**
**      gctUINT32 Timeout
**          Timeout value specified in milliseconds.
**          Specify the value of gcvINFINITE to keep the thread suspended
**          until the mutex has been acquired.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_AcquireMutex(
    IN gcoOS Os,
    IN gctPOINTER Mutex,
    IN gctUINT32 Timeout
    )
{
    gceSTATUS status;
    pthread_mutex_t *mutex;

    gcmHEADER_ARG("Mutex=0x%x Timeout=%u", Mutex, Timeout);

    /* Validate the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Mutex != gcvNULL);

    /* Cast the pointer. */
    mutex = (pthread_mutex_t *) Mutex;

    /* Test for infinite. */
    if (Timeout == gcvINFINITE)
    {
        /* Lock the mutex. */
        if (pthread_mutex_lock(mutex))
        {
            /* Some error. */
            status = gcvSTATUS_GENERIC_IO;
        }
        else
        {
            /* Success. */
            status = gcvSTATUS_OK;
        }
    }
    else
    {
        /* Try locking the mutex. */
        if (pthread_mutex_trylock(mutex))
        {
            /* Assume timeout. */
            status = gcvSTATUS_TIMEOUT;

            /* Loop while not timeout. */
            while (Timeout-- > 0)
            {
                /* Try locking the mutex. */
                if (pthread_mutex_trylock(mutex) == 0)
                {
                    /* Success. */
                    status = gcvSTATUS_OK;
                    break;
                }

                /* Sleep 1 millisecond. */
                usleep(1000);
            }
        }
        else
        {
            /* Success. */
            status = gcvSTATUS_OK;
        }
    }

    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_ReleaseMutex
**
**  Release an acquired mutex.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctPOINTER Mutex
**          Pointer to the mutex to be released.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_ReleaseMutex(
    IN gcoOS Os,
    IN gctPOINTER Mutex
    )
{
    pthread_mutex_t *mutex;

    gcmHEADER_ARG("Mutex=0x%x", Mutex);

    /* Validate the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Mutex != gcvNULL);

    /* Cast the pointer. */
    mutex = (pthread_mutex_t *) Mutex;

    /* Release the mutex. */
    pthread_mutex_unlock(mutex);

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_Delay
**
**  Delay execution of the current thread for a number of milliseconds.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctUINT32 Delay
**          Delay to sleep, specified in milliseconds.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_Delay(
    IN gcoOS Os,
    IN gctUINT32 Delay
    )
{
    gcmHEADER_ARG("Delay=%u", Delay);

    /* Sleep for a while. */
    usleep((Delay == 0) ? 1 : (1000 * Delay));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gcoOS_StrStr(
    IN gctCONST_STRING String,
    IN gctCONST_STRING SubString,
    OUT gctSTRING * Output
    )
{
    gcmHEADER_ARG("String=0x%x SubString=0x%x", String, SubString);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(SubString != gcvNULL);

    /* Call C. */
    *Output = strstr(String, SubString);

    /* Success. */
    gcmFOOTER_ARG("*Output=0x%x", *Output);
    return gcvSTATUS_OK;
}

gceSTATUS
gcoOS_StrFindReverse(
    IN gctCONST_STRING String,
    IN gctINT8 Character,
    OUT gctSTRING * Output
    )
{
    gcmHEADER_ARG("String=0x%x Character=%d", String, Character);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Output != gcvNULL);

    /* Call C. */
    *Output = strrchr(String, Character);

    /* Success. */
    gcmFOOTER_ARG("*Output=0x%x", *Output);
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_StrCopy
**
**  Copy a string.
**
**  INPUT:
**
**      gctSTRING Destination
**          Pointer to the destination string.
**
**      gctCONST_STRING Source
**          Pointer to the source string.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_StrCopySafe(
    IN gctSTRING Destination,
    IN gctSIZE_T DestinationSize,
    IN gctCONST_STRING Source
    )
{
    gcmHEADER_ARG("Destination=0x%x DestinationSize=%lu Source=0x%x",
                  Destination, DestinationSize, Source);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Destination != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Source != gcvNULL);

    /* Don't overflow the destination buffer. */
    strncpy(Destination, Source, DestinationSize - 1);

    /* Put this there in case the strncpy overflows. */
    Destination[DestinationSize - 1] = '\0';

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_StrCat
**
**  Append a string.
**
**  INPUT:
**
**      gctSTRING Destination
**          Pointer to the destination string.
**
**      gctCONST_STRING Source
**          Pointer to the source string.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_StrCatSafe(
    IN gctSTRING Destination,
    IN gctSIZE_T DestinationSize,
    IN gctCONST_STRING Source
    )
{
    gctSIZE_T n;

    gcmHEADER_ARG("Destination=0x%x DestinationSize=%lu Source=0x%x",
                  Destination, DestinationSize, Source);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Destination != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Source != gcvNULL);

    /* Find the end of the destination. */
    n = strlen(Destination);
    if (n + 1 < DestinationSize)
    {
        /* Append the string but don't overflow the destination buffer. */
        strncpy(Destination + n, Source, DestinationSize - n - 1);

        /* Put this there in case the strncpy overflows. */
        Destination[DestinationSize - 1] = '\0';
    }

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_StrCmp
**
**  Compare two strings and return whether they match or not.
**
**  INPUT:
**
**      gctCONST_STRING String1
**          Pointer to the first string to compare.
**
**      gctCONST_STRING String2
**          Pointer to the second string to compare.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK if the strings match
**      gcvSTATUS_LARGER if String1 > String2
**      gcvSTATUS_SMALLER if String1 < String2
*/
gceSTATUS
gcoOS_StrCmp(
    IN gctCONST_STRING String1,
    IN gctCONST_STRING String2
    )
{
    int result;
    gceSTATUS status;

    gcmHEADER_ARG("String1=0x%x String2=0x%x", String1, String2);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String1 != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(String2 != gcvNULL);

    /* Compare the strings and return proper status. */
    result = strcmp(String1, String2);

    status = (result == 0) ? gcvSTATUS_OK
           : (result >  0) ? gcvSTATUS_LARGER
                           : gcvSTATUS_SMALLER;

    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_StrNCmp
**
**  Compare characters of two strings and return whether they match or not.
**
**  INPUT:
**
**      gctCONST_STRING String1
**          Pointer to the first string to compare.
**
**      gctCONST_STRING String2
**          Pointer to the second string to compare.
**
**      gctSIZE_T Count
**          Number of characters to compare.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK if the strings match
**      gcvSTATUS_LARGER if String1 > String2
**      gcvSTATUS_SMALLER if String1 < String2
*/
gceSTATUS
gcoOS_StrNCmp(
    IN gctCONST_STRING String1,
    IN gctCONST_STRING String2,
    IN gctSIZE_T Count
    )
{
    int result;
    gceSTATUS status;

    gcmHEADER_ARG("String1=0x%x String2=0x%x Count=%lu",
                  String1, String2, Count);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String1 != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(String2 != gcvNULL);

    /* Compare the strings and return proper status. */
    result = strncmp(String1, String2, Count);

    status = (result == 0)
            ? gcvSTATUS_OK
            : ((result > 0) ? gcvSTATUS_LARGER : gcvSTATUS_SMALLER);
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_StrToFloat
**
**  Convert string to float.
**
**  INPUT:
**
**      gctCONST_STRING String
**          Pointer to the string to be converted.
**
**
**  OUTPUT:
**
**      gctFLOAT * Float
**          Pointer to a variable that will receive the float.
**
*/
gceSTATUS
gcoOS_StrToFloat(
    IN gctCONST_STRING String,
    OUT gctFLOAT * Float
    )
{
    gcmHEADER_ARG("String=%s", gcmOPT_STRING(String));

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);

    *Float = (gctFLOAT) atof(String);

    gcmFOOTER_ARG("*Float=%f", *Float);
    return gcvSTATUS_OK;
}

/* Converts a hex string to 32-bit integer. */
gceSTATUS gcoOS_HexStrToInt(IN gctCONST_STRING String,
               OUT gctINT * Int)
{
    gcmHEADER_ARG("String=%s", gcmOPT_STRING(String));
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Int != gcvNULL);

    sscanf(String, "%x", Int);

    gcmFOOTER_ARG("*Int=%d", *Int);
    return gcvSTATUS_OK;
}

/* Converts a hex string to float. */
gceSTATUS gcoOS_HexStrToFloat(IN gctCONST_STRING String,
               OUT gctFLOAT * Float)
{
    gctSTRING pch = gcvNULL;
    gctCONST_STRING delim = "x.p";
    gctFLOAT b=0.0, exp=0.0;
    gctINT s=0;

    gcmHEADER_ARG("String=%s", gcmOPT_STRING(String));
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Float != gcvNULL);

    pch = strtok((gctSTRING)String, delim);
    if (pch == NULL) goto onError;

    pch = strtok(NULL, delim);
    if (pch == NULL) goto onError;
    gcmVERIFY_OK(gcoOS_StrToFloat(pch, &b));

    pch = strtok(NULL, delim);
    if (pch == NULL) goto onError;
    gcmVERIFY_OK(gcoOS_HexStrToInt(pch, &s));

    pch = strtok(NULL, delim);
    if (pch == NULL) goto onError;
    gcmVERIFY_OK(gcoOS_StrToFloat(pch, &exp));

    *Float = (float)(b + s / (float)(1 << 24)) * (float)pow(2.0, exp);

    gcmFOOTER_ARG("*Float=%d", *Float);
    return gcvSTATUS_OK;

onError:
    gcmFOOTER_NO();
    return gcvSTATUS_INVALID_ARGUMENT;
}

/*******************************************************************************
**
**  gcoOS_StrToInt
**
**  Convert string to integer.
**
**  INPUT:
**
**      gctCONST_STRING String
**          Pointer to the string to be converted.
**
**
**  OUTPUT:
**
**      gctINT * Int
**          Pointer to a variable that will receive the integer.
**
*/
gceSTATUS
gcoOS_StrToInt(
    IN gctCONST_STRING String,
    OUT gctINT * Int
    )
{
    gcmHEADER_ARG("String=%s", gcmOPT_STRING(String));

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);

    *Int = (gctINT) atoi(String);

    gcmFOOTER_ARG("*Int=%d", *Int);
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_MemCmp
**
**  Compare two memory regions and return whether they match or not.
**
**  INPUT:
**
**      gctCONST_POINTER Memory1
**          Pointer to the first memory region to compare.
**
**      gctCONST_POINTER Memory2
**          Pointer to the second memory region to compare.
**
**      gctSIZE_T Bytes
**          Number of bytes to compare.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK if the memory regions match or gcvSTATUS_MISMATCH if the
**      memory regions don't match.
*/
gceSTATUS
gcoOS_MemCmp(
    IN gctCONST_POINTER Memory1,
    IN gctCONST_POINTER Memory2,
    IN gctSIZE_T Bytes
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Memory1=0x%x Memory2=0x%x Bytes=%lu",
                  Memory1, Memory2, Bytes);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Memory1 != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Memory2 != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Bytes > 0);

    /* Compare the memory rregions and return proper status. */
    status = (memcmp(Memory1, Memory2, Bytes) == 0)
               ? gcvSTATUS_OK
               : gcvSTATUS_MISMATCH;
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_PrintStr
**
**  Append a "printf" formatted string to a string buffer and adjust the offset
**  into the string buffer.  There is no checking for a buffer overflow, so make
**  sure the string buffer is large enough.
**
**  INPUT:
**
**      gctSTRING String
**          Pointer to the string buffer.
**
**      gctUINT_PTR Offset
**          Pointer to a variable that holds the current offset into the string
**          buffer.
**
**      gctCONST_STRING Format
**          Pointer to a "printf" style format to append to the string buffer
**          pointet to by <String> at the offset specified by <*Offset>.
**
**      ...
**          Variable number of arguments that will be used by <Format>.
**
**  OUTPUT:
**
**      gctUINT_PTR Offset
**          Pointer to a variable that receives the new offset into the string
**          buffer pointed to by <String> after the formatted string pointed to
**          by <Formnat> has been appended to it.
*/
gceSTATUS
gcoOS_PrintStrSafe(
    IN gctSTRING String,
    IN gctSIZE_T StringSize,
    IN OUT gctUINT_PTR Offset,
    IN gctCONST_STRING Format,
    ...
    )
{
    va_list arguments;

    gcmHEADER_ARG("String=0x%x StringSize=%u *Offset=%u Format=0x%x",
                  String, StringSize, *Offset, Format);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Offset != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Format != gcvNULL);

    va_start(arguments, Format);

    if (*Offset < StringSize)
    {
        /* Format the string. */
        gctINT n = vsnprintf(String + *Offset,
                             StringSize - *Offset - 1,
                             Format,
                             arguments);

        if (n > 0)
        {
            *Offset += n;
        }
    }

    va_end(arguments);

    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_PrintStrV
**
**  Append a "vprintf" formatted string to a string buffer and adjust the offset
**  into the string buffer.  There is no checking for a buffer overflow, so make
**  sure the string buffer is large enough.
**
**  INPUT:
**
**      gctSTRING String
**          Pointer to the string buffer.
**
**      gctUINT_PTR Offset
**          Pointer to a variable that holds the current offset into the string
**          buffer.
**
**      gctCONST_STRING Format
**          Pointer to a "printf" style format to append to the string buffer
**          pointet to by <String> at the offset specified by <*Offset>.
**
**      gctPOINTER ArgPtr
**          Pointer to list of arguments.
**
**  OUTPUT:
**
**      gctUINT_PTR Offset
**          Pointer to a variable that receives the new offset into the string
**          buffer pointed to by <String> after the formatted string pointed to
**          by <Formnat> has been appended to it.
*/
gceSTATUS
gcoOS_PrintStrVSafe(
    OUT gctSTRING String,
    IN gctSIZE_T StringSize,
    IN OUT gctUINT_PTR Offset,
    IN gctCONST_STRING Format,
    IN gctARGUMENTS Arguments
    )
{
    gcmHEADER_ARG("String=0x%x StringSize=%lu *Offset=%u Format=0x%x "
                  "Arguments=0x%x",
                  String, StringSize, gcmOPT_VALUE(Offset), Format, Arguments);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Offset != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Format != gcvNULL);

    if (*Offset < StringSize)
    {
        /* Format the string. */
        gctINT n = vsnprintf(String + *Offset,
                             StringSize - *Offset - 1,
                             Format,
                             Arguments);

        if (n > 0)
        {
            *Offset += n;
        }
    }

    /* Success. */
    gcmFOOTER_ARG("*Offset=%u", *Offset);
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_MapUserMemory
**
**  Lock down a user buffer and return an DMA'able address to be used by the
**  hardware to access it.
**
**  INPUT:
**
**      gctPOINTER Memory
**          Pointer to memory to lock down.
**
**      gctSIZE_T Size
**          Size in bytes of the memory to lock down.
**
**  OUTPUT:
**
**      gctPOINTER * Info
**          Pointer to variable receiving the information record required by
**          gcoOS_UnmapUserMemory.
**
**      gctUINT32_PTR Address
**          Pointer to a variable that will receive the address DMA'able by the
**          hardware.
*/
gceSTATUS
gcoOS_MapUserMemory(
    IN gcoOS Os,
    IN gctPOINTER Memory,
    IN gctSIZE_T Size,
    OUT gctPOINTER * Info,
    OUT gctUINT32_PTR Address
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Memory=0x%x Size=%lu", Memory, Size);

    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL);

    status = gcoOS_MapUserMemoryEx(Os, Memory, ~0U, Size, Info, Address);

    gcmFOOTER();

    return status;
}

gceSTATUS
gcoOS_MapUserMemoryEx(
    IN gcoOS Os,
    IN gctPOINTER Memory,
    IN gctUINT32 Physical,
    IN gctSIZE_T Size,
    OUT gctPOINTER * Info,
    OUT gctUINT32_PTR Address
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Memory=0x%x Size=%lu", Memory, Size);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL || Physical != ~0U);
    gcmDEBUG_VERIFY_ARGUMENT(Size > 0);
    gcmDEBUG_VERIFY_ARGUMENT(Info != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Address != gcvNULL);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_MAP_USER_MEMORY;
    iface.u.MapUserMemory.memory = gcmPTR_TO_UINT64(Memory);
    iface.u.MapUserMemory.physical = Physical;
    iface.u.MapUserMemory.size   = Size;

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &iface, gcmSIZEOF(iface),
                                   &iface, gcmSIZEOF(iface)));

    /* Return the info on success. */
    *Info    = gcmINT2PTR(iface.u.MapUserMemory.info);
    *Address = iface.u.MapUserMemory.address;

    /* Success. */
    gcmFOOTER_ARG("*Info=0x%x *Address=0x%08x", *Info, *Address);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_UnmapUserMemory
**
**  Unlock a user buffer and that was previously locked down by
**  gcoOS_MapUserMemory.
**
**  INPUT:
**
**      gctPOINTER Memory
**          Pointer to memory to unlock.
**
**      gctSIZE_T Size
**          Size in bytes of the memory to unlock.
**
**      gctPOINTER Info
**          Information record returned by gcoOS_MapUserMemory.
**
**      gctUINT32_PTR Address
**          The address returned by gcoOS_MapUserMemory.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_UnmapUserMemory(
    IN gcoOS Os,
    IN gctPOINTER Memory,
    IN gctSIZE_T Size,
    IN gctPOINTER Info,
    IN gctUINT32 Address
    )
{
    gcsHAL_INTERFACE iface;
    gceSTATUS status;

    gcmHEADER_ARG("Memory=0x%x Size=%lu Info=0x%x Address=0x%08x",
                  Memory, Size, Info, Address);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Memory != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Size > 0);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_UNMAP_USER_MEMORY;
    iface.u.UnmapUserMemory.memory  = gcmPTR_TO_UINT64(Memory);
    iface.u.UnmapUserMemory.size    = Size;
    iface.u.UnmapUserMemory.info    = gcmPTR2INT32(Info);
    iface.u.UnmapUserMemory.address = Address;

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &iface, gcmSIZEOF(iface),
                                   &iface, gcmSIZEOF(iface)));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_StrDup
**
**  Duplicate the given string by copying it into newly allocated memory.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctCONST_STRING String
**          Pointer to string to duplicate.
**
**  OUTPUT:
**
**      gctSTRING * Target
**          Pointer to variable holding the duplicated string address.
*/
gceSTATUS
gcoOS_StrDup(
    IN gcoOS Os,
    IN gctCONST_STRING String,
    OUT gctSTRING * Target
    )
{
    gctSIZE_T bytes;
    gctSTRING string;
    gceSTATUS status;

    gcmHEADER_ARG("String=0x%x", String);

    gcmDEBUG_VERIFY_ARGUMENT(String != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Target != gcvNULL);

    bytes = gcoOS_StrLen(String, gcvNULL);

    gcmONERROR(gcoOS_Allocate(gcvNULL, bytes + 1, (gctPOINTER *) &string));

    memcpy(string, String, bytes + 1);

    *Target = string;

    /* Success. */
    gcmFOOTER_ARG("*Target=0x%x", gcmOPT_VALUE(Target));
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_LoadLibrary
**
**  Load a library.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctCONST_STRING Library
**          Name of library to load.
**
**  OUTPUT:
**
**      gctHANDLE * Handle
**          Pointer to variable receiving the library handle.
*/
gceSTATUS
gcoOS_LoadLibrary(
    IN gcoOS Os,
    IN gctCONST_STRING Library,
    OUT gctHANDLE * Handle
    )
{
#if gcdSTATIC_LINK
    return gcvSTATUS_NOT_SUPPORTED;
#else
    gctSIZE_T length;
    gctSTRING library = gcvNULL;
    gceSTATUS status = gcvSTATUS_OK;

    gcmHEADER_ARG("Library=%s", gcmOPT_STRING(Library));

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Handle != gcvNULL);

    /* Reset the handle. */
    *Handle = gcvNULL;

    if (Library != gcvNULL)
    {
        /* Get the length of the library name. */
        length = strlen(Library);

        /* Test if the libray has ".so" at the end. */
        if (strcmp(Library + length - 3, ".so") != 0)
        {
            /* Allocate temporay string buffer. */
            gcmONERROR(gcoOS_Allocate(
                gcvNULL, length + 3 + 1, (gctPOINTER *) &library
                ));

            /* Copy the library name to the temporary string buffer. */
            strcpy(library, Library);

            /* Append the ".so" to the temporary string buffer. */
            strcat(library, ".so");

            /* Replace the library name. */
            Library = library;
        }

        *Handle = dlopen(Library, RTLD_NOW);

        /* Failed? */
        if (*Handle == gcvNULL)
        {
            gcmTRACE(
                gcvLEVEL_ERROR, "%s(%d): %s", __FUNCTION__, __LINE__, Library
                );

            gcmTRACE(
                gcvLEVEL_ERROR, "%s(%d): %s", __FUNCTION__, __LINE__, dlerror()
                );

            /* Library could not be loaded. */
            gcmONERROR(gcvSTATUS_NOT_FOUND);
        }
    }

OnError:
    /* Free the temporary string buffer. */
    if (library != gcvNULL)
    {
        gcmVERIFY_OK(gcmOS_SAFE_FREE(gcvNULL, library));
    }

    gcmFOOTER_ARG("*Handle=0x%x status=%d", *Handle, status);
    return status;
#endif
}

/*******************************************************************************
**
**  gcoOS_FreeLibrary
**
**  Unload a loaded library.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctHANDLE Handle
**          Handle of a loaded libarry.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_FreeLibrary(
    IN gcoOS Os,
    IN gctHANDLE Handle
    )
{
#if gcdSTATIC_LINK
    return gcvSTATUS_NOT_SUPPORTED;
#else
    gcmHEADER_ARG("Handle=0x%x", Handle);

#if !gcdBUILT_FOR_VALGRIND
    /* Free the library. */
    dlclose(Handle);
#endif

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
#endif
}

/*******************************************************************************
**
**  gcoOS_GetProcAddress
**
**  Get the address of a function inside a loaded library.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctHANDLE Handle
**          Handle of a loaded libarry.
**
**      gctCONST_STRING Name
**          Name of function to get the address of.
**
**  OUTPUT:
**
**      gctPOINTER * Function
**          Pointer to variable receiving the function pointer.
*/
gceSTATUS
gcoOS_GetProcAddress(
    IN gcoOS Os,
    IN gctHANDLE Handle,
    IN gctCONST_STRING Name,
    OUT gctPOINTER * Function
    )
{
#if gcdSTATIC_LINK
    return gcvSTATUS_NOT_SUPPORTED;
#else
    gceSTATUS status = gcvSTATUS_OK;

    gcmHEADER_ARG("Handle=0x%x Name=%s", Handle, gcmOPT_STRING(Name));

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Name != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Function != gcvNULL);

    /* Get the address of the function. */
    *Function = dlsym(Handle, Name);

    if (*Function == gcvNULL)
    {
        gcmTRACE(
            gcvLEVEL_WARNING,
            "%s(%d): Function %s not found.",
            __FUNCTION__, __LINE__, Name
            );

        /* Function could not be found. */
        status = gcvSTATUS_NOT_FOUND;
    }

    /* Success. */
    gcmFOOTER_ARG("*Function=0x%x status=%d", *Function, status);
    return status;
#endif
}

#if VIVANTE_PROFILER
gceSTATUS
gcoOS_ProfileStart(
    IN gcoOS Os
    )
{
    gcPLS.os->allocCount   = 0;
    gcPLS.os->allocSize    = 0;
    gcPLS.os->maxAllocSize = 0;
    gcPLS.os->freeCount    = 0;
    gcPLS.os->freeSize     = 0;

#if gcdGC355_MEM_PRINT
    gcPLS.os->oneRecording = 0;
    gcPLS.os->oneSize      = 0;
#endif

    return gcvSTATUS_OK;
}

gceSTATUS
gcoOS_ProfileEnd(
    IN gcoOS Os,
    IN gctCONST_STRING Title
    )
{
    gcmPRINT("08) System memory - maximum: %u \n", gcPLS.os->maxAllocSize);
    gcmPRINT("09) System memory - allocation count: %u \n", gcPLS.os->allocCount);
    gcmPRINT("10) System memory - allocation total: %u \n", gcPLS.os->allocSize);
    gcmPRINT("11) System memory - deallocation count: %u \n", gcPLS.os->freeCount);
    gcmPRINT("12) System memory - deallocation total: %u \n", gcPLS.os->freeSize);

    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_SetProfileSetting
**
**  Set Vivante profiler settings.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctBOOL Enable
**          Enable or Disable Vivante profiler.
**
**      gctCONST_STRING FileName
**          Specify FileName for storing profile data into.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_SetProfileSetting(
    IN gcoOS Os,
    IN gctBOOL Enable,
    IN gctCONST_STRING FileName
    )
{
    gcsHAL_INTERFACE iface;

    if (strlen(FileName) >= gcdMAX_PROFILE_FILE_NAME)
    {
        return gcvSTATUS_INVALID_ARGUMENT;
    }

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_SET_PROFILE_SETTING;
    iface.u.SetProfileSetting.enable = Enable;

    /* Call the kernel. */
    return gcoOS_DeviceControl(gcvNULL,
                               IOCTL_GCHAL_INTERFACE,
                               &iface, gcmSIZEOF(iface),
                               &iface, gcmSIZEOF(iface));
}
#endif

gceSTATUS
gcoOS_Compact(
    IN gcoOS Os
    )
{
    return gcvSTATUS_OK;
}

/*----------------------------------------------------------------------------*/
/*----------------------------------- Atoms ----------------------------------*/

struct gcsATOM
{
    /* Counter. */
    gctINT32 counter;

#if !gcdBUILTIN_ATOMIC_FUNCTIONS
    /* Mutex. */
    pthread_mutex_t mutex;
#endif
};

/* Create an atom. */
gceSTATUS
gcoOS_AtomConstruct(
    IN gcoOS Os,
    OUT gcsATOM_PTR * Atom
    )
{
    gceSTATUS status;
    gcsATOM_PTR atom = gcvNULL;

    gcmHEADER();

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Atom != gcvNULL);

    do
    {
        /* Allocate memory for the atom. */
        gcmERR_BREAK(gcoOS_Allocate(gcvNULL,
                                    gcmSIZEOF(struct gcsATOM),
                                    (gctPOINTER *) &atom));

        /* Initialize the atom to 0. */
        atom->counter = 0;

#if !gcdBUILTIN_ATOMIC_FUNCTIONS
        if (pthread_mutex_init(&atom->mutex, gcvNULL) != 0)
        {
            status = gcvSTATUS_OUT_OF_RESOURCES;
            break;
        }
#endif

        /* Return pointer to atom. */
        *Atom = atom;

        /* Success. */
        gcmFOOTER_ARG("*Atom=%p", *Atom);
        return gcvSTATUS_OK;
    }
    while (gcvFALSE);

    /* Free the atom. */
    if (atom != gcvNULL)
    {
        gcmOS_SAFE_FREE(gcvNULL, atom);
    }

    /* Return error status. */
    gcmFOOTER();
    return status;
}

/* Destroy an atom. */
gceSTATUS
gcoOS_AtomDestroy(
    IN gcoOS Os,
    IN gcsATOM_PTR Atom
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Atom=0x%x", Atom);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Atom != gcvNULL);

    /* Free the atom. */
    status = gcmOS_SAFE_FREE(gcvNULL, Atom);

    /* Return the status. */
    gcmFOOTER();
    return status;
}

gceSTATUS
gcoOS_AtomGet(
    IN gcoOS Os,
    IN gcsATOM_PTR Atom,
    OUT gctINT32_PTR Value
    )
{
    gcmHEADER_ARG("Atom=0x%0x", Atom);

    /* Verify the arguments. */
    gcmVERIFY_ARGUMENT(Atom != gcvNULL);

    /* Get the atom value. */
    *Value = Atom->counter;

    /* Success. */
    gcmFOOTER_ARG("*Value=%d", *Value);
    return gcvSTATUS_OK;
}

gceSTATUS
gcoOS_AtomSet(
    IN gcoOS Os,
    IN gcsATOM_PTR Atom,
    IN gctINT32 Value
    )
{
    gcmHEADER_ARG("Atom=0x%0x Value=%d", Atom, Value);

    /* Verify the arguments. */
    gcmVERIFY_ARGUMENT(Atom != gcvNULL);

    /* Set the atom value. */
    Atom->counter = Value;

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;
}

/* Increment an atom. */
gceSTATUS
gcoOS_AtomIncrement(
    IN gcoOS Os,
    IN gcsATOM_PTR Atom,
    OUT gctINT32_PTR OldValue OPTIONAL
    )
{
    gctINT32 value;

    gcmHEADER_ARG("Atom=0x%x", Atom);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Atom != gcvNULL);

    /* Increment the atom's counter. */
#if gcdBUILTIN_ATOMIC_FUNCTIONS
    value = __sync_fetch_and_add(&Atom->counter, 1);
#else
    /* Lock the mutex. */
    pthread_mutex_lock(&Atom->mutex);

    /* Get original value. */
    value = Atom->counter;

    /* Add given value. */
    Atom->counter += 1;

    /* Unlock the mutex. */
    pthread_mutex_unlock(&Atom->mutex);
#endif

    if (OldValue != gcvNULL)
    {
        /* Return the original value to the caller. */
        *OldValue = value;
    }

    /* Success. */
    gcmFOOTER_ARG("*OldValue=%d", gcmOPT_VALUE(OldValue));
    return gcvSTATUS_OK;
}

/* Decrement an atom. */
gceSTATUS
gcoOS_AtomDecrement(
    IN gcoOS Os,
    IN gcsATOM_PTR Atom,
    OUT gctINT32_PTR OldValue OPTIONAL
    )
{
    gctINT32 value;

    gcmHEADER_ARG("Atom=0x%x", Atom);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Atom != gcvNULL);

    /* Decrement the atom's counter. */
#if gcdBUILTIN_ATOMIC_FUNCTIONS
    value = __sync_fetch_and_sub(&Atom->counter, 1);
#else
    /* Lock the mutex. */
    pthread_mutex_lock(&Atom->mutex);

    /* Get original value. */
    value = Atom->counter;

    /* Subtract given value. */
    Atom->counter -= 1;

    /* Unlock the mutex. */
    pthread_mutex_unlock(&Atom->mutex);
#endif

    if (OldValue != gcvNULL)
    {
        /* Return the original value to the caller. */
        *OldValue = value;
    }

    /* Success. */
    gcmFOOTER_ARG("*OldValue=%d", gcmOPT_VALUE(OldValue));
    return gcvSTATUS_OK;
}

gctHANDLE
gcoOS_GetCurrentProcessID(
    void
    )
{
    return (gctHANDLE)(gctUINTPTR_T) getpid();
}

gctHANDLE
gcoOS_GetCurrentThreadID(
    void
    )
{
    return (gctHANDLE) pthread_self();
}

/*----------------------------------------------------------------------------*/
/*----------------------------------- Time -----------------------------------*/

/*******************************************************************************
**
**  gcoOS_GetTicks
**
**  Get the number of milliseconds since the system started.
**
**  INPUT:
**
**  OUTPUT:
**
*/
gctUINT32
gcoOS_GetTicks(
    void
    )
{
    struct timeval tv;

    /* Return the time of day in milliseconds. */
    gettimeofday(&tv, 0);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

/*******************************************************************************
**
**  gcoOS_GetTime
**
**  Get the number of microseconds since 1970/1/1.
**
**  INPUT:
**
**  OUTPUT:
**
**      gctUINT64_PTR Time
**          Pointer to a variable to get time.
**
*/
gceSTATUS
gcoOS_GetTime(
    OUT gctUINT64_PTR Time
    )
{
    struct timeval tv;

    /* Return the time of day in microseconds. */
    gettimeofday(&tv, 0);
    *Time = (tv.tv_sec * 1000000) + tv.tv_usec;
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gcoOS_GetCPUTime
**
**  Get CPU time usage in microseconds.
**
**  INPUT:
**
**  OUTPUT:
**
**      gctUINT64_PTR CPUTime
**          Pointer to a variable to get CPU time usage.
**
*/
gceSTATUS
gcoOS_GetCPUTime(
    OUT gctUINT64_PTR CPUTime
    )
{
    struct rusage usage;

    /* Return CPU time in microseconds. */
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        *CPUTime  = usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec;
        *CPUTime += usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
        return gcvSTATUS_OK;
    }
    else
    {
        *CPUTime = 0;
        return gcvSTATUS_INVALID_ARGUMENT;
    }
}

/*******************************************************************************
**
**  gcoOS_GetMemoryUsage
**
**  Get current processes resource usage.
**
**  INPUT:
**
**  OUTPUT:
**
**      gctUINT32_PTR MaxRSS
**          Total amount of resident set memory used.
**          The value will be in terms of memory pages used.
**
**      gctUINT32_PTR IxRSS
**          Total amount of memory used by the text segment
**          in kilobytes multiplied by the execution-ticks.
**
**      gctUINT32_PTR IdRSS
**          Total amount of private memory used by a process
**          in kilobytes multiplied by execution-ticks.
**
**      gctUINT32_PTR IsRSS
**          Total amount of memory used by the stack in
**          kilobytes multiplied by execution-ticks.
**
*/
gceSTATUS
gcoOS_GetMemoryUsage(
    OUT gctUINT32_PTR MaxRSS,
    OUT gctUINT32_PTR IxRSS,
    OUT gctUINT32_PTR IdRSS,
    OUT gctUINT32_PTR IsRSS
    )
{
    struct rusage usage;

    /* Return memory usage. */
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        *MaxRSS = usage.ru_maxrss;
        *IxRSS  = usage.ru_ixrss;
        *IdRSS  = usage.ru_idrss;
        *IsRSS  = usage.ru_isrss;
        return gcvSTATUS_OK;
    }
    else
    {
        *MaxRSS = 0;
        *IxRSS  = 0;
        *IdRSS  = 0;
        *IsRSS  = 0;
        return gcvSTATUS_INVALID_ARGUMENT;
    }
}

/*******************************************************************************
**
**  gcoOS_ReadRegister
**
**  Read data from a register.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctUINT32 Address
**          Address of register.
**
**  OUTPUT:
**
**      gctUINT32 * Data
**          Pointer to a variable that receives the data read from the register.
*/
gceSTATUS
gcoOS_ReadRegister(
    IN gcoOS Os,
    IN gctUINT32 Address,
    OUT gctUINT32 * Data
    )
{
    gcsHAL_INTERFACE iface;
    gceSTATUS status;

    gcmHEADER_ARG("Address=0x%x", Address);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Data != gcvNULL);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_READ_REGISTER;
    iface.u.ReadRegisterData.address = Address;
    iface.u.ReadRegisterData.data    = 0xDEADDEAD;

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &iface, gcmSIZEOF(iface),
                                   &iface, gcmSIZEOF(iface)));

    /* Return the Data on success. */
    *Data = iface.u.ReadRegisterData.data;

    /* Success. */
    gcmFOOTER_ARG("*Data=0x%08x", *Data);
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_WriteRegister
**
**  Write data to a register.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctUINT32 Address
**          Address of register.
**
**      gctUINT32 Data
**          Data for register.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_WriteRegister(
    IN gcoOS Os,
    IN gctUINT32 Address,
    IN gctUINT32 Data
    )
{
    gcsHAL_INTERFACE iface;
    gceSTATUS status;

    gcmHEADER_ARG("Address=0x%x Data=0x%08x", Address, Data);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_WRITE_REGISTER;
    iface.u.WriteRegisterData.address = Address;
    iface.u.WriteRegisterData.data    = Data;

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &iface, gcmSIZEOF(iface),
                                   &iface, gcmSIZEOF(iface)));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

static gceSTATUS
gcoOS_Cache(
    IN gctUINT32 Node,
    IN gctPOINTER Logical,
    IN gctSIZE_T Bytes,
    IN gceCACHEOPERATION Operation
    )
{
    gcsHAL_INTERFACE ioctl;
    gceSTATUS status;

    gcmHEADER_ARG("Node=0x%x Logical=0x%x Bytes=%u Operation=%d",
                  Node, Logical, Bytes, Operation);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Logical != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(Bytes > 0);

    /* Set up the cache. */
    ioctl.command            = gcvHAL_CACHE;
    ioctl.u.Cache.operation  = Operation;
    ioctl.u.Cache.node       = Node;
    ioctl.u.Cache.logical    = gcmPTR_TO_UINT64(Logical);
    ioctl.u.Cache.bytes      = Bytes;

    /* Call the kernel. */
    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &ioctl, gcmSIZEOF(ioctl),
                                   &ioctl, gcmSIZEOF(ioctl)));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**  gcoOS_CacheClean
**
**  Clean the cache for the specified addresses.  The GPU is going to need the
**  data.  If the system is allocating memory as non-cachable, this function can
**  be ignored.
**
**  ARGUMENTS:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctUINT32 Node
**          Pointer to the video memory node that needs to be flushed.
**
**      gctPOINTER Logical
**          Logical address to flush.
**
**      gctSIZE_T Bytes
**          Size of the address range in bytes to flush.
*/
gceSTATUS
gcoOS_CacheClean(
    IN gcoOS Os,
    IN gctUINT32 Node,
    IN gctPOINTER Logical,
    IN gctSIZE_T Bytes
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Node=0x%x Logical=0x%x Bytes=%u",
                  Node, Logical, Bytes);

    /* Call common code. */
    gcmONERROR(gcoOS_Cache(Node, Logical, Bytes, gcvCACHE_CLEAN));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**  gcoOS_CacheFlush
**
**  Flush the cache for the specified addresses and invalidate the lines as
**  well.  The GPU is going to need and modify the data.  If the system is
**  allocating memory as non-cachable, this function can be ignored.
**
**  ARGUMENTS:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctUINT32 Node
**          Pointer to the video memory node that needs to be flushed.
**
**      gctPOINTER Logical
**          Logical address to flush.
**
**      gctSIZE_T Bytes
**          Size of the address range in bytes to flush.
*/
gceSTATUS
gcoOS_CacheFlush(
    IN gcoOS Os,
    IN gctUINT32 Node,
    IN gctPOINTER Logical,
    IN gctSIZE_T Bytes
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Node=0x%x Logical=0x%x Bytes=%u",
                  Node, Logical, Bytes);

    /* Call common code. */
    gcmONERROR(gcoOS_Cache(Node, Logical, Bytes, gcvCACHE_FLUSH));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**  gcoOS_CacheInvalidate
**
**  Invalidate the lines. The GPU is going modify the data.  If the system is
**  allocating memory as non-cachable, this function can be ignored.
**
**  ARGUMENTS:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctUINT32 Node
**          Pointer to the video memory node that needs to be invalidated.
**
**      gctPOINTER Logical
**          Logical address to flush.
**
**      gctSIZE_T Bytes
**          Size of the address range in bytes to invalidated.
*/
gceSTATUS
gcoOS_CacheInvalidate(
    IN gcoOS Os,
    IN gctUINT32 Node,
    IN gctPOINTER Logical,
    IN gctSIZE_T Bytes
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Node=0x%x Logical=0x%x Bytes=%u",
                  Node, Logical, Bytes);

    /* Call common code. */
    gcmONERROR(gcoOS_Cache(Node, Logical, Bytes, gcvCACHE_INVALIDATE));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**  gcoOS_MemoryBarrier
**  Make sure the CPU has executed everything up to this point and the data got
**  written to the specified pointer.
**  ARGUMENTS:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctPOINTER Logical
**          Logical address to flush.
**
*/
gceSTATUS
gcoOS_MemoryBarrier(
    IN gcoOS Os,
    IN gctPOINTER Logical
    )
{
    gceSTATUS status;

    gcmHEADER_ARG("Logical=0x%x", Logical);

    /* Call common code. */
    gcmONERROR(gcoOS_Cache(0, Logical, 1, gcvCACHE_MEMORY_BARRIER));

    /* Success. */
    gcmFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}


/*----------------------------------------------------------------------------*/
/*----- Profiling ------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/

#if VIVANTE_PROFILER || gcdENABLE_PROFILING
gceSTATUS
gcoOS_GetProfileTick(
    OUT gctUINT64_PTR Tick
    )
{
#ifdef CLOCK_MONOTONIC
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);

    *Tick = time.tv_nsec + time.tv_sec * 1000000000ULL;

    return gcvSTATUS_OK;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}

gceSTATUS
gcoOS_QueryProfileTickRate(
    OUT gctUINT64_PTR TickRate
    )
{
#ifdef CLOCK_MONOTONIC
    struct timespec res;

    clock_getres(CLOCK_MONOTONIC, &res);

    *TickRate = res.tv_nsec + res.tv_sec * 1000000000ULL;

    return gcvSTATUS_OK;
#else
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}

/*******************************************************************************
**  gcoOS_ProfileDB
**
**  Manage the profile database.
**
**  The database layout is very simple:
**
**      <RecordID> (1 byte) <record data>
**
**  The <RecordID> can be one of the following values:
**
**      1       Initialize a new function to be profiled. Followed by the NULL-
**              terminated name of the function, 4 bytes of the function ID and
**              8 bytes of the profile tick.
**      2       Enter a function to be profiled. Followed by 4 bytes of function
**              ID and 8 bytes of the profile tick.
**      3       Exit a function to be profiled. Followed by 8 bytes of the
**              profile tick.
**
**  There are three options to manage the profile database. One would be to
**  enter a function that needs to be profiled. This is specified with both
**  <Function> and <Initialized> pointers initialized to some value. Here
**  <Function> is pointing to a string with the function name and <Initialized>
**  is pointing to a boolean value that tells the profiler whether this function
**  has been initialized or not.
**
**  The second option would be to exit a function that was being profiled. This
**  is specified by <Function> pointing to a string with the function name and
**  <Initialized> set to gcvNULL.
**
**  The third and last option is to flush the profile database. This is
**  specified with <Function> set to gcvNULL.
**
***** PARAMETERS
**
**  Function
**
**      Pointer to a string with the function name being profiled or gcvNULL to
**      flush the profile database.
**
**  Initialized
**
**      Pointer to a boolean variable that informs the profiler if the entry of
**      a function has been initialized or not, or gcvNULL to mark the exit of a
**      function being profiled.
*/
void
gcoOS_ProfileDB(
    IN gctCONST_STRING Function,
    IN OUT gctBOOL_PTR Initialized
    )
{
    gctUINT64 nanos;
    static gctUINT8_PTR profileBuffer = gcvNULL;
    static gctSIZE_T profileSize, profileThreshold, totalBytes;
    static gctUINT32 profileIndex;
    static gctINT profileLevel;
    static FILE * profileDB = gcvNULL;
    int len, bytes;

    /* Check if we need to flush the profile database. */
    if (Function == gcvNULL)
    {
        if (profileBuffer != gcvNULL)
        {
            /* Check of the profile database exists. */
            if (profileIndex > 0)
            {
                if (profileDB == gcvNULL)
                {
                    /* Open the profile database file. */
                    profileDB = fopen("profile.database", "wb");
                }

                if (profileDB != gcvNULL)
                {
                    /* Write the profile database to the file. */
                    totalBytes += fwrite(profileBuffer,
                                         1, profileIndex,
                                         profileDB);
                }
            }

            if (profileDB != gcvNULL)
            {
                /* Convert the size of the profile database into a nice human
                ** readable format. */
                char buf[] = "#,###,###,###";
                int i;

                i = strlen(buf);
                while ((totalBytes != 0) && (i > 0))
                {
                    if (buf[--i] == ',') --i;

                    buf[i]      = '0' + (totalBytes % 10);
                    totalBytes /= 10;
                }

                /* Print the size of the profile database. */
                gcmPRINT("Closing the profile database: %s bytes.", &buf[i]);

                /* Close the profile database file. */
                fclose(profileDB);
                profileDB = gcvNULL;
            }

            /* Destroy the profile database. */
            free(profileBuffer);
            profileBuffer = gcvNULL;
        }
    }

    /* Check if we have to enter a function. */
    else if (Initialized != gcvNULL)
    {
        /* Check if the profile database exists or not. */
        if (profileBuffer == gcvNULL)
        {
            /* Allocate the profile database. */
            for (profileSize = 32 << 20;
                 profileSize > 0;
                 profileSize -= 1 << 20
            )
            {
                profileBuffer = malloc(profileSize);

                if (profileBuffer != gcvNULL)
                {
                    break;
                }
            }

            if (profileBuffer == gcvNULL)
            {
                /* Sorry - no memory. */
                gcmPRINT("Cannot create the profile buffer!");
                return;
            }

            /* Reset the profile database. */
            profileThreshold = gcmMIN(profileSize / 2, 4 << 20);
            totalBytes       = 0;
            profileIndex     = 0;
            profileLevel     = 0;
        }

        /* Increment the profile level. */
        ++profileLevel;

        /* Determine number of bytes to copy. */
        len   = strlen(Function) + 1;
        bytes = 1 + (*Initialized ? 0 : len) + 4 + 8;

        /* Check if the profile database has enough space. */
        if (profileIndex + bytes > profileSize)
        {
            gcmPRINT("PROFILE ENTRY: index=%lu size=%lu bytes=%d level=%d",
                     profileIndex, profileSize, bytes, profileLevel);

            if (profileDB == gcvNULL)
            {
                /* Open the profile database file. */
                profileDB = fopen("profile.database", "wb");
            }

            if (profileDB != gcvNULL)
            {
                /* Write the profile database to the file. */
                totalBytes += fwrite(profileBuffer, 1, profileIndex, profileDB);
            }

            /* Empty the profile databse. */
            profileIndex = 0;
        }

        /* Check whether this function is initialized or not. */
        if (*Initialized)
        {
            /* Already initialized - don't need to save name. */
            profileBuffer[profileIndex] = 2;
        }
        else
        {
            /* Not yet initialized, save name as well. */
            profileBuffer[profileIndex] = 1;
            memcpy(profileBuffer + profileIndex + 1, Function, len);
            profileIndex += len;

            /* Mark function as initialized. */
            *Initialized = gcvTRUE;
        }

        /* Copy the function ID into the profile database. */
        memcpy(profileBuffer + profileIndex + 1, &Initialized, 4);

        /* Get the profile tick. */
        gcoOS_GetProfileTick(&nanos);

        /* Copy the profile tick into the profile database. */
        memcpy(profileBuffer + profileIndex + 5, &nanos, 8);
        profileIndex += 1 + 4 + 8;
    }

    /* Exit a function, check whether the profile database is around. */
    else if (profileBuffer != gcvNULL)
    {
        /* Get the profile tick. */
        gcoOS_GetProfileTick(&nanos);

        /* Check if the profile database has enough space. */
        if (profileIndex + 1 + 8 > profileSize)
        {
            gcmPRINT("PROFILE EXIT: index=%lu size=%lu bytes=%d level=%d",
                     profileIndex, profileSize, 1 + 8, profileLevel);

            if (profileDB == gcvNULL)
            {
                /* Open the profile database file. */
                profileDB = fopen("profile.database", "wb");
            }

            if (profileDB != gcvNULL)
            {
                /* Write the profile database to the file. */
                totalBytes += fwrite(profileBuffer, 1, profileIndex, profileDB);
            }

            /* Empty the profile databse. */
            profileIndex = 0;
        }

        /* Copy the profile tick into the profile database. */
        profileBuffer[profileIndex] = 3;
        memcpy(profileBuffer + profileIndex + 1, &nanos, 8);
        profileIndex += 1 + 8;

        /* Decrease the profile level and check whether the profile database is
        ** getting too big if we exit a top-level function. */
        if ((--profileLevel == 0)
        &&  (profileSize - profileIndex < profileThreshold)
        )
        {
            if (profileDB == gcvNULL)
            {
                /* Open the profile database file. */
                profileDB = fopen("profile.database", "wb");
            }

            if (profileDB != gcvNULL)
            {
                /* Write the profile database to the file. */
                totalBytes += fwrite(profileBuffer, 1, profileIndex, profileDB);

                /* Flush the file now. */
                fflush(profileDB);
            }

            /* Empty the profile databse. */
            profileIndex = 0;
        }
    }
}
#endif

/******************************************************************************\
******************************* Signal Management ******************************
\******************************************************************************/

#undef _GC_OBJ_ZONE
#define _GC_OBJ_ZONE    gcvZONE_SIGNAL

/*******************************************************************************
**
**  gcoOS_CreateSignal
**
**  Create a new signal.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctBOOL ManualReset
**          If set to gcvTRUE, gcoOS_Signal with gcvFALSE must be called in
**          order to set the signal to nonsignaled state.
**          If set to gcvFALSE, the signal will automatically be set to
**          nonsignaled state by gcoOS_WaitSignal function.
**
**  OUTPUT:
**
**      gctSIGNAL * Signal
**          Pointer to a variable receiving the created gctSIGNAL.
*/
gceSTATUS
gcoOS_CreateSignal(
    IN gcoOS Os,
    IN gctBOOL ManualReset,
    OUT gctSIGNAL * Signal
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("ManualReset=%d", ManualReset);

    /* Verify the arguments. */
    gcmDEBUG_VERIFY_ARGUMENT(Signal != gcvNULL);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_USER_SIGNAL;
    iface.u.UserSignal.command     = gcvUSER_SIGNAL_CREATE;
    iface.u.UserSignal.manualReset = ManualReset;

    /* Call kernel driver. */
    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &iface, gcmSIZEOF(iface),
                                   &iface, gcmSIZEOF(iface)));

    *Signal = (gctSIGNAL)(gctUINTPTR_T) iface.u.UserSignal.id;

    gcmFOOTER_ARG("*Signal=0x%x", *Signal);
    return gcvSTATUS_OK;

OnError:
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_DestroySignal
**
**  Destroy a signal.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctSIGNAL Signal
**          Pointer to the gctSIGNAL.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_DestroySignal(
    IN gcoOS Os,
    IN gctSIGNAL Signal
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Signal=0x%x", Signal);

    gcmTRACE_ZONE(
        gcvLEVEL_VERBOSE, gcvZONE_OS,
        "gcoOS_DestroySignal: signal->%d.",
        (gctINT)(gctUINTPTR_T)Signal
        );

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_USER_SIGNAL;
    iface.u.UserSignal.command = gcvUSER_SIGNAL_DESTROY;
    iface.u.UserSignal.id      = (gctINT)(gctUINTPTR_T) Signal;

    /* Call kernel driver. */
    status = gcoOS_DeviceControl(gcvNULL,
                                 IOCTL_GCHAL_INTERFACE,
                                 &iface, gcmSIZEOF(iface),
                                 &iface, gcmSIZEOF(iface));

    /* Success. */
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_Signal
**
**  Set a state of the specified signal.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctSIGNAL Signal
**          Pointer to the gctSIGNAL.
**
**      gctBOOL State
**          If gcvTRUE, the signal will be set to signaled state.
**          If gcvFALSE, the signal will be set to nonsignaled state.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_Signal(
    IN gcoOS Os,
    IN gctSIGNAL Signal,
    IN gctBOOL State
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Signal=0x%x State=%d", Signal, State);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_USER_SIGNAL;
    iface.u.UserSignal.command = gcvUSER_SIGNAL_SIGNAL;
    iface.u.UserSignal.id      = (gctINT)(gctUINTPTR_T) Signal;
    iface.u.UserSignal.state   = State;

    /* Call kernel driver. */
    status = gcoOS_DeviceControl(gcvNULL,
                                 IOCTL_GCHAL_INTERFACE,
                                 &iface, gcmSIZEOF(iface),
                                 &iface, gcmSIZEOF(iface));

    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_WaitSignal
**
**  Wait for a signal to become signaled.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to an gcoOS object.
**
**      gctSIGNAL Signal
**          Pointer to the gctSIGNAL.
**
**      gctUINT32 Wait
**          Number of milliseconds to wait.
**          Pass the value of gcvINFINITE for an infinite wait.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_WaitSignal(
    IN gcoOS Os,
    IN gctSIGNAL Signal,
    IN gctUINT32 Wait
    )
{
#if gcdNULL_DRIVER
    return gcvSTATUS_OK;
#else
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Signal=0x%x Wait=%u", Signal, Wait);

    /* Initialize the gcsHAL_INTERFACE structure. */
    iface.command = gcvHAL_USER_SIGNAL;
    iface.u.UserSignal.command = gcvUSER_SIGNAL_WAIT;
    iface.u.UserSignal.id      = (gctINT)(gctUINTPTR_T) Signal;
    iface.u.UserSignal.wait    = Wait;

    /* Call kernel driver. */
    status = gcoOS_DeviceControl(gcvNULL,
                                 IOCTL_GCHAL_INTERFACE,
                                 &iface, gcmSIZEOF(iface),
                                 &iface, gcmSIZEOF(iface));

    gcmFOOTER_ARG("Signal=0x%x status=%d", Signal, status);
    return status;
#endif
}

/*******************************************************************************
**
**  gcoOS_MapSignal
**
**  Map a signal from another process.
**
**  INPUT:
**
**      gctSIGNAL  RemoteSignal
**
**  OUTPUT:
**
**      gctSIGNAL * LocalSignal
**          Pointer to a variable receiving the created gctSIGNAL.
*/
gceSTATUS
gcoOS_MapSignal(
    IN gctSIGNAL  RemoteSignal,
    OUT gctSIGNAL * LocalSignal
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("RemoteSignal=%d", RemoteSignal);

    gcmDEBUG_VERIFY_ARGUMENT(RemoteSignal != gcvNULL);
    gcmDEBUG_VERIFY_ARGUMENT(LocalSignal != gcvNULL);

    iface.command = gcvHAL_USER_SIGNAL;
    iface.u.UserSignal.command  = gcvUSER_SIGNAL_MAP;
    iface.u.UserSignal.id = (gctINT)(gctUINTPTR_T) RemoteSignal;

    gcmONERROR(gcoOS_DeviceControl(gcvNULL,
                                   IOCTL_GCHAL_INTERFACE,
                                   &iface, gcmSIZEOF(iface),
                                   &iface, gcmSIZEOF(iface)));

    *LocalSignal = (gctSIGNAL)(gctUINTPTR_T) iface.u.UserSignal.id;

    gcmFOOTER_ARG("*LocalSignal=0x%x", *LocalSignal);
    return gcvSTATUS_OK;

OnError:
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_UnmapSignal
**
**  Unmap a signal mapped from another process.
**
**  INPUT:
**
**      gctSIGNAL Signal
**          Pointer to the gctSIGNAL.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_UnmapSignal(
    IN gctSIGNAL Signal
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Signal=0x%x", Signal);

    gcmDEBUG_VERIFY_ARGUMENT(Signal != gcvNULL);

    gcmTRACE_ZONE(
        gcvLEVEL_VERBOSE, gcvZONE_OS,
        "gcoOS_UnmapSignal: signal->%d.",
        (gctINT)(gctUINTPTR_T)Signal
        );

    iface.command = gcvHAL_USER_SIGNAL;
    iface.u.UserSignal.command = gcvUSER_SIGNAL_UNMAP;
    iface.u.UserSignal.id      = (gctINT)(gctUINTPTR_T) Signal;

    status = gcoOS_DeviceControl(gcvNULL,
                                 IOCTL_GCHAL_INTERFACE,
                                 &iface, gcmSIZEOF(iface),
                                 &iface, gcmSIZEOF(iface));

    gcmFOOTER();
    return status;
}


void _SignalHandlerForSIGFPEWhenSignalCodeIs0(
    int sig_num,
    siginfo_t * info,
    void * ucontext
    )
{
    gctINT signalCode;

    signalCode = ((info->si_code) & 0xffff);
    if (signalCode == 0)
    {
        /* simply ignore the signal, this is a temporary fix for bug 4203 */
        return;
    }

    /* Let OS handle the signal */
    gcoOS_Print("Process got signal (%d). To further debug the issue, you should run in debug mode", sig_num);
    signal (sig_num, SIG_DFL);
    raise (sig_num);
    return;
}

/*******************************************************************************
**
**  gcoOS_AddSignalHandler
**
**  Adds Signal handler depending on Signal Handler Type
**
**  INPUT:
**
**      gceSignalHandlerType SignalHandlerType
**          Type of handler to be added
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gcoOS_AddSignalHandler (
    IN gceSignalHandlerType SignalHandlerType
    )
{
    gceSTATUS status = gcvSTATUS_OK;

    gcmHEADER_ARG("SignalHandlerType=0x%x", SignalHandlerType);

    switch(SignalHandlerType)
    {
    case gcvHANDLE_SIGFPE_WHEN_SIGNAL_CODE_IS_0:
        {
#if gcdDEBUG
                /* Handler will not be registered in debug mode*/
                gcmTRACE(
                    gcvLEVEL_INFO,
                    "%s(%d): Will not register signal handler for type gcvHANDLE_SIGFPE_WHEN_SIGNAL_CODE_IS_0 in debug mode",
                    __FUNCTION__, __LINE__
                    );
#else
            struct sigaction temp;
            struct sigaction sigact;
            sigaction (SIGFPE, NULL, &temp);
            if (temp.sa_handler != (void *)_SignalHandlerForSIGFPEWhenSignalCodeIs0)
            {
                sigact.sa_handler = (void *)_SignalHandlerForSIGFPEWhenSignalCodeIs0;
                sigact.sa_flags = SA_RESTART | SA_SIGINFO;
                sigemptyset(&sigact.sa_mask);
                sigaction(SIGFPE, &sigact, (struct sigaction *)NULL);
            }
#endif
            break;
        }

    default:
        {
            /* Unknown handler type */
            gcmTRACE(
                gcvLEVEL_ERROR,
                "%s(%d): Cannot register a signal handler for type 0x%0x",
                __FUNCTION__, __LINE__, SignalHandlerType
                );
            break;
        }

    }
    gcmFOOTER();
    return status;
}

/*******************************************************************************
**
**  gcoOS_DetectProcessByNamePid
**
**  Detect if the given process is the executable specified.
**
**  INPUT:
**
**      gctCONST_STRING Name
**          Name (full or partial) of executable.
**
**      gctHANDLE Pid
**          Process id.
**
**  OUTPUT:
**
**      Nothing.
**
**
**  RETURN:
**
**      gcvSTATUS_TRUE
**              if process is as specified by Name parameter.
**      gcvSTATUS_FALSE
**              Otherwise.
**
*/
gceSTATUS
gcoOS_DetectProcessByNamePid(
    IN gctCONST_STRING Name,
    IN gctHANDLE Pid
    )
{
    gceSTATUS status = gcvSTATUS_FALSE;
    gctFILE _handle = 0;
    gctUINT offset = 0;
    gctSIZE_T bytesRead = 0;
    gctSTRING pos = gcvNULL;
    char procEntryName[128];
    char procEntry[128];

    gcmHEADER_ARG("Name=%s Pid=%d", Name, Pid);

    /* Construct the proc cmdline entry */
    gcmONERROR(gcoOS_PrintStrSafe(procEntryName,
                    gcmSIZEOF(procEntryName),
                    &offset,
                    "/proc/%d/cmdline",
                    Pid));

    offset = 0;

    /* Open the procfs entry */
    gcmONERROR(gcoOS_Open(gcvNULL,
                   procEntryName,
                   gcvFILE_READ,
                   &_handle));


    gcmONERROR(gcoOS_Read(gcvNULL,
                            _handle,
                            gcmSIZEOF(procEntry)-1,
                            procEntry,
                            &bytesRead));

    procEntry[bytesRead] = '\0';

    gcmONERROR(gcoOS_StrStr((gctCONST_STRING)procEntry, Name, &pos));

    if (pos)
    {
        status = gcvSTATUS_TRUE;
    }

OnError:
    if(_handle)
    {
        gcoOS_Close(gcvNULL, _handle);
    }

    if (gcmIS_ERROR(status))
    {
        status = gcvSTATUS_FALSE;
    }

    /* Return the status. */
    gcmFOOTER();
    return status;
}


/*******************************************************************************
**
**  gcoOS_DetectProcessByName
**
**  Detect if the current process is the executable specified.
**
**  INPUT:
**
**      gctCONST_STRING Name
**          Name (full or partial) of executable.
**
**  OUTPUT:
**
**      Nothing.
**
**
**  RETURN:
**
**      gcvSTATUS_TRUE
**              if process is as specified by Name parameter.
**      gcvSTATUS_FALSE
**              Otherwise.
**
*/
gceSTATUS
gcoOS_DetectProcessByName(
    IN gctCONST_STRING Name
    )
{
    gceSTATUS status = gcvSTATUS_FALSE;
    gcmHEADER_ARG("Name=%s", Name);

    status = gcoOS_DetectProcessByNamePid(Name, gcoOS_GetCurrentProcessID());

    gcmFOOTER();
    return status;
}

gceSTATUS
gcoOS_DetectProcessByEncryptedName(
    IN gctCONST_STRING Name
    )
{
    gceSTATUS status = gcvSTATUS_FALSE;
    gctCHAR *p, buff[1024];
    p = buff;

    gcmONERROR(gcoOS_StrCopySafe(buff, gcmCOUNTOF(buff), Name));

    while (*p)
    {
        *p = ~(*p);
        p++;
    }

    status = gcoOS_DetectProcessByName(buff);

OnError:
    if (gcmIS_ERROR(status))
    {
        status = gcvSTATUS_FALSE;
    }

    return status;
}

#if defined(ANDROID)
static gceSTATUS
ParseSymbol(
    IN gctCONST_STRING Library,
    IN gcoOS_SymbolsList Symbols
    )
{
    gceSTATUS status = gcvSTATUS_FALSE;
    int fp;
    Elf32_Ehdr ehdr;
    Elf32_Shdr shdr;
    Elf32_Sym  sym;
    char secNameTab[2048];
    char *symNameTab = gcvNULL;
    int i = 0, j = 0, size_dynsym = 0, match = 0;
    unsigned int offset_dynsym = 0, offset_dynstr = 0, size_dynstr = 0;

    fp = open(Library, O_RDONLY);
    if (fp == -1)
        return gcvSTATUS_FALSE;

    if (read(fp, &ehdr, sizeof(Elf32_Ehdr)) == 0)
    {
        close(fp);
        return gcvSTATUS_FALSE;
    }

    if (ehdr.e_type != ET_DYN)
    {
        close(fp);
        return gcvSTATUS_FALSE;
    }

    if (lseek(fp, 0, SEEK_END) > 1024 * 1024 * 10)
    {
        close(fp);
        return gcvSTATUS_SKIP;
    }

    lseek(fp, ehdr.e_shoff + ehdr.e_shstrndx * 40, SEEK_SET);
    if (read(fp, &shdr, ehdr.e_shentsize) == 0)
    {
        close(fp);
        return gcvSTATUS_FALSE;
    }

    lseek(fp, (unsigned int)shdr.sh_offset, SEEK_SET);
    if (read(fp, secNameTab, shdr.sh_size) == 0)
    {
        close(fp);
        return gcvSTATUS_FALSE;
    }

    lseek(fp, ehdr.e_shoff, SEEK_SET);
    for (i = 0; i <  ehdr.e_shnum; i++)
    {
        if (read(fp, &shdr, (ssize_t)ehdr.e_shentsize) == 0)
        {
            close(fp);
            return gcvSTATUS_FALSE;
        }

        if (strcmp(secNameTab + shdr.sh_name, ".dynsym") == 0)
        {
            offset_dynsym = (unsigned int)shdr.sh_offset;
            size_dynsym = (int)(shdr.sh_size / shdr.sh_entsize);
        }
        else if(strcmp(secNameTab + shdr.sh_name, ".dynstr") == 0)
        {
            offset_dynstr = (unsigned int)shdr.sh_offset;
            size_dynstr = (unsigned int)shdr.sh_size;
        }
    }

    if (size_dynstr == 0)
    {
        close(fp);
        return gcvSTATUS_FALSE;
    }
    else
        symNameTab = (char *)malloc(size_dynstr);

    if ((lseek(fp, (off_t)offset_dynstr, SEEK_SET) != (off_t)offset_dynstr) || (offset_dynstr == 0))
    {
        free(symNameTab);
        symNameTab = gcvNULL;
        close(fp);
        return gcvSTATUS_FALSE;
    }
    if (read(fp, symNameTab, (size_t)(size_dynstr)) == 0)
    {
        free(symNameTab);
        symNameTab = gcvNULL;
        close(fp);
        return gcvSTATUS_FALSE;
    }

    if (size_dynsym > 8 * 1000)
    {
        free(symNameTab);
        symNameTab = gcvNULL;
        close(fp);
        return gcvSTATUS_SKIP;
    }

    lseek(fp, (off_t)offset_dynsym, SEEK_SET);
    for (i = 0; i < size_dynsym; i ++)
    {
        if (read(fp, &sym, (size_t)16) > 0)
        {
            for (j = 0; j < 10 && Symbols.symList[j]; j ++ )
            {
                gctCHAR *p, buffers[1024];
                p = buffers;
                buffers[0]='\0';
                strcat(buffers, Symbols.symList[j]);

                while (*p)
                {
                    *p = ~(*p);
                    p++;
                }

                if (strcmp(symNameTab + sym.st_name, buffers) == 0)
                    match++;
            }

            if (match >= j)
            {
                free(symNameTab);
                symNameTab = gcvNULL;
                close(fp);
                return gcvSTATUS_TRUE;
            }
        }
        else
        {
            status = gcvSTATUS_FALSE;
            break;
        }
    }

    if (symNameTab != gcvNULL)
    {
        free(symNameTab);
        symNameTab = gcvNULL;
    }

    close(fp);

    if (gcmIS_ERROR(status))
    {
        status = gcvSTATUS_FALSE;
    }
    return status;
}

gceSTATUS
gcoOS_DetectProgrameByEncryptedSymbols(
    IN gcoOS_SymbolsList Symbols
    )
{
    gceSTATUS status = gcvSTATUS_FALSE;

    /* Detect shared library path. */
    char path[256] = "/data/data/";
    char * s = NULL, * f = NULL;
    ssize_t size;

    s = path + strlen(path);

    /* Open cmdline file. */
    int fd = open("/proc/self/cmdline", O_RDONLY);

    if (fd < 0)
    {
        gcmPRINT("open /proc/self/cmdline failed");
        return gcvSTATUS_FALSE;
    }

    /* Connect cmdline string to path. */
    size = read(fd, s, 64);
    close(fd);

    if (size < 0)
    {
        gcmPRINT("read /proc/self/cmdline failed");
        return gcvSTATUS_FALSE;
    }

    f = strstr(s, ":");
    size = (f != NULL) ? (f - s) : size;

    s[size] = '\0';

    /* Connect /lib. */
    strcat(path, "/lib");

    /* Open shared library path. */
    DIR * dir = opendir(path);

    if (!dir)
    {
        return gcvSTATUS_FALSE;
    }

    //const char * found = NULL;
    struct dirent * dp;
    int count = 0;

    while ((dp = readdir(dir)) != NULL)
    {
        if (dp->d_type == DT_REG && ++count > 8)
        {
            status = gcvSTATUS_SKIP;
            goto OnError;
        }
    }
    rewinddir(dir);

    while ((dp = readdir(dir)) != NULL)
    {
        /* Only test regular file. */
        if (dp->d_type == DT_REG)
        {
            char buf[256];

            snprintf(buf, 256, "%s/%s", path, dp->d_name);

            gcmONERROR(ParseSymbol(buf, Symbols));

            if (status)
                break;
        }
    }

OnError:

    closedir(dir);

    if (gcmIS_ERROR(status))
    {
        status = gcvSTATUS_FALSE;
    }

    return status;
}
#endif

#if gcdANDROID_NATIVE_FENCE_SYNC
#include <sync/sync.h>

gceSTATUS
gcoOS_CreateSyncPoint(
    IN gcoOS Os,
    OUT gctSYNC_POINT * SyncPoint
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Os=0x%x", Os);

    /* Call kernel API to dup signal to native fence fd. */
    iface.command                       = gcvHAL_SYNC_POINT;
    iface.u.SyncPoint.command           = gcvSYNC_POINT_CREATE;

    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

    /* Return created sync point. */
    *SyncPoint = gcmUINT64_TO_PTR(iface.u.SyncPoint.syncPoint);

    /* Success. */
    gcmFOOTER_ARG("*SyncPoint=%u", (gctUINT) iface.u.SyncPoint.syncPoint);
    return gcvSTATUS_OK;

OnError:
    *SyncPoint = gcvNULL;

    /* Return the status. */
    gcmFOOTER();
    return status;
}

gceSTATUS
gcoOS_DestroySyncPoint(
    IN gcoOS Os,
    IN gctSYNC_POINT SyncPoint
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Os=0x%x SyncPoint=%u", Os, (gctUINT) SyncPoint);

    /* Call kernel API to dup signal to native fence fd. */
    iface.command                       = gcvHAL_SYNC_POINT;
    iface.u.SyncPoint.command           = gcvSYNC_POINT_DESTROY;
    iface.u.SyncPoint.syncPoint         = gcmPTR_TO_UINT64(SyncPoint);

    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

OnError:
    /* Return the status. */
    gcmFOOTER();
    return status;
}

gceSTATUS
gcoOS_CreateNativeFence(
    IN gcoOS Os,
    IN gctSYNC_POINT SyncPoint,
    OUT gctINT * FenceFD
    )
{
    gceSTATUS status;
    gcsHAL_INTERFACE iface;

    gcmHEADER_ARG("Os=0x%x SyncPoint=%d", Os, (gctUINT) SyncPoint);

    /* Call kernel API to dup signal to native fence fd. */
    iface.command                       = gcvHAL_CREATE_NATIVE_FENCE;
    iface.u.CreateNativeFence.syncPoint = gcmPTR_TO_UINT64(SyncPoint);
    iface.u.CreateNativeFence.fenceFD   = -1;

    gcmONERROR(gcoOS_DeviceControl(
        gcvNULL,
        IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        ));

    /* Return fence fd. */
    *FenceFD = iface.u.CreateNativeFence.fenceFD;

    /* Success. */
    gcmFOOTER_ARG("*FenceFD=%d", iface.u.CreateNativeFence.fenceFD);
    return gcvSTATUS_OK;

OnError:
    *FenceFD = -1;

    /* Return the status. */
    gcmFOOTER();
    return status;
}

gceSTATUS
gcoOS_WaitNativeFence(
    IN gcoOS Os,
    IN gctINT FenceFD,
    IN gctUINT32 Timeout
    )
{
    int err;
    int wait;
    gceSTATUS status;

    gcmASSERT(FenceFD != -1);
    gcmHEADER_ARG("Os=0x%x FenceFD=%d Timeout=%d", Os, FenceFD, Timeout);

    wait = (Timeout == gcvINFINITE) ? -1
         : (int) Timeout;

    /* err = ioctl(FenceFD, SYNC_IOC_WAIT, &wait); */
    err = sync_wait(FenceFD, wait);

    switch (err)
    {
    case 0:
        status = gcvSTATUS_OK;
        break;

    case -1:
        /*
         * libc ioctl:
         * On error, -1 is returned, and errno is set appropriately.
         * 'errno' is positive.
         */
        if (errno == ETIME)
        {
            status = gcvSTATUS_TIMEOUT;
            break;
        }

    default:
        status = gcvSTATUS_GENERIC_IO;
        break;
    }

    gcmFOOTER_ARG("status=%d", status);
    return status;
}
#endif

gceSTATUS
gcoOS_CPUPhysicalToGPUPhysical(
    IN gctUINT32 CPUPhysical,
    OUT gctUINT32_PTR GPUPhysical
    )
{
    *GPUPhysical = CPUPhysical;
    return gcvSTATUS_OK;
}

void
gcoOS_RecordAllocation(void)
{
#if gcdGC355_MEM_PRINT
    gcoOS os;
    if (gcPLS.os != gcvNULL)
    {
        os = gcPLS.os;

        os->oneSize = 0;
        os->oneRecording = 1;
    }
#endif
}

gctINT32
gcoOS_EndRecordAllocation(void)
{
    gctINT32   result = 0;
#if gcdGC355_MEM_PRINT
    gcoOS os;

    if (gcPLS.os != gcvNULL)
    {
        os = gcPLS.os;

        if (os->oneRecording == 1)
        {
            result = os->oneSize;

            os->oneSize = 0;
            os->oneRecording = 0;
        }
    }

#endif
    return result;
}

void
gcoOS_AddRecordAllocation(gctSIZE_T Size)
{
#if gcdGC355_MEM_PRINT
    gcoOS os;

    if (gcPLS.os != gcvNULL)
    {
        os = gcPLS.os;

        if (os->oneRecording == 1)
        {
            os->oneSize += (gctINT32)Size;
        }
    }
#endif
}
