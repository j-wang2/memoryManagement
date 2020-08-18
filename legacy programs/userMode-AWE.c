/*
 * Usermode virtual memory program intended as a rudimentary simulation
 * of Windows OS kernelmode memory management
 * 
 * Jason Wang, August 2020
 */

#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <stdbool.h>
#include <windows.h>
#pragma comment(lib, "Advapi32.lib")

#define NUM_PAGES 512
#define PAGE_SIZE 4096

typedef struct _hardwarePTE{
    ULONG64 validBit: 1;
    ULONG64 transitionBit: 1;
    ULONG64 dirtyBit: 1;
    ULONG64 writeBit: 1;    // read if 0, write if 1
    ULONG64 PFN: 40;    //  page frame number (PHYSICAL)
} hardwarePTE, *PhardwarePTE;

typedef ULONG64 ulongPTE;

typedef struct _PTE{
    union {
        hardwarePTE hPTE;
        ULONG_PTR ulongPTE;
     } u1;
} PTE, *PPTE;

typedef struct _PFNdata {
    LIST_ENTRY links;
    ULONG64 statusBits: 5;
} PFNdata, *PPFNdata;

typedef struct _listData {
    LIST_ENTRY head;
    ULONG64 count;
} listData, *PlistData;

typedef enum {
    FREE,               // 0
    STANDBY,            // 1
    MODIFIED,           // 2
    ACTIVE,             // 3
} PFNstatus;

typedef enum {
    SUCCESS,            // 0
    ACCESS_VIOLATION,   // 1
    NO_FREE_PAGES,      // 2
} faultStatus;

typedef enum {
    READ,               // 0
    WRITE,              // 1
} readWrite;

// Global variables
void* leafVABlock;
void* leafVABlockEnd;
PPFNdata PFNarray;
PPTE PTEarray;
void* zeroVA;

// listheads
listData listHeads[ACTIVE];
#define freeListHead listHeads[FREE]
#define standbyListHead listHeads[STANDBY]
#define modifiedListHead listHeads[MODIFIED]
// LIST_ENTRY freeListHead;
// LIST_ENTRY standbyListHead;

faultStatus
pageFault(void* virtualAddress, readWrite RWpermission);

PPTE
getPTE(void* virtualAddress);

PPFNdata
getPage();

faultStatus
accessVA (PVOID virtualAddress, readWrite req);

void
enqueue(PLIST_ENTRY listHead, PLIST_ENTRY newItem);

PLIST_ENTRY
dequeue(PLIST_ENTRY listHead);

void
dequeueSpecific(PLIST_ENTRY removeItem);

BOOL
LoggedSetLockPagesPrivilege ( HANDLE hProcess,
                              BOOL bEnable);

BOOLEAN
getPrivilege ();

void* 
initVABlock(int numPages, int pageSize);

PPFNdata
initPFNarray(PULONG_PTR aPFNs, int numPages, int pageSize);

PPTE
initPTEarray(int numPages, int pageSize);

BOOLEAN
zeroPage(ULONG_PTR PFN);

/*
 * function
 *  - gets page given a VA
 *  - if PTE exists, return SUCCESS (2)?
 *  - otherweise, dequeue from freed list and return pointer to PFN entry 
 */
