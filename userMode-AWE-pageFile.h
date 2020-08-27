
#ifndef USERMODE_H
#define USERMODE_H

#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <stdbool.h>
#include <windows.h>
#pragma comment(lib, "Advapi32.lib")

#define NUM_PAGES 512
#define PTE_INDEX_BITS 9        // log2(NUM_PAGES)
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12           // number of bits to multiply/divide by page size
#define PFN_BITS 40

#define PAGEFILE_PAGES 512
#define PAGEFILE_SIZE PAGEFILE_PAGES*PAGE_SIZE    // 
#define PAGEFILE_BITS 20 // actually 2^19 currently
#define INVALID_PAGEFILE_INDEX 0xfffff // 20 bits (MUST CORRESPOND TO PAGEFILE BITS)

#define ASSERT(x) if((x) == FALSE) DebugBreak()

typedef struct _hardwarePTE{
    ULONG64 validBit: 1;            // valid bit MUST be set for hPTE format
    ULONG64 writeBit: 1;            // read if 0, write if 1
    ULONG64 executeBit: 1;
    ULONG64 dirtyBit: 1;
    ULONG64 PFN: PFN_BITS;          //  page frame number (PHYSICAL)
} hardwarePTE, *PhardwarePTE;

typedef struct _transitionPTE{
    ULONG64 validBit: 1;            // valid bit MUST be 0 for tPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be 1 for dzPTE
    ULONG64 permissions: 3;                    // read if 0, write if 1
    ULONG64 readInProgressBit: 1;
    ULONG64 PFN: PFN_BITS;
} transitionPTE, *PtransitionPTE;

typedef struct _pageFilePTE{
    ULONG64 validBit: 1;            // valid bit MUST be 0 for dzPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be 0 for dzPTE
    ULONG64 pageFileBit: 1;       // pfbit MUST be 1 for pfPTE
    ULONG64 permissions: 3;
    ULONG64 pageFileIndex: PAGEFILE_BITS;
} pageFilePTE, *PpageFilePTE;

typedef struct _demandZeroPTE{
    ULONG64 validBit: 1;            // valid bit MUST be 0 for dzPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be 0 for dzPTE
    ULONG64 pageFileBit: 1;       // pf bit is 1 for dzPTE
    ULONG64 permissions: 3;
    ULONG64 pageFileIndex: PAGEFILE_BITS;   // PF index MUST be INVALID_PAGEFILE_INDEX( MAXULONG_PTR) for dzPTE
} demandZeroPTE, *PdemandZeroPTE;

typedef ULONG64 ulongPTE;

typedef struct _PTE{
    union {
        hardwarePTE hPTE;
        transitionPTE tPTE;
        pageFilePTE pfPTE;
        demandZeroPTE dzPTE;
        ULONG64 ulongPTE;
     } u1;
} PTE, *PPTE;

typedef struct _PFNdata {
    LIST_ENTRY links;
    ULONG64 statusBits: 5;
    ULONG64 pageFileOffset: PAGEFILE_BITS;
    ULONG64 PTEindex: PTE_INDEX_BITS;
    ULONG64 refCount: 16;          
} PFNdata, *PPFNdata;

typedef struct _listData {
    LIST_ENTRY head;
    ULONG64 count;
} listData, *PlistData;

typedef enum {
    ZERO,               // 0
    FREE,               // 1
    STANDBY,            // 2
    MODIFIED,           // 3
    ACTIVE,             // 4
} PFNstatus;

typedef enum {
    NO_ACCESS,          // 0
    READ_ONLY,          // 1
    READ_WRITE,         // 2
    READ_EXECUTE,       // 3
    READ_WRITE_EXECUTE, // 4
    // given 3 bits, have capacity for up to 3 more
} PTEpermissions;

typedef enum {
    SUCCESS,            // 0
    ACCESS_VIOLATION,   // 1
    NO_FREE_PAGES,      // 2
} faultStatus;

typedef enum {
    READ,               // 0
    WRITE,              // 1
} readWrite;

/*
 *  GLOBAL VARIABLES
 */
extern void* leafVABlock;                  // starting address of memory block
extern void* leafVABlockEnd;               // ending address of memory block

extern PPFNdata PFNarray;                  // starting address of PFN metadata array
extern PPTE PTEarray;                      // starting address of page table

extern void* zeroVA;                       // specific VA used for zeroing PFNs (via AWE mapping)

extern ULONG_PTR totalCommittedPages;      // count of committed pages (initialized to zero)
extern ULONG_PTR totalMemoryPageLimit;     // limit of committed pages (memory block + pagefile space)


extern void* pageFileVABlock;              // starting address of pagefile "disk" (memory)
extern void* modifiedWriteVA;              // specific VA used for writing out page contents to pagefile
extern void* pageFileFormatVA;             // specific VA used for copying in page contents from pagefile
extern ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];

// Execute-Write-Read (bit ordering)
extern ULONG_PTR permissionMasks[];
#define readMask (1 << 0)                   // 1
#define writeMask (1 << 1)                  // 2
#define executeMask (1 << 2)                // 4


// listHeads array
extern listData listHeads[ACTIVE];
#define zeroListHead listHeads[ZERO]
#define freeListHead listHeads[FREE]
#define standbyListHead listHeads[STANDBY]
#define modifiedListHead listHeads[MODIFIED]


/*
 * getPTE: function to find corresponding PTE from a given VA
 * 
 * Returns PPTE
 *  - currPTE (corresponding PTE) on success
 *  - NULL on failure (access violation, VA outside of range)
 */
PPTE
getPTE(void* virtualAddress);


/*
 * trimPage(void* VA): function to trim the entire containing page corresponding to VA param
 *  - Converts from VALID format PTE to TRANSITION format PTE
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure (i.e. PTE valid bit not set)
 */
BOOLEAN
trimPage(void* virtualAddress);


/*****************************************************************
   LoggedSetLockPagesPrivilege: a function to obtain or
   release the privilege of locking physical pages.

   Inputs:

       HANDLE hProcess: Handle for the process for which the
       privilege is needed

       BOOL bEnable: Enable (TRUE) or disable?

   Return value: TRUE indicates success, FALSE failure.

*****************************************************************/
BOOL
LoggedSetLockPagesPrivilege ( HANDLE hProcess,
                              BOOL bEnable);


/*
 * getPrivilege: wrapper function for getting page mapping privilege
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
getPrivilege ();


/*
 * initVABlock: initializes the block of memory to be managed
 *  - 
 * 
 * Returns PVOID:
 *  - starting VA
 * 
 */
PVOID
initVABlock(int numPages, int pageSize);


PPFNdata
initPFNarray(PULONG_PTR aPFNs, int numPages, int pageSize);


PPTE
initPTEarray(int numPages, int pageSize);


PVOID
initPageFile(int diskSize);


BOOLEAN
zeroPage(ULONG_PTR PFN);


BOOLEAN
zeroPageWriter();

/*
 * TODO (future): bump refcount and set read in progress bit
 * 
 * modifiedPageWriter: function to pull a page off modified list and write to pagefile
 * - checks if rhere are any pages on modified list
 * - if there are, call writePage, update status bits and enqueue PFN to standby list
 * 
 * returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
modifiedPageWriter();


/********* LOCAL functions *******/
VOID
initLinkHead(PLIST_ENTRY headLink);


VOID
initListHead(PlistData headData);


#endif //USERMODE_H