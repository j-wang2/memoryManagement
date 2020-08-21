
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
#define PFN_BITS 40

#define PAGEFILE_PAGES 512
#define PAGEFILE_SIZE PAGEFILE_PAGES*PAGE_SIZE    // 
#define PAGEFILE_BITS 20 // actually 2^19 currently

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

typedef struct _demandZeroPTE{          // TODO: not pfPTE if pagefile index is maxulong
    ULONG64 validBit: 1;            // valid bit MUST be 0 for dzPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be 0 for dzPTE
    ULONG64 demandZeroBit: 1;       // dz bit MUST be 1 for dzPTE
    ULONG64 permissions: 3;
} demandZeroPTE, *PdemandZeroPTE;

typedef struct _pageFilePTE{
    ULONG64 validBit: 1;            // valid bit MUST be 0 for dzPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be 0 for dzPTE
    ULONG64 demandZeroBit: 1;       // dz bit MUST be 0 for pfPTE
    ULONG64 permissions: 3;
    ULONG64 pageFileIndex: PAGEFILE_BITS;
} pageFilePTE, *PpageFilePTE;

typedef ULONG64 ulongPTE;

typedef struct _PTE{
    union {
        hardwarePTE hPTE;
        transitionPTE tPTE;
        pageFilePTE pfPTE;
        demandZeroPTE dzPTE;
        ULONG_PTR ulongPTE;
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
 * getPTEpermissions: function to convert input PTE's permissions (as separate bits) into a return PTEpermissions (3 consecutive bits)
 * 
 * 
 * 
 */
PTEpermissions
getPTEpermissions(PTE curr);

/*
 * checkPTEpermissions: function to check current PTE permissions with requested permissions
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
checkPTEpermissions(PTEpermissions currP, PTEpermissions checkP);



/*
 * pageFault: function to simulate and handle a pagefault
 * function
 *  - gets page given a VA
 *  - if PTE exists, return SUCCESS (2)?
 *  - otherweise, dequeue from freed list and return pointer to PFN entry 
 * 
 * Returns faultStatus:
 *  - SUCCESS on successful 
 *  - ACCESS_VIOLATION on access violation ( out of bounds/ invalid permissions)
 * 
 */
faultStatus
pageFault(void* virtualAddress, PTEpermissions RWEpermissions);


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
 * TODO - FIX!
 *   - need to add zero list
 * 
 * getPage: function to get a page off zero/free/standby list
 * 
 * Returns PPFNdata
 *  - returnPFN (pfn metadata for the freed page) on success
 *  - NULL on failure (all lists empty)
 */
PPFNdata
getPage();

/*
 * accessVA: function to access a VA, given either read or write permissions
 *  - pagefaults if not already valid
 * 
 * Returns faultStatus value
 *  - SUCCESS on success
 *  - ACCESS_VIOLATION on failure
 */
faultStatus
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions);

/*
 * isVAaccessible: function to check access to a VA - DOES NOT pagefault on failure
 * 
 * Returns faultStatus value
 *  - SUCCESS on success
 *  - ACCESS_VIOLATION on failure (1)
 */
faultStatus 
isVAaccessible (PVOID virtualAddress, PTEpermissions RWEpermissions);

/*
 * commitVA: function to commit a VA
 *  - changes PTE to demand zero
 *  - checks whether there is still memory left
 * 
 * Returns boolean
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
commitVA (PVOID startVA, PTEpermissions RWEpermissions, ULONG_PTR commitSize);

/*TODO - need to adapt for PF format
 * decommitVA: function to decommit a given virtual address
 * 
 * Returns BOOLEAN
 *  - TRUE on success
 *  - FALSE on failure (VA out of bounds, PTE already zero)
 */
BOOLEAN
decommitVA (PVOID startVA, ULONG_PTR commitSize);

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
modifiedPageWriter();


#endif //USERMODE_H