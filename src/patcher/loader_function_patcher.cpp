#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include "common/common.h"
#include "common/fs_defs.h"
#include "common/loader_defs.h"
#include "game/rpx_rpl_table.h"
#include "dynamic_libs/fs_functions.h"
#include "dynamic_libs/os_functions.h"
#include "kernel/kernel_functions.h"
#include "loader_function_patcher.hpp"
#include "../utils/function_patcher.h"
#include "discdumper.h"

static u32 gLoaderPhysicalBufferAddr __attribute__((section(".data"))) = 0;

DECL(int, FSBindMount, void *pClient, void *pCmd, char *source, char *target, int error)
{
    if(strcmp(target, "/vol/app_priv") == 0 && IsDumpingDiscUsbMeta())
    {
        //! on game discs
        //! redirect mount target path to /vol/meta and dump it then unmount and re-mount on the target position
        char acPath[10];
        strcpy(acPath, "/vol/meta");

        int res = real_FSBindMount(pClient, pCmd, source, acPath, error);
        if(res == 0)
        {
            DumpMetaPath(pClient, pCmd, NULL);
            FSBindUnmount(pClient, pCmd, acPath, -1);
        }
    }
    return real_FSBindMount(pClient, pCmd, source, target, error);
}

DECL(int, OSDynLoad_Acquire, char* rpl, unsigned int *handle, int r5 __attribute__((unused))) {
    int result = real_OSDynLoad_Acquire(rpl, handle, 0);

    if(rpxRplTableGetCount() > 0)
    {
        DumpRpxRpl(NULL);
    }

    return result;
}