faultStatus
pageFault(void* virtualAddress, readWrite RWpermission)
{
    printf("pageFault\n");

    // get the PTE from the VA
    PPTE currPTE;
    currPTE = getPTE(virtualAddress);

    // invalid VA (not in range)
    if (currPTE == NULL) {
        return ACCESS_VIOLATION;
    }
    
    // make a shallow copy/"snapshot" of the PTE to edit and check
    PTE tempPTE;
    tempPTE = *currPTE;

    // check to see if PTE is zero - if so, set writeBit to one initially
    if (tempPTE.u1.ulongPTE == 0) {
        tempPTE.u1.hPTE.writeBit = 1;
    }

    // check permissions and update dirty bit if write access
    if (tempPTE.u1.hPTE.writeBit < RWpermission) {
        fprintf(stderr, "Invalid permissions\n");
        return ACCESS_VIOLATION;
    } else if (RWpermission == WRITE) {
        tempPTE.u1.hPTE.dirtyBit = 1;
    }

    // if PTE is in array and valid, just return success and print that pfn is already valid
    if (tempPTE.u1.hPTE.validBit == 1) {
        printf("PFN is already valid\n");
        return SUCCESS;
    } else {
        
        // declare pageNum (PFN) ULONG
        ULONG_PTR pageNum;

        // if transition PTE   
        if (tempPTE.u1.hPTE.transitionBit == 1) {

            pageNum = tempPTE.u1.hPTE.PFN;

            PPFNdata transitionPFN;
            transitionPFN = PFNarray + pageNum;

            PLIST_ENTRY tLink;
            tLink = &transitionPFN->links;

            // dequeue from either standby or modified list
            dequeueSpecific(tLink);

            // update count for lists (works for both standby and modified)
            PFNstatus currStatus = transitionPFN->statusBits;
            listHeads[currStatus].count--;

            // & set status to active
            transitionPFN->statusBits = ACTIVE;

            // set PTE valid bit to 1
            tempPTE.u1.hPTE.validBit = 1;
            tempPTE.u1.hPTE.transitionBit = 0;

            // assign VA to point at physical page, mirroring our local PTE change
            MapUserPhysicalPages(virtualAddress, 1, &pageNum);

            // compiler writes out as indivisible store
            * (volatile PTE *) currPTE = tempPTE;
            return SUCCESS;

        // otherwise (if active and transition bits are both 0), pull new PFN
        // indicates that this is zero or demand zero PTE
        } else{
            printf("attempting to allocate new page from freed list \n");
            
            // dequeue a page of memory from freed list, setting status bits to active
            PPFNdata freedPFN;
            freedPFN = getPage();
            if (freedPFN == NULL) {
                fprintf(stderr, "failed to successfully dequeue PFN from freed list\n");
                return NO_FREE_PAGES;
            }
            // decrement freeListHead count
            freeListHead.count--;
            freedPFN->statusBits = ACTIVE;
            
            // assign currPFN as calculated, and change PTE to validBit;
            pageNum = freedPFN - PFNarray;
            tempPTE.u1.hPTE.PFN = pageNum;
            tempPTE.u1.hPTE.validBit = 1;

            MapUserPhysicalPages(virtualAddress, 1, &pageNum);

            // compiler writes out as indivisible store
            * (volatile PTE *) currPTE = tempPTE;
            return SUCCESS;         // return value of 2; 
        }
    }
}

PPTE
getPTE(void* virtualAddress)
{
    // verify VA param is within the range of the VA block
    if (virtualAddress < leafVABlock || virtualAddress >= leafVABlockEnd) {
        fprintf(stderr, "access violation \n");
        return NULL;
    }

    // get VA's offset into the leafVABlock
    ULONG_PTR offset;
    offset = (ULONG_PTR) virtualAddress - (ULONG_PTR) leafVABlock;

    // convert offset to pagetable index
    ULONG_PTR pageTableIndex;
    pageTableIndex = offset/PAGE_SIZE;     // can i just shift by 12 bytes here instead?

    // get the corresponding page table entry from the PTE array
    PPTE currPTE;
    currPTE = PTEarray + pageTableIndex;

    return currPTE;
}

// PPFNdata - FIX
PPFNdata
getPage()
{
    PPFNdata returnPFN;
    if (freeListHead.count != 0) {
        returnPFN = (PPFNdata) dequeue(&freeListHead.head);
        freeListHead.count--;
    } else if (standbyListHead.count != 0) {
        returnPFN = (PPFNdata) dequeue(&standbyListHead.head);
        standbyListHead.count--;
    } else if (modifiedListHead.count != 0) {
        returnPFN = (PPFNdata) dequeue(&modifiedListHead.head);
        modifiedListHead.count--;
    } else {
        return NULL;
    }
    ULONG_PTR PFN;
    PFN = returnPFN - PFNarray;
    zeroPage(PFN);
    return returnPFN;
}

/*
 * function to access a VA, given either read or write permissions
 */
faultStatus 
accessVA (PVOID virtualAddress, readWrite req) 
{
    faultStatus PFstatus;
    PFstatus = SUCCESS;
    while (PFstatus == SUCCESS) {
        _try {
            if (req == READ) {
                *(volatile CHAR *)virtualAddress; // read

            } else if (req == WRITE) {
                *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress; // write

            } else {
                fprintf(stderr, "invalid permissions\n");
            }
            break;
        } _except (EXCEPTION_EXECUTE_HANDLER) {
            PFstatus = pageFault(virtualAddress, req);
            // pageFault(virtualAddress, WRITE);
        }
    }

    return PFstatus;
}
faultStatus 
isVAaccessible (PVOID virtualAddress, readWrite req) 
{
    _try {
        if (req == READ) {
            *(volatile CHAR *)virtualAddress; // read

        } else if (req == WRITE) {
            *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress; // write

        } else {
            fprintf(stderr, "invalid permissions\n");
        }
        return SUCCESS;
    } _except (EXCEPTION_EXECUTE_HANDLER) {
        return ACCESS_VIOLATION;
    }
}
void
enqueue(PLIST_ENTRY listHead, PLIST_ENTRY newItem) 
{
    PLIST_ENTRY prevFirst;
    prevFirst = listHead->Flink;
    
    listHead->Flink = newItem;
    newItem->Blink = listHead;
    newItem->Flink = prevFirst;
    prevFirst->Blink = newItem;

    return; // should I haave a return value?
}


PLIST_ENTRY
dequeue(PLIST_ENTRY listHead) 
{

    // verify list has items chained to the head
    if (listHead->Flink == listHead) {
        fprintf(stderr, "empty list\n");
        return NULL;
    }

    PLIST_ENTRY returnItem;
    PLIST_ENTRY newFirst;
    returnItem = listHead->Flink;
    newFirst = returnItem->Flink;

    // set listhead's flink to the return item's flink
    listHead->Flink = newFirst;
    newFirst->Blink = listHead;

    // set returnItem's flink/blink to null before returning it
    returnItem->Flink = NULL;
    returnItem->Blink = NULL;

    return returnItem;
}

void
dequeueSpecific(PLIST_ENTRY removeItem)
{
    PLIST_ENTRY prev;
    prev = removeItem->Blink;

    PLIST_ENTRY next;
    next = removeItem->Flink;

    prev->Flink = next;
    next->Blink = prev;

    removeItem->Flink = NULL;
    removeItem->Blink = NULL;
    return;
}

/*
 * trimPage(void* VA)
 * Trims entire page corresponding to VA
 * FIX - change return value to note success
 */
void
trimPage(void* virtualAddress)
{
    printf("trimming page with VA %d\n", (ULONG) virtualAddress);
    PPTE PTEaddress;
    PTE PTEtoTrim;
    PTEaddress = getPTE(virtualAddress);     //JW fix - error check
    PTEtoTrim = *PTEaddress;

    // check if PTE's valid bit is set - if not, can't be trimmed and return failure
    if (PTEtoTrim.u1.hPTE.validBit == 0) {
        fprintf(stderr, "PTE is not valid\n");
        return;
    }
    // invalidate PTE
    PTEtoTrim.u1.hPTE.validBit = 0;
    MapUserPhysicalPages(virtualAddress, 1, NULL);

    // if (!MapUserPhysicalPages(virtualAddress, 1, NULL)) {
    //     fprintf(stderr, "failed to unmap page at VA %u\n", (ULONG_PTR)virtualAddress);
    // }  // must be pre-dirtybit

    // get pageNum
    ULONG_PTR pageNum;
    pageNum = PTEtoTrim.u1.hPTE.PFN;

    // get PFN
    PPFNdata PFNtoTrim;
    PFNtoTrim = PFNarray + pageNum;

    // get link
    PLIST_ENTRY linkToTrim;
    linkToTrim = &PFNtoTrim->links;

    // check dirtyBit to see if page has been modified
    if (PTEtoTrim.u1.hPTE.dirtyBit == 0) {

        // add given VA's page to standby list
        enqueue(&standbyListHead.head, linkToTrim);
        PFNtoTrim->statusBits = STANDBY;   
        // set standbyBit to 1
        PTEtoTrim.u1.hPTE.transitionBit = 1;  

    } else if (PTEtoTrim.u1.hPTE.dirtyBit == 1) {
        // add given VA's page to modified list;
        enqueue(&modifiedListHead.head, linkToTrim);
        PFNtoTrim->statusBits = MODIFIED;
        PTEtoTrim.u1.hPTE.transitionBit = 1;

    }
    *PTEaddress = PTEtoTrim;

}
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
                            BOOL bEnable)
{
    struct {
        DWORD Count;
        LUID_AND_ATTRIBUTES Privilege [1];
    } Info;

    HANDLE Token;
    BOOL Result;

    // Open the token.

    Result = OpenProcessToken ( hProcess,
                                TOKEN_ADJUST_PRIVILEGES,
                                & Token);

    if( Result != TRUE ) 
    {
        _tprintf( _T("Cannot open process token.\n") );
        return FALSE;
    }

    // Enable or disable?

    Info.Count = 1;
    if( bEnable ) 
    {
        Info.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
    } 
    else 
    {
        Info.Privilege[0].Attributes = 0;
    }

    // Get the LUID.

    Result = LookupPrivilegeValue ( NULL,
                                    SE_LOCK_MEMORY_NAME,
                                    &(Info.Privilege[0].Luid));

    if( Result != TRUE ) 
    {
        _tprintf( _T("Cannot get privilege for %s.\n"), SE_LOCK_MEMORY_NAME );
        return FALSE;
    }

    // Adjust the privilege.

    Result = AdjustTokenPrivileges ( Token, FALSE,
                                    (PTOKEN_PRIVILEGES) &Info,
                                    0, NULL, NULL);

    // Check the result.

    if( Result != TRUE ) 
    {
        _tprintf (_T("Cannot adjust token privileges (%u)\n"), GetLastError() );
        return FALSE;
    } 
    else 
    {
        if( GetLastError() != ERROR_SUCCESS ) 
        {
        _tprintf (_T("Cannot enable the SE_LOCK_MEMORY_NAME privilege; "));
        _tprintf (_T("please check the local policy.\n"));
        return FALSE;
        }
    }

    CloseHandle( Token );

    return TRUE;
}

BOOLEAN
getPrivilege ()
{
    return LoggedSetLockPagesPrivilege( GetCurrentProcess(), TRUE );
}

void* 
initVABlock(int numPages, int pageSize)
{
    // initialize block of "memory" (unsure of type)
    void* leafVABlock;

    // leafVABlock = VirtualAlloc(NULL, NUM_PAGES*PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    // creates a VAD node that we can define
    leafVABlock = VirtualAlloc(NULL, NUM_PAGES*PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    if (leafVABlock == NULL) {
        exit(-1);
    }

    leafVABlockEnd = (PVOID) ( (ULONG_PTR)leafVABlock + NUM_PAGES*PAGE_SIZE );
    return leafVABlock;
}

PPFNdata
initPFNarray(PULONG_PTR aPFNs, int numPages, int pageSize)
{
    ULONG_PTR maxPFN = 0;
    for (int i = 0; i < numPages; i++) {
        if (maxPFN < aPFNs[i]) {
            maxPFN = aPFNs[i];
        }
    }

    // initialize PFN metadata array
    PPFNdata PFNarray;
    PFNarray = VirtualAlloc(NULL, (maxPFN+1)*(sizeof(PFNdata)), MEM_COMMIT, PAGE_READWRITE);
    if (PFNarray == NULL) {
        exit(-1);
    }

    // add all pages to freed list

    for (int i = 0; i < numPages; i++) {
        PLIST_ENTRY currLink;
        PPFNdata currPFN = PFNarray + aPFNs[i];
        currLink = &currPFN->links;
        enqueue(&freeListHead.head, currLink);
        freeListHead.count++;
        currPFN->statusBits = FREE;
    }

    return PFNarray;
}

PPTE
initPTEarray(int numPages, int pageSize)
{    
    // initialize PTE array
    PPTE PTEarray;
    PTEarray = VirtualAlloc(NULL, NUM_PAGES*(sizeof(PTE)), MEM_COMMIT, PAGE_READWRITE);
    if (PTEarray == NULL) {
        exit(-1);
    }
    return PTEarray;
}

BOOLEAN
zeroPage(ULONG_PTR PFN)
{

    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(zeroVA, 1, &PFN)) {
        fprintf(stderr, "error remapping zeroVA\n");
        return FALSE;
    }

    memset(zeroVA, 0, PAGE_SIZE);

    // unmap zeroVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(zeroVA, 1, NULL)) {
        fprintf(stderr, "error zeroing page\n");
        return FALSE;
    }
    return TRUE;
}

// 
void 
main() 
{
    // initialize free list
    LIST_ENTRY freeLink;
    freeLink.Flink = &freeLink;
    freeLink.Blink = &freeLink;

    freeListHead.head = freeLink;
    freeListHead.count = 0;

    // initialize standby list
    LIST_ENTRY standbyLink;
    standbyLink.Flink = &standbyLink;
    standbyLink.Blink = &standbyLink;

    standbyListHead.head = standbyLink;
    standbyListHead.count = 0;

    // initialize modified list
    LIST_ENTRY modifiedLink;
    modifiedLink.Flink = &modifiedLink;
    modifiedLink.Blink = &modifiedLink;

    modifiedListHead.head = modifiedLink;
    modifiedListHead.count = 0;

    // virtual alloc for zeroVA (since MEM_PHYSICAL is the one)
    zeroVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    // initialize memory

    // allocate an array of PFNs that are returned by AllocateUserPhysPages
    ULONG_PTR *aPFNs;
    aPFNs = VirtualAlloc(NULL, NUM_PAGES*(sizeof(ULONG_PTR)), MEM_COMMIT, PAGE_READWRITE);
    if (aPFNs == NULL) {
        fprintf(stderr, "failed to allocate PFNarray\n");
    }

    // secure privilege for the code
    BOOLEAN privResult;
    privResult = getPrivilege();
    BOOLEAN bResult;

    ULONG_PTR numPagesReturned = NUM_PAGES;
    bResult = AllocateUserPhysicalPages(GetCurrentProcess(), &numPagesReturned, aPFNs);
    if (bResult != TRUE) {
        fprintf(stderr, "could not allocate pages successfunlly \n");
        exit(-1);
    }

    if (numPagesReturned != NUM_PAGES) {
        fprintf(stderr, "allocated only %d pages out of %d pages requested\n", numPagesReturned, NUM_PAGES);
    }

    // create VAD
    leafVABlock = initVABlock(numPagesReturned, PAGE_SIZE);     // will be the starting VA of the memory I've allocated

    // create local PFN metadata array
    PFNarray = initPFNarray(aPFNs, numPagesReturned, PAGE_SIZE);

    // create local PTE array to map VAs to pages
    PTEarray = initPTEarray(numPagesReturned, PAGE_SIZE);

    PPTE currPTE = PTEarray;
    void* testVA = leafVABlock;


    printf("--------------------------------\n");
    // FAULTING in testVAs

    ULONG_PTR testNum = 10;
    printf("faulting in %d pages\n", testNum);
    for (int i = 0; i < testNum; i++) {
        faultStatus testStatus;
        if (i % 2 == 0) {
            testStatus = accessVA(testVA, WRITE);
        } else {
            testStatus = accessVA(testVA, READ);
        }
        printf("tested (VA = %d), return status = %d\n", (ULONG) testVA, testStatus);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }


    printf("--------------------------------\n");

    // TRIMMING tested VAs (active -> standby/modified)

    // reset testVa
    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {
        trimPage(testVA);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }


    printf("--------------------------------\n");

    // toggle - can either FAULT or TEST VAs (dependingon line 715/716)

    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {
        // faultStatus testStatus = isVAaccessible(testVA, WRITE);  // to TEST VAs
        faultStatus testStatus = pageFault(testVA, WRITE);      // to FAULT VAs
    
        printf("tested (VA = %d), return status = %d\n", (ULONG) testVA, testStatus);
        testVA = (void*) ( (ULONG) testVA + PAGE_SIZE);
    }
    printf("program complete\n");
    //  free memory allocated
    VirtualFree(leafVABlock, 0, MEM_RELEASE);
    VirtualFree(PFNarray, 0, MEM_RELEASE);
    VirtualFree(PTEarray, 0, MEM_RELEASE);
}