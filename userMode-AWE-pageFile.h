
#ifndef USERMODE_H
#define USERMODE_H

#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <stdbool.h>
#include <windows.h>
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "MinCore.lib")


/******** Key functionality macros ********/
#define NUM_THREADS 5
#define MULTIPLE_MAPPINGS                           // enables multiple mappings


/*********** Testing macros ************/
#define CHECK_PFNS
// #define PAGEFILE_OFF                            // Fills pagefile slots and bitarray, allowing
                                                // program to run sans pagefile
// #define PTE_CHANGE_LOG
#define PAGEFILE_PFN_CHECK                      // When toggled on, enables a debugging replacement
                                                // for the pagefile that includes additional information
#define VERIFY_ADDRESS_SIGNATURES
#define AV_TEMP_TESTING                                // temporary workaround for PF due to app verifier
#define TESTING_ZERO                                // toggles zero page thread
#define TESTING_MODIFIED                            // toggles modified page writer thread
#define TESTING_VERIFY_ADDRESSES                    // tests addresses that are written on decommit
#define CONTINUOUS_FAULT_TEST
// #define TRADE_PAGES


/*********** number of physical memory pages to allocate (+ PF pages for total memory) **********/
// #define NUM_PAGES 64
#define NUM_PAGES 512
// #define NUM_PAGES 2048
// #define NUM_PAGES 64000

#define MIN_AVAILABLE_PAGES 100

#define VM_MULTIPLIER 2                          // VM space is this many times larger than num physical pages successfully allocated


/********** PTE bit macros *************/
#define PERMISSIONS_BITS 3                          // bits in non-valid PTE formats reserved for permissions
#define PTE_INDEX_BITS 12                            // log2(NUM_PAGES)
#define PFN_BITS 40                                 // number of bits to store PFN index


/************ page size macros ***********/
#define PAGE_SIZE 4096                              // page size
#define PAGE_SHIFT 12                               // number of bits to multiply/divide by page size (log2 page size)


/*********** pagefile macros ***************************/
// #define PAGEFILE_PAGES 8                           // capacity of pagefile pages (+ NUM_PAGES for total memory)

#define PAGEFILE_PAGES 512                          // capacity of pagefile pages (+ NUM_PAGES for total memory)
#define PAGEFILE_SIZE PAGEFILE_PAGES*PAGE_SIZE      // total pagefile size 
#define PAGEFILE_BITS 20                            // actually 2^19 currently
#define INVALID_BITARRAY_INDEX 0xfffff              // 20 bits (MUST CORRESPOND TO PAGEFILE BITS)


#define PAGES_PER_LOCK 64


/**************** assert macro *****************/
#define ASSERT(x) if((x) == FALSE) DebugBreak()


/***************** print macros ******************/
#define PRINT(fmt, ...) if (debugMode == TRUE) { printf(fmt, __VA_ARGS__); }
#define PRINT_ERROR(fmt, ...) fprintf(stderr, fmt, __VA_ARGS__); ASSERT(FALSE); 
#define PRINT_ALWAYS(fmt, ...) printf(fmt, __VA_ARGS__)


/***************** STRUCT definitions **************/
typedef struct _hardwarePTE{
    ULONG64 validBit: 1;            // valid bit MUST be set for hPTE format
    ULONG64 writeBit: 1;            // read if 0, write if 1
    ULONG64 executeBit: 1;
    ULONG64 dirtyBit: 1;
    ULONG64 agingBit: 1;
    ULONG64 padding: 3;             // To maintain 4-bit alignment (for debugging ease at space efficiency cost)
    ULONG64 PFN: PFN_BITS;          //  page frame number (PHYSICAL)
} hardwarePTE, *PhardwarePTE;

typedef struct _transitionPTE{
    ULONG64 validBit: 1;            // valid bit MUST be 0 for tPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be set for tPTE
    ULONG64 permissions: PERMISSIONS_BITS;
    ULONG64 padding: 3;             // To maintain 4-bit alignment (for debugging ease at space efficiency cost)
    ULONG64 PFN: PFN_BITS;
} transitionPTE, *PtransitionPTE;

typedef struct _pageFilePTE{
    ULONG64 validBit: 1;            // valid bit MUST be 0 for pfPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be 0 for pfPTE
    ULONG64 decommitBit: 1;
    ULONG64 permissions: PERMISSIONS_BITS;
    ULONG64 padding: 2;             // To maintain 4-bit alignment (for debugging ease at space efficiency cost)
    ULONG64 pageFileIndex: PAGEFILE_BITS;
} pageFilePTE, *PpageFilePTE;