// This function is called every time after LiBounceOneChunk.
// It waits for the asynchronous call of LiLoadAsync for the IOSU to fill data to the RPX/RPL address
// and return the still remaining bytes to load.
// We override it and replace the loaded date from LiLoadAsync with our data and our remaining bytes to load.
DECL(int, LiWaitOneChunk, int * iRemainingBytes, const char *filename, int fileType)
{
    int result;
    int remaining_bytes = 0;
    unsigned int core_id;

    int *sgBufferNumber;
    int *sgBounceError;
    int *sgGotBytes;
    int *sgTotalBytes;
    int *sgIsLoadingBuffer;
    int *sgFinishedLoadingBuffer;
    unsigned int * __load_reply;

    // get the offset of per core global variable for dynload initialized (just a simple address + (core_id * 4))
    unsigned int gDynloadInitialized;

    // get the current core
    asm volatile("mfspr %0, 0x3EF" : "=r" (core_id));

    // Comment (Dimok):
    // time measurement at this position for logger  -> we don't need it right now except maybe for debugging
    //unsigned long long systemTime1 = Loader_GetSystemTime();

	if(OS_FIRMWARE == 550)
    {
        // pointer to global variables of the loader
        loader_globals_550_t *loader_globals = (loader_globals_550_t*)(0xEFE19E80);

        gDynloadInitialized = *(volatile unsigned int*)(0xEFE13DBC + (core_id << 2));
        __load_reply = (unsigned int *)0xEFE1D998;
        sgBufferNumber = &loader_globals->sgBufferNumber;
        sgBounceError = &loader_globals->sgBounceError;
        sgGotBytes = &loader_globals->sgGotBytes;
        sgTotalBytes = &loader_globals->sgTotalBytes;
        sgFinishedLoadingBuffer = &loader_globals->sgFinishedLoadingBuffer;
        // not available on 5.5.x
        sgIsLoadingBuffer = NULL;
    }
    else
    {
        // pointer to global variables of the loader
        loader_globals_t *loader_globals = (loader_globals_t*)(0xEFE19D00);

        gDynloadInitialized = *(volatile unsigned int*)(0xEFE13C3C + (core_id << 2));
        __load_reply = (unsigned int *)0xEFE1D818;
        sgBufferNumber = &loader_globals->sgBufferNumber;
        sgBounceError = &loader_globals->sgBounceError;
        sgGotBytes = &loader_globals->sgGotBytes;
        sgIsLoadingBuffer = &loader_globals->sgIsLoadingBuffer;
        // not available on < 5.5.x
        sgTotalBytes = NULL;
        sgFinishedLoadingBuffer = NULL;
    }

    // the data loading was started in LiBounceOneChunk() and here it waits for IOSU to finish copy the data
    if(gDynloadInitialized != 0) {
        result = LiWaitIopCompleteWithInterrupts(0x2160EC0, &remaining_bytes);

    }
    else {
        result = LiWaitIopComplete(0x2160EC0, &remaining_bytes);
    }


    // Comment (Dimok):
    // time measurement at this position for logger -> we don't need it right now except maybe for debugging
    //unsigned long long systemTime2 = Loader_GetSystemTime();

    //------------------------------------------------------------------------------------------------------------------
    // Start of our function intrusion:
    // After IOSU is done writing the data into the 0xF6000000/0xF6400000 address,
    // we overwrite it with our data before setting the global flag for IsLoadingBuffer to 0
    // Do this only if we are in the game that was launched by our method
    if((result == 0) && *(volatile unsigned int*)0xEFE00000 != 0x6d656e2e && *(volatile unsigned int*)0xEFE00000 != 0x66666C5F)
    {
        s_rpx_rpl *rpl_struct = rpxRplTableGet();
        int found = 0;
        int entryIndex = rpxRplTableGetCount();

        while(entryIndex > 0 && rpl_struct)
        {
            // if we load RPX then the filename can't be checked as it is the Mii Maker or Smash Bros RPX name
            // there skip the filename check for RPX
            int len = strlen(filename);
            int len2 = strlen(rpl_struct->name);
            if ((len != len2) && (len != (len2 - 4)))
            {
                rpl_struct = rpl_struct->next;
                continue;
            }

            if(strncasecmp(filename, rpl_struct->name, len) == 0)
            {
                found = 1;
                break;
            }

            rpl_struct = rpl_struct->next;
        }

        unsigned int load_address = (*sgBufferNumber == 1) ? gLoaderPhysicalBufferAddr : (gLoaderPhysicalBufferAddr + 0x00400000); // virtual 0xF6000000 and 0xF6400000

        int bytes_loaded = remaining_bytes;
        if(remaining_bytes == 0)
        {
            bytes_loaded = __load_reply[3];
        }
        else
        {
            bytes_loaded = remaining_bytes;
        }

        int system_rpl = 0;

        //! all RPL seem to have some kind of header of 0x80 bytes except the system ones
        if(!found && fileType == 1)
        {
            if(*(volatile unsigned int *)(load_address + 0x80) == 0x7F454C46)
            {
                load_address += 0x80;
                bytes_loaded -= 0x80;
                system_rpl = 0;
            }
            else
                system_rpl = 1;
        }

        if(!system_rpl)
        {
            if(!found)
            {
                s_mem_area* mem_area             = memoryGetAreaTable();
                unsigned int mem_area_addr_start = mem_area->address;
                unsigned int mem_area_addr_end   = mem_area->address + mem_area->size;
                unsigned int mem_area_offset     = 0;

                // on RPLs we need to find the free area we can store data to (at least RPX was already loaded by this point)
                if(entryIndex > 0)
                    mem_area = rpxRplTableGetNextFreeMemArea(&mem_area_addr_start, &mem_area_addr_end, &mem_area_offset);

                rpl_struct = rpxRplTableAddEntry(filename, mem_area_offset, 0, fileType == 0, entryIndex, mem_area);
            }

            rpl_struct->size += rpxRplCopyDataToMem(rpl_struct, rpl_struct->size, (unsigned char*)load_address, bytes_loaded);
        }
    }

    // end of our little intrusion into this function
    //------------------------------------------------------------------------------------------------------------------

    // set the result to the global bounce error variable
    *sgBounceError = result;

    // disable global flag that buffer is still loaded by IOSU
	if(OS_FIRMWARE == 550)
    {
        unsigned int zeroBitCount = 0;
        asm volatile("cntlzw %0, %0" : "=r" (zeroBitCount) : "r"(*sgFinishedLoadingBuffer));
        *sgFinishedLoadingBuffer = zeroBitCount >> 5;
    }
    else
    {
        *sgIsLoadingBuffer = 0;
    }

    // check result for errors
    if(result == 0) {
        // the remaining size is set globally and in stack variable only
        // if a pointer was passed to this function
        if(iRemainingBytes) {
            *sgGotBytes = remaining_bytes;
            *iRemainingBytes = remaining_bytes;
            // on 5.5.x a new variable for total loaded bytes was added
            if(sgTotalBytes != NULL) {
                *sgTotalBytes += remaining_bytes;
            }
        }
        // Comment (Dimok):
        // calculate time difference and print it on logging how long the wait for asynchronous data load took
        // something like (systemTime2 - systemTime1) * constant / bus speed, did not look deeper into it as we don't need that crap
    }
    else {
        // Comment (Dimok):
        // a lot of error handling here. depending on error code sometimes calls Loader_Panic() -> we don't make errors so we can skip that part ;-P
    }
    return result;
}
/* *****************************************************************************
 * Creates function pointer array
 * ****************************************************************************/


hooks_magic_t method_hooks_loader[] __attribute__((section(".data"))) = {
    MAKE_MAGIC(FSBindMount,                 LIB_CORE_INIT,       STATIC_FUNCTION),
    // LOADER function
    MAKE_MAGIC(LiWaitOneChunk,              LIB_LOADER,          STATIC_FUNCTION),
    // Dynamic RPL loading functions
    MAKE_MAGIC(OSDynLoad_Acquire,           LIB_CORE_INIT,       STATIC_FUNCTION),
};

u32 method_hooks_size_loader __attribute__((section(".data"))) = sizeof(method_hooks_loader) / sizeof(hooks_magic_t);

//! buffer to store our instructions needed for our replacements
volatile unsigned int method_calls_loader[sizeof(method_hooks_loader) / sizeof(hooks_magic_t) * FUNCTION_PATCHER_METHOD_STORE_SIZE] __attribute__((section(".data")));

