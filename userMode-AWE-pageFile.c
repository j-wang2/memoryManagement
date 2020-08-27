/*
 * Usermode virtual memory program intended as a rudimentary simulation
 * of Windows OS kernelmode memory management
 * 
 * Jason Wang, August 2020
 */

#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
#include "pageFile.h"
#include "getPage.h"
#include "PTEpermissions.h"
#include "VApermissions.h"
#include "pageFault.h"


/******* GLOBALS *****/
void* leafVABlock;                  // starting address of memory block
void* leafVABlockEnd;               // ending address of memory block

PPFNdata PFNarray;                  // starting address of PFN metadata array
PPTE PTEarray;                      // starting address of page table

void* zeroVA;                       // specific VA used for zeroing PFNs (via AWE mapping)

ULONG_PTR totalCommittedPages;      // count of committed pages (initialized to zero)
ULONG_PTR totalMemoryPageLimit = NUM_PAGES + PAGEFILE_SIZE / PAGE_SIZE;    // limit of committed pages (memory block + pagefile space)

void* pageFileVABlock;              // starting address of pagefile "disk" (memory)
void* modifiedWriteVA;              // specific VA used for writing out page contents to pagefile
void* pageFileFormatVA;             // specific VA used for copying in page contents from pagefile
ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];

// Execute-Write-Read (bit ordering)
ULONG_PTR permissionMasks[] = { 0, readMask, (readMask | writeMask), (readMask | executeMask), (readMask | writeMask | executeMask) };


listData listHeads[ACTIVE];         // intialization of listHeads array


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

    // divide offset by PAGE_SIZE
    pageTableIndex = offset >> PAGE_SHIFT;

    // get the corresponding page table entry from the PTE array
    PPTE currPTE;
    currPTE = PTEarray + pageTableIndex;

    return currPTE;
}


BOOLEAN
trimPage(void* virtualAddress)
{
    printf("trimming page with VA %u\n", (ULONG) virtualAddress);

    PPTE PTEaddress;
    PTEaddress = getPTE(virtualAddress);

    if (PTEaddress == NULL) {
        fprintf(stderr, "could not trim VA %u - no PTE associated with address\n", (ULONG) virtualAddress);
        return FALSE;
    }
    
    // take snapshot of old PTE
    PTE oldPTE;
    oldPTE = *PTEaddress;

    // check if PTE's valid bit is set - if not, can't be trimmed and return failure
    if (oldPTE.u1.hPTE.validBit == 0) {
        fprintf(stderr, "could not trim VA %u - PTE is not valid\n", (ULONG) virtualAddress);
        return FALSE;
    }

    // zero new PTE
    PTE PTEtoTrim;
    PTEtoTrim.u1.ulongPTE = 0;

    // unmap page from VA (invalidates hardwarePTE)
    MapUserPhysicalPages(virtualAddress, 1, NULL);

    // get pageNum
    ULONG_PTR pageNum;
    pageNum = oldPTE.u1.hPTE.PFN;

    // get PFN
    PPFNdata PFNtoTrim;
    PFNtoTrim = PFNarray + pageNum;

    // check dirtyBit to see if page has been modified
    if (oldPTE.u1.hPTE.dirtyBit == 0) {

        // add given VA's page to standby list
        enqueuePage(&standbyListHead, PFNtoTrim);

    } 
    else if (oldPTE.u1.hPTE.dirtyBit == 1) {

        // add given VA's page to modified list;
        enqueuePage(&modifiedListHead, PFNtoTrim);

    }

    // set transitionBit to 1
    PTEtoTrim.u1.tPTE.transitionBit = 1;  
    PTEtoTrim.u1.tPTE.PFN = pageNum;

    PTEtoTrim.u1.tPTE.permissions = getPTEpermissions(oldPTE);

    * (volatile PTE *) PTEaddress = PTEtoTrim;

    return TRUE;

}


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


PVOID 
initVABlock(int numPages, int pageSize)
{
    // initialize block of "memory" for use by program
    void* leafVABlock;

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

    PVOID commitCheckVA; 
    ULONG_PTR maxPFN = 0;
    for (int i = 0; i < numPages; i++) {
        if (maxPFN < aPFNs[i]) {
            maxPFN = aPFNs[i];
        }
    }

    // initialize PFN metadata array
    PPFNdata PFNarray;
    PFNarray = VirtualAlloc(NULL, (maxPFN+1)*(sizeof(PFNdata)), MEM_RESERVE, PAGE_READWRITE);

    if (PFNarray == NULL) {
        fprintf(stderr, "Could not allocate for PFN metadata array\n");
        exit(-1);
    }

    // loop through all PFNs, MEM_COMMITTING PFN metadata subsections for each in the aPFNs array
    for (int i = 0; i < numPages; i++){
        PPFNdata newPFN;
        newPFN = PFNarray + aPFNs[i];

        commitCheckVA = VirtualAlloc(newPFN, sizeof (PFNdata), MEM_COMMIT, PAGE_READWRITE);
        if (commitCheckVA == NULL) {
            fprintf(stderr, "failed to commit subsection of PFN array at PFN %d\n", i);
            exit(-1);
        }

        // increment committed page count
        totalCommittedPages++; 
    }
    
    // add all pages to freed list - can be combined with previous for loop
    for (int i = 0; i < numPages; i++) {

        PPFNdata currPFN = PFNarray + aPFNs[i];
        enqueuePage(&freeListHead, currPFN);        
    }

    return PFNarray;
}


PPTE
initPTEarray(int numPages, int pageSize)
{    
    // initialize PTE array
    PPTE PTEarray;
    PTEarray = VirtualAlloc(NULL, NUM_PAGES*(sizeof(PTE)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (PTEarray == NULL) {
        fprintf(stderr, "Could not allocate for PTEarray\n");
        exit(-1);
    }
    return PTEarray;
}


PVOID
initPageFile(int diskSize) 
{
    void* pageFileAddress;
    pageFileAddress = VirtualAlloc(NULL, diskSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (pageFileAddress == NULL) {
        fprintf(stderr, "Could not allocate for pageFile\n");
        exit(-1);
    }
    return pageFileAddress;
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


BOOLEAN
zeroPageWriter()
{
    printf("freeListCount == %llu\n", freeListHead.count);

    PPFNdata PFNtoZero;
    PFNtoZero = dequeuePage(&freeListHead);

    if (PFNtoZero == NULL) {
        fprintf(stderr, "free list empty - could not write out\n");
        return FALSE;
    }

    // get page number, rather than PFN metadata
    ULONG_PTR pageNumtoZero;
    pageNumtoZero = PFNtoZero - PFNarray;

    // zeroPage (does not update status bits in PFN metadata)
    zeroPage(pageNumtoZero);

    // enqueue to zeroList (updates status bits in PFN metadata)
    enqueuePage(&zeroListHead, PFNtoZero);
    printf(" - Moved page from free -> zero \n");

    return TRUE;
}


BOOLEAN
modifiedPageWriter()
{
    printf("modifiedListCount == %llu\n", modifiedListHead.count);
    PPFNdata PFNtoWrite;
    PFNtoWrite = dequeuePage(&modifiedListHead);

    if (PFNtoWrite == NULL) {
        fprintf(stderr, "modified list empty - could not write out\n");
        return FALSE;
    }


    BOOLEAN bResult;
    bResult = writePage(PFNtoWrite);

    if (bResult != TRUE) {
        fprintf(stderr, "error writing out page\n");
        return FALSE;
    }

    // PFN status to standby (from modified)
    PFNtoWrite->statusBits = STANDBY;

    // enqueue page to standby
    enqueuePage(&standbyListHead, PFNtoWrite);

    printf(" - Moved page from modified -> standby (wrote out to PageFile successfully)\n");

    return TRUE;   

}


VOID
initLinkHead(PLIST_ENTRY headLink)
{
    headLink->Flink = headLink;
    headLink->Blink = headLink;
}


VOID
initListHead(PlistData headData)
{
    initLinkHead(&(headData->head));
    headData->count = 0;
}

 
VOID 
main() 
{

    // initialize free/standby/modified lists
    for (int status = 0; status < ACTIVE; status++) {
        initListHead(&listHeads[status]);
    }

    // reserve AWE address for zeroVA
    zeroVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    // reserve AWE address for modifiedWriteVA 
    modifiedWriteVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    // reserve AWE address for pageFileFormatVA
    pageFileFormatVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    // memset the pageFile bit array
    memset(&pageFileBitArray, 0, PAGEFILE_PAGES/(8*sizeof(ULONG_PTR) ) );

    // allocate an array of PFNs that are returned by AllocateUserPhysPages
    ULONG_PTR *aPFNs;
    aPFNs = VirtualAlloc(NULL, NUM_PAGES*(sizeof(ULONG_PTR)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
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

    // create PageFile section of memory
    pageFileVABlock = initPageFile(PAGEFILE_SIZE);


    /************** TESTING *****************/
    PPTE currPTE = PTEarray;
    void* testVA = leafVABlock;


    printf("--------------------------------\n");


    /************* FAULTING in testVAs *****************/

    ULONG_PTR testNum = 10;
    printf("Committing and then faulting in %d pages\n", testNum);
    for (int i = 0; i < testNum; i++) {
        faultStatus testStatus;

        commitVA(testVA, READ_ONLY, 1);    // commits with READ_ONLY permissions
        protectVA(testVA, READ_WRITE, 1);

        // commitVA(testVA, READ_WRITE, 1);    // commits with read/write permissions (VirtualProtect does not allow execute permissions)
    
        if (i % 2 == 1) {

            // write VA to the VA location (should remain same throughout entire course of program despite physical page changes)
            testStatus = writeVA(testVA, testVA);

        } else {

            testStatus = accessVA(testVA, READ_ONLY);
            
        }

        printf("tested (VA = %x), return status = %u\n", (ULONG) testVA, testStatus);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);

    }


    printf("--------------------------------\n");


    /************ TRIMMING tested VAs (active -> standby/modified) **************/

    // reset testVa
    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {

        // trim the VAs we have just tested (to transition, from active)
        trimPage(testVA);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);

        // alternate calling modifiedPageWriter and zeroPageWriter
        if (i % 2 == 0) {

            modifiedPageWriter();

        } else {

            zeroPageWriter();

        }
    
    }


    printf("--------------------------------\n");

    // testVA = leafVABlock;

    // // toggle - can either FAULT or TEST VAs 
    // for (int i = 0; i < testNum; i++) {
    //     faultStatus testStatus = accessVA(testVA, READ_WRITE);  // to TEST VAs
    //     // faultStatus testStatus = pageFault(testVA, READ_ONLY);      // to FAULT VAs
    
    //     printf("tested (VA = %d), return status = %u\n", (ULONG) testVA, testStatus);
    //     testVA = (void*) ( (ULONG) testVA + PAGE_SIZE);
    // }

    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {
        // trimPage(testVA);

#define CHECK_PAGEFILE
#ifdef CHECK_PAGEFILE
        // to test PF fault - switch order in getPage and add this
        for (int j = 0; j < 3; j++) {
            getPage();
        }
#endif

        faultStatus testStatus = pageFault(testVA, READ_WRITE);  // to TEST VAs
        // faultStatus testStatus = pageFault(testVA, READ_ONLY);      // to FAULT VAs
        printf("tested (VA = %x), return status = %u\n", (ULONG) testVA, testStatus);
        testVA = (void*) ( (ULONG) testVA + PAGE_SIZE);
    }

    /***************** DECOMMITTING AND CHECKING VAs **************/

    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {

        printf("decommiting (VA = %x) with contents %x\n", (ULONG) testVA, * (ULONG *) testVA);
        printf("decommiting (VA = %x) with contents %s\n", (ULONG) testVA, * (PCHAR *) testVA);

        decommitVA(testVA, 1);

        testVA = (void*) ( (ULONG) testVA + PAGE_SIZE);
    }

    printf("----------------\nprogram complete\n----------------");

    //  free memory allocated
    VirtualFree(leafVABlock, 0, MEM_RELEASE);
    VirtualFree(PFNarray, 0, MEM_RELEASE);
    VirtualFree(PTEarray, 0, MEM_RELEASE);


}