typedef struct _demandZeroPTE{
    ULONG64 validBit: 1;            // valid bit MUST be 0 for dzPTE
    ULONG64 transitionBit: 1;       // transition bit MUST be 0 for dzPTE
    ULONG64 decommitBit: 1;
    ULONG64 permissions: PERMISSIONS_BITS;
    ULONG64 padding: 2;                     // To maintain 4-bit alignment (for debugging ease at space efficiency cost)
    ULONG64 pageFileIndex: PAGEFILE_BITS;   // PF index MUST be INVALID_BITARRAY_INDEX( MAXULONG_PTR) for dzPTE
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

typedef struct _eventNode {
    LIST_ENTRY links;
    HANDLE event;
} eventNode, *PeventNode;

typedef struct _PFNdata {
    LIST_ENTRY links;
    ULONG64 statusBits: 5;
    ULONG64 pageFileOffset: PAGEFILE_BITS;
    ULONG64 PTEindex: PTE_INDEX_BITS;
    ULONG64 writeInProgressBit: 1;          // Overloaded bit - also used to signify to page trader that page could be being zeroed
    ULONG64 readInProgressBit: 1;
    ULONG64 refCount: 16;                
    ULONG64 remodifiedBit: 1;      
    ULONG64 padding: 20;  
    volatile LONG lockBits;                // 31 free bits if necessary
    PeventNode readInProgEventNode;
} PFNdata, *PPFNdata;

typedef struct _listData {
    LIST_ENTRY head;
    ULONG64 count;
    CRITICAL_SECTION lock;
    HANDLE newPagesEvent;
} listData, *PlistData;

typedef struct _VANode {
    LIST_ENTRY links;
    PVOID VA;
} VANode, *PVANode;






/*********** ENUM definitions ************/
typedef enum {          // DO NOT EDIT ORDER without checking enqueue/dequeue
    ZERO,               // 0
    FREE,               // 1
    STANDBY,            // 2
    MODIFIED,           // 3
    QUARANTINE,         // 4
    NONE,               // 5 (transitioning between states)
    AWAITING_FREE,      // 6
    AWAITING_QUARANTINE,// 7
    ACTIVE,             // 8 (ACTIVE must be last since it is used to dimensionalize arrays)
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
    NO_AVAILABLE_PAGES, // 2
    PAGE_STATE_CHANGE,  // 3
} faultStatus;

typedef enum {
    READ,               // 0
    WRITE,              // 1
} readWrite;



/**************  GLOBAL VARIABLES  *************/
extern BOOLEAN debugMode;

extern void* leafVABlock;                  // starting address of memory block
extern void* leafVABlockEnd;               // ending address of memory block

extern PPFNdata PFNarray;                  // starting address of PFN array
extern PPTE PTEarray;                      // starting address of page table

extern ULONG64 totalCommittedPages;           // count of committed pages (initialized to zero)
extern ULONG_PTR totalMemoryPageLimit;     // limit of committed pages (memory block + pagefile space)


extern void* pageFileVABlock;              // starting address of pagefile "disk" (memory)

#ifdef PAGEFILE_PFN_CHECK
typedef struct _pageFileDebug {
    PPTE currPTE;
    PPFNdata currPFN;
    PTE PTEdata;
    PFNdata PFNdata;
} pageFileDebug, *PPageFileDebug;
extern PPageFileDebug pageFileDebugArray;
#else
extern ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];
#endif

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
#define quarantineListHead listHeads[QUARANTINE]

extern listData zeroVAListHead;             // list of zeroVAs used for zeroing PFNs (via AWE mapping)
extern listData writeVAListHead;            // list of writeVAs used for writing to page file
extern listData readPFVAListHead;
extern listData pageTradeVAListHead;

extern listData VADListHead;               // list of VADs

extern listData readInProgEventListHead;

extern CRITICAL_SECTION PTELock;            // coarse-grained lock on page table/directory
extern PCRITICAL_SECTION PTELockArray;      // finer-grained lock array for page table/directory (replaces above)


extern HANDLE wakeTrimHandle;

extern HANDLE wakeModifiedWriterHandle;

extern PULONG_PTR VADBitArray;

extern ULONG_PTR virtualMemPages;

extern CRITICAL_SECTION pageFileLock;




// toggle multithreading on and off
#define MULTITHREADING



// /*****************************************************************

//     * ADAPTED FROM https://docs.microsoft.com/en-us/windows/win32/memory/awe-example *

//    LoggedSetLockPagesPrivilege: a function to obtain or
//    release the privilege of locking physical pages.

//    Inputs:

//        HANDLE hProcess: Handle for the process for which the
//        privilege is needed

//        BOOL bEnable: Enable (TRUE) or disable?

//    Return value: TRUE indicates success, FALSE failure.


// *****************************************************************/
// BOOL
// LoggedSetLockPagesPrivilege ( HANDLE hProcess,
//                               BOOL bEnable);


// /*
//  * getPrivilege: wrapper function for getting page mapping privilege
//  * 
//  * Returns BOOLEAN:
//  *  - TRUE on success
//  *  - FALSE on failure
//  */
// BOOL
// getPrivilege ();


BOOLEAN
zeroPage(ULONG_PTR PFN);


BOOLEAN
zeroPageWriter();


DWORD WINAPI
zeroPageThread();

VOID
releaseAwaitingFreePFN(PPFNdata PFNtoFree);


/* 
 * modifiedPageWriter: function to pull a page off modified list and write to pagefile
 * - checks if there are any pages on modified list
 * - if there are, call writePageToFileSystem, update status bits and enqueue PFN to standby list
 * - when shared pages are added, bump refcount
 * 
 * returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
modifiedPageWriter();


DWORD WINAPI
modifiedPageThread();


BOOLEAN
faultAndAccessTest();


DWORD WINAPI
faultAndAccessTestThread(HANDLE terminationHandle);



ULONG_PTR
trimValidPTEs();


DWORD WINAPI
trimValidPTEThread(HANDLE terminationHandle);



/********* LOCAL functions *******/
VOID
initLinkHead(PLIST_ENTRY headLink);


VOID
initListHead(PlistData headData);


#endif //USERMODE_H