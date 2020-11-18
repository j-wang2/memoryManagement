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
#include "jLock.h"
#include "pageTrade.h"
#include "VADNodes.h"

/******************************************************
 *********************** GLOBALS **********************
 *****************************************************/
void* leafVABlock;                      // starting address of virtual memory block
void* leafVABlockEnd;                   // ending address of virtual memory block

PPFNdata PFNarray;                      // starting address of PFN metadata array
PPTE PTEarray;                          // starting address of page table

ULONG64 totalCommittedPages;               // count of committed pages (initialized to zero)
ULONG_PTR totalMemoryPageLimit = NUM_PAGES + (PAGEFILE_SIZE >> PAGE_SHIFT);    // limit of committed pages (memory block + pagefile space)

void* pageFileVABlock;                  // starting address of pagefile "disk" (memory)

#ifdef PAGEFILE_PFN_CHECK
PPageFileDebug pageFileDebugArray;
#else 
ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];
#endif

// Execute-Write-Read (bit ordering)
ULONG_PTR permissionMasks[] = { 0, readMask, (readMask | writeMask), (readMask | executeMask), (readMask | writeMask | executeMask) };

/************ List declarations *****************/
listData listHeads[ACTIVE];             // page listHeads array

listData zeroVAListHead;                // listHead of zeroVAs used for zeroing PFNs (via AWE mapping)
listData writeVAListHead;               // listHead of writeVAs used for writing to page file
listData readPFVAListHead;              // listHead of pagefile read VAs used for reading from pagefile
listData pageTradeVAListHead;

listData VADListHead;                   // list of VADs
listData readInProgEventListHead;       // list of events

/********** Locks ************/
PCRITICAL_SECTION PTELockArray;
CRITICAL_SECTION pageFileLock;

HANDLE physicalPageHandle;              // for multi-mapped pages (to support multithreading)

HANDLE wakeTrimHandle;
HANDLE wakeModifiedWriterHandle;

ULONG_PTR numPagesReturned;
ULONG_PTR virtualMemPages;

PULONG_PTR VADBitArray;

BOOLEAN debugMode;                      // toggled by -v flag on cmd line

#ifdef CHECK_PFNS
PULONG_PTR aPFNs;
#endif


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


BOOL
getPrivilege ()
{
    BOOL bResult;
    bResult = LoggedSetLockPagesPrivilege( GetCurrentProcess(), TRUE );
    return bResult;
}


VOID 
initVABlock(ULONG_PTR numPages)
{


    #ifdef MULTIPLE_MAPPINGS

    MEM_EXTENDED_PARAMETER extendedParameters = {0};
    extendedParameters.Type = MemExtendedParameterUserPhysicalHandle;
    extendedParameters.Handle = physicalPageHandle;


    leafVABlock = VirtualAlloc2(NULL, NULL, numPages << PAGE_SHIFT, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE, &extendedParameters, 1);      // equiv to numVAs*PAGE_SIZE

    #else
    // creates a VAD node that we can define (i.e. is not pagefaulted by underlying kernel mm)
    leafVABlock = VirtualAlloc(NULL, numPages*pageSize, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    #endif

    if (leafVABlock == NULL) {
        PRINT_ERROR("[initVABlock] unable to initialize VA block\n");
        exit(-1);
    }

    // Does not need to be checked for overflow since VirtualAlloc will not return a value
    leafVABlockEnd = (PVOID) ( (ULONG_PTR)leafVABlock + ( numPages << PAGE_SHIFT ) );   // equiv to numPages*PAGE_SIZE


}


VOID
initPFNarray(PULONG_PTR aPFNs, ULONG_PTR numPages)
{

    PVOID commitCheckVA; 
    ULONG_PTR maxPFN = 0;

    // loop through aPFNs array (from AllocateUserPhysicalPages) to find largest numerical PFN
    for (int i = 0; i < numPages; i++) {
        if (maxPFN < aPFNs[i]) {
            maxPFN = aPFNs[i];
        }
    }

    // VirtualAlloc (with MEM_RESERVE) PFN metadata array
    PFNarray = VirtualAlloc(NULL, (maxPFN+1)*(sizeof(PFNdata)), MEM_RESERVE, PAGE_READWRITE);

    if (PFNarray == NULL) {
        PRINT_ERROR("Could not allocate for PFN metadata array\n");
        exit(-1);
    }

    // loop through all PFNs, MEM_COMMITTING PFN subsections and enqueueing to free for each page
    for (int i = 0; i < numPages; i++) {
        PPFNdata newPFN;
        newPFN = PFNarray + aPFNs[i];

        commitCheckVA = VirtualAlloc(newPFN, sizeof (PFNdata), MEM_COMMIT, PAGE_READWRITE);
        
        if (commitCheckVA == NULL) {
            PRINT_ERROR("failed to commit subsection of PFN array at PFN %d\n", i);
            exit(-1);
        }


        //
        // Note: no lock needed functionally (simply to satisfy assert in enqueuePage)
        //

        acquireJLock(&newPFN->lockBits);

        //
        // "Reset"/clear pagefile offset & refcount PFN fields
        //

        newPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        newPFN->refCount = 0;

        //
        // add page to free list
        //

        enqueuePage(&freeListHead, newPFN);

        releaseJLock(&newPFN->lockBits);

    }

}


VOID
initPTEarray(ULONG_PTR numPages)
{    

    // VirtualAlloc (with MEM_RESERVE | MEM_COMMIT) for PTE array
    PTEarray = VirtualAlloc(NULL, numPages*(sizeof(PTE)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (PTEarray == NULL) {
        PRINT_ERROR("Could not allocate for PTEarray\n");
        exit(-1);
    }

}


VOID
initPageFile(ULONG_PTR diskSize) 
{

    // VirtualAlloc (with MEM_RESERVE | MEM_COMMIT) for pageFileVABlock
    pageFileVABlock = VirtualAlloc(NULL, diskSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (pageFileVABlock == NULL) {
        PRINT_ERROR("Could not allocate for pageFile\n");
        exit(-1);
    }

    #ifndef PAGEFILE_OFF

    InterlockedIncrement64(&totalCommittedPages); // todo - (currently used as a "working " page)

    #else
    for (int i = 0; i < PAGEFILE_PAGES; i++) {

        InterlockedIncrement64(&totalCommittedPages); // (currently used as a "working " page)

    }
    #endif

}


ULONG_PTR
allocatePhysPages(ULONG_PTR numPages, PULONG_PTR aPFNs) 
{

    // secure privilege for the code
    BOOL privResult;
    privResult = getPrivilege();
    if (privResult != TRUE) {
        PRINT_ERROR("could not get privilege successfully \n");
        exit(-1);
    }    


    ULONG_PTR numPagesAllocated;
    numPagesAllocated = numPages;



    #ifdef MULTIPLE_MAPPINGS

    MEM_EXTENDED_PARAMETER extendedParameters = {0};


    extendedParameters.Type = MemSectionExtendedParameterUserPhysicalFlags;
    extendedParameters.ULong64 = 0;
    
    physicalPageHandle = CreateFileMapping2(NULL,
                                            NULL, 
                                            SECTION_MAP_READ | SECTION_MAP_WRITE, 
                                            PAGE_READWRITE, 
                                            SEC_RESERVE, 
                                            0, 
                                            NULL, 
                                            &extendedParameters, 
                                            1 );
    
    if (physicalPageHandle == NULL) {
        PRINT_ERROR("could not create file mapping\n");
        exit(-1);
    }


    #else
    
    physicalPageHandle = GetCurrentProcess();

    #endif

    BOOL bResult;
    bResult = AllocateUserPhysicalPages(physicalPageHandle, &numPagesAllocated, aPFNs);

    if (bResult != TRUE) {
        PRINT_ERROR("could not allocate pages successfully \n");
        exit(-1);
    }

    if (numPagesAllocated != numPages) {
        PRINT("allocated only %llu pages out of %u pages requested\n", numPagesAllocated, NUM_PAGES);
    }

    return numPagesAllocated;


}


BOOLEAN
zeroPage(ULONG_PTR PFN)
{

    PVANode zeroVANode;
    zeroVANode = dequeueLockedVA(&zeroVAListHead);

    while (zeroVANode == NULL) {
        
        PRINT("[zeroPage] Waiting for release of event node (list empty)\n");

        WaitForSingleObject(zeroVAListHead.newPagesEvent, INFINITE);

        zeroVANode = dequeueLockedVA(&zeroVAListHead);

    }

    PVOID zeroVA;
    zeroVA = zeroVANode->VA;

    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(zeroVA, 1, &PFN)) {
        PRINT_ERROR("error remapping zeroVA\n");
        enqueueVA(&zeroVAListHead, zeroVANode);
        return FALSE;
    }


    memset(zeroVA, 0, PAGE_SIZE);

    // unmap zeroVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(zeroVA, 1, NULL)) {
        PRINT_ERROR("error zeroing page\n");
        enqueueVA(&zeroVAListHead, zeroVANode);
        return FALSE;
    }

    enqueueVA(&zeroVAListHead, zeroVANode);

    return TRUE;

}


BOOLEAN
zeroPageWriter()
{
    //
    // Acquires & releases both list and page locks
    //

    PPFNdata PFNtoZero;
    PFNtoZero = dequeueLockedPage(&freeListHead, TRUE);


    if (PFNtoZero == NULL) {

        PRINT("free list empty - could not write out\n");
        return FALSE;

    }

    //
    // Set overloaded "writeinprogress" bit (to signify page is currently being zeroed)
    // Note: write in progress is also set by modified page writer, but in a different context (todo check)
    //

    PFNtoZero->writeInProgressBit = 1;

    releaseJLock(&PFNtoZero->lockBits);


    // Derive page number from PFN metadata
    ULONG_PTR pageNumtoZero;
    pageNumtoZero = PFNtoZero - PFNarray;


    // zero the page contents, does not update status bits in PFN metadata
    // note: can be page traded within this time, where status bits could change from ZERO to AWAITING_QUARANTINE
    zeroPage(pageNumtoZero);


    acquireJLock(&PFNtoZero->lockBits);


    // clear "writeinprogress" bit (to signify page has been zeroed and can now be traded, once lock is released)
    PFNtoZero->writeInProgressBit = 0;

    if (PFNtoZero->statusBits == AWAITING_QUARANTINE) {

        enqueuePage(&quarantineListHead, PFNtoZero);
        PRINT(" - moved page from free -> quarantine\n");

    } else {


        #ifdef PAGEFILE_PFN_CHECK
        enqueuePageBasic(&zeroListHead, PFNtoZero);
        #else
        // enqueue to zeroList (updates status bits in PFN metadata)
        enqueuePage(&zeroListHead, PFNtoZero);
        #endif

    }

    releaseJLock(&PFNtoZero->lockBits);

    PRINT(" - Moved page from free -> zero \n");

    return TRUE;

}


DWORD WINAPI
zeroPageThread(HANDLE terminationHandle)
{

    int numZeroed = 0;
    int numWaited = 0;

    // create local handle array
    HANDLE handleArray[2];
    handleArray[0] = terminationHandle;
    handleArray[1] = freeListHead.newPagesEvent;

    // zero pages until none left to zero
    while (TRUE) {

        BOOLEAN bres;

        //
        // zeroPageWriter returns false on empty zero page list. 
        // Thus, in the case it returns false, it will wait for either
        // a new page event or termination handle.
        //

        bres = zeroPageWriter();

        if (bres == FALSE) {

            DWORD retVal = WaitForMultipleObjects(2, handleArray, FALSE, INFINITE);
            DWORD index = retVal - WAIT_OBJECT_0;

            if (index == 0) {
                PRINT_ALWAYS("zeropagethread - numPages moved to zero list : %d, numWaited : %d\n", numZeroed, numWaited);
                return 0;
            }

            //  PRINT("NEW PAGES in free list\n");
            numWaited++;

            continue;
        }
        numZeroed++;
        // return TRUE;
    }

}


BOOLEAN
freePageTestWriter()
{

    PPFNdata PFNtoFree;
    PFNtoFree = dequeueLockedPage(&zeroListHead, FALSE);

    if (PFNtoFree == NULL) {
        PRINT("zero list empty - could not write out\n");
        return FALSE;
    }

    acquireJLock(&PFNtoFree->lockBits);

    #ifdef PAGEFILE_PFN_CHECK
    enqueuePageBasic(&freeListHead, PFNtoFree);
    #else
    // enqueue to freeList (updates status bits in PFN metadata)
    enqueuePage(&freeListHead, PFNtoFree);
    #endif

    releaseJLock(&PFNtoFree->lockBits);

    return TRUE;
}


DWORD WINAPI
freePageTestThread(HANDLE terminationHandle)
{

    int numFreed = 0;
    int numWaited = 0;

    // create local handle array
    HANDLE handleArray[2];
    handleArray[0] = terminationHandle;
    handleArray[1] = zeroListHead.newPagesEvent;

    // zero pages until none left to zero
    while (TRUE) {
        BOOLEAN bres;
        bres = freePageTestWriter();
        if (bres == FALSE) {

            DWORD retVal = WaitForMultipleObjects(2, handleArray, FALSE, INFINITE);
            DWORD index = retVal - WAIT_OBJECT_0;

            if (index == 0) {
                PRINT_ALWAYS("freepagetestthread - numPages moved to free list: %d, numWaited : %d \n", numFreed, numWaited);
                return 0;
            }

            //  PRINT("NEW PAGES in zero list\n");
            numWaited++;
            continue;
        }
        numFreed++;

    }
}


VOID
releaseAwaitingFreePFN(PPFNdata PFNtoFree)
{

    //
    // Verify page lock is held
    //

    ASSERT(PFNtoFree->lockBits == 1);
        
    //
    // Clear index in pagefile, pagefile offset field in PFN,
    // and remodified bit in PFN.
    //

    clearPFBitIndex(PFNtoFree->pageFileOffset);

    PFNtoFree->pageFileOffset = INVALID_BITARRAY_INDEX;

    PFNtoFree->remodifiedBit = 0;

    //
    // Enqueue Page to free list
    //

    enqueuePage(&freeListHead, PFNtoFree);

    PRINT("[releaseAwaitingFreePFN] VA decommitted during PF write\n");

}



volatile ULONG_PTR ctrs[4];

BOOLEAN
modifiedPageWriter()
{

    BOOLEAN wakeModifiedWriter;
    PPFNdata PFNtoWrite;

    wakeModifiedWriter = FALSE;

    PRINT("[modifiedPageWriter] modifiedListCount == %llu\n", modifiedListHead.count);

    //
    // Return page with lock bits set
    //

    PFNtoWrite = dequeueLockedPage(&modifiedListHead, TRUE);

    if (PFNtoWrite == NULL) {

        PRINT("[modifiedPageWriter] modified list empty - could not write out\n");
        return FALSE;

    }

    //
    // Lock has previuosly been acquired - assert that PFN has no associated pagefile
    // offset ad that write in progress bit is zero. Then set write in progress bit to 1
    //

    ASSERT(PFNtoWrite->pageFileOffset == INVALID_BITARRAY_INDEX);

    ASSERT(PFNtoWrite->writeInProgressBit == 0);

    PFNtoWrite->writeInProgressBit = 1;

    //
    // Revert PFN status to modified so that it can once again be faulted
    //

    PFNtoWrite->statusBits = MODIFIED;
    
    //
    // Clear PFN's remodified bit (since it will be written/attempted to be written
    // to pagefile), and can now be re-set once page lock is released
    //

    PFNtoWrite->remodifiedBit = 0;

    //
    // Release PFN lock: write in progress bit has been set, status bits have
    // been set to modified, PFN dirty bit has been cleared so that another
    // faulter can reset it upon checking that write in progress bit is 1.
    // Page can be now decommitted or re-modified
    //

    releaseJLock(&PFNtoWrite->lockBits);

    ULONG_PTR expectedSig;
    expectedSig = (ULONG_PTR) leafVABlock + ( ( PFNtoWrite->PTEindex ) << PAGE_SHIFT );

    //
    // Write page to pagefile - can no longer be accessed since write in progress is set
    //

    BOOLEAN bResult;
    bResult = writePageToFileSystem(PFNtoWrite, expectedSig);

    //
    // Re-acquire PFN lock post-pagefile write
    //

    acquireJLock(&PFNtoWrite->lockBits);

    // since we've marked the PFN as write in progress, it CANNOT be in zero or free state
    //  - would require a decommit (via decommitVA), which checks the writeInProgressBit
    //  - so, we can assert that it is in neither free nor zero
    ASSERT (PFNtoWrite->statusBits != FREE && PFNtoWrite->statusBits != ZERO);

    //
    // clear write in progress bit (since page has been written)
    //

    ASSERT(PFNtoWrite->writeInProgressBit == 1);

    PFNtoWrite->writeInProgressBit = 0;

    PTE logPTE;
    logPTE.u1.ulongPTE = bResult;

    PTE zeroPTE;
    zeroPTE.u1.ulongPTE = 0;

    logEntry( PTEarray + PFNtoWrite->PTEindex, logPTE, zeroPTE, PFNtoWrite);

    // check if PFN has been decommitted
    if (PFNtoWrite->statusBits == AWAITING_FREE) {
        
        releaseAwaitingFreePFN(PFNtoWrite);

        releaseJLock(&PFNtoWrite->lockBits);

        return TRUE;

    }
    
    //
    // If filesystem write fails
    //

    if (bResult != TRUE) {

        //
        // Since write has failed, re-set PFN dirty bit, clear
        // PF bit index and clear pagefile offset out of PFN
        //


        clearPFBitIndex(PFNtoWrite->pageFileOffset);

        PFNtoWrite->pageFileOffset = INVALID_BITARRAY_INDEX;

        //
        // If the page has not since been faulted in, re-enqueue to modified list
        // (since page has failed the write to pagefile, cannnot be enqueued to 
        // standby list)
        //

        if (PFNtoWrite->statusBits != ACTIVE) {

            PFNtoWrite->remodifiedBit = 0;

            wakeModifiedWriter = enqueuePage(&modifiedListHead, PFNtoWrite);

        } else {
            
            PFNtoWrite->remodifiedBit = 1;

        }

        releaseJLock(&PFNtoWrite->lockBits);


        PRINT("[modifiedPageWriter] error writing out page\n");

        
        return FALSE;

    }

    //
    // If page has been re-modified
    //

    if (PFNtoWrite->remodifiedBit == 1) {

        //
        // Since write failure (bRes == FALSE) would leave pagefile offset as invalid
        //

        ASSERT(PFNtoWrite->pageFileOffset != INVALID_BITARRAY_INDEX);

        PFNtoWrite->remodifiedBit = 0;
            
        //
        // Clear pagefile space from bitarray and clear index in PFN
        //

        clearPFBitIndex(PFNtoWrite->pageFileOffset);

        PFNtoWrite->pageFileOffset = INVALID_BITARRAY_INDEX;


        // If the page has not since been faulted in, re-enqueue to modified list
        // (since page has been re-modified, i.e. write->write fault->trim)
        if (PFNtoWrite->statusBits != ACTIVE) {

            wakeModifiedWriter = enqueuePage(&modifiedListHead, PFNtoWrite);

        }

        releaseJLock(&PFNtoWrite->lockBits);

        if (wakeModifiedWriter == TRUE) {
            
            BOOL bRes;
            bRes = SetEvent(wakeModifiedWriterHandle);

            if (bRes != TRUE) {
                PRINT_ERROR("[trimPTE] failed to set event\n");
            }

            ResetEvent(wakeModifiedWriterHandle);

        }

        PRINT("[modifiedPageWriter] Page has since been modified, clearing PF space\n");

        ctrs[3]++;
        
        return TRUE;
    }

    //
    // CurrPTE must be "good" since PFN has not been decommitted (we hold lock)
    // can be either valid or transition (dangling since we hold write in progress bit)
    //

    PPTE currPTE;
    currPTE = PTEarray + PFNtoWrite->PTEindex;    


    // if PFN has not since been faulted in to active, enqueue to standby list
    // if it has been write faulted in, check the dirty bit
    //  - if it is set, clear the PF bit index
    //  - otherwise, continue
    if (PFNtoWrite->statusBits != ACTIVE && PFNtoWrite->remodifiedBit == 0) {

        ASSERT(currPTE->u1.hPTE.validBit != 1 && currPTE->u1.tPTE.transitionBit == 1);

        // enqueue page to standby (since has not been redirtied)
        enqueuePage(&standbyListHead, PFNtoWrite);

        PRINT(" - Moved page from modified -> standby (wrote out to PageFile successfully)\n");


    }

    releaseJLock(&PFNtoWrite->lockBits);

    return TRUE;   

}


DWORD WINAPI
modifiedPageThread(HANDLE terminationHandle)
{

    int numWrittenOut = 0;
    int numWaited = 0;

    // create local handle array
    HANDLE handleArray[2];
    handleArray[0] = terminationHandle;
    handleArray[1] = wakeModifiedWriterHandle;



    // write out modified pages to pagefile until modified page list empty
    while (TRUE) {

        BOOLEAN bres;
        bres = modifiedPageWriter();

        if (bres == FALSE) {

            DWORD retVal = WaitForMultipleObjects(2, handleArray, FALSE, 1000);
            DWORD index = retVal - WAIT_OBJECT_0;

            if (index == 0) {
                PRINT_ALWAYS("modifiedPageThread - numPages moved to standby list: %d, numWaited : %d \n", numWrittenOut, numWaited);
                return 0;
            }

            numWaited++;
            continue;
        }
        numWrittenOut++;

    }
}


BOOLEAN
faultAndAccessTest()
{
    

    PVOID testVA;
    BOOLEAN bRes;


    PRINT_ALWAYS("Fault and access test\n");

    /************** TESTING *****************/
    testVA = leafVABlock;


    /************* FAULTING in and WRITING/ACCESSING testVAs *****************/


    // bRes = commitVA(testVA, READ_WRITE, virtualMemPages << PAGE_SHIFT);     // commits with READ_ONLY permissions
    // protectVA(testVA, READ_WRITE, virtualMemPages << PAGE_SHIFT);   // converts to read write

    // if (bRes != TRUE) {

    //     PRINT("Unable to commit pages \n");

    // }


    for (int i = 0; i < virtualMemPages; i++) {

        faultStatus testStatus;
        bRes = commitVA(testVA, READ_WRITE, 1);     // commits with READ_ONLY permissions

        //
        // Write->Trim->Access
        //

        testStatus = writeVA(testVA, testVA);

        trimVA(testVA);

        testStatus = accessVA(testVA, READ_ONLY);

        /************ TRADING pages ***********/

        #ifdef TRADE_PAGES

        PRINT("Attempting to trade VA\n");
        tradeVA(testVA);

        #endif

        PRINT("tested (VA = %llu), return status = %u\n", (ULONG_PTR) testVA, testStatus);

        // iterate to next VA
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);

    }


    /************ TRIMMING tested VAs (active -> standby/modified) **************/

    PRINT("Trimming %llu pages, modified writing half of them\n", virtualMemPages);

    // reset testVa
    testVA = leafVABlock;

    for (int i = 0; i < virtualMemPages; i++) {

        // trim the VAs we have just tested (to transition, from active)
        trimVA(testVA);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);


        #ifndef MULTITHREADING

        // alternate calling modifiedPageWriter and zeroPageWriter
        if (i % 2 == 0) {

            modifiedPageWriter();

        } 

        else {
            
            zeroPageWriter();

        }
        #endif
    
    }


    /****************** FAULTING back in trimmed pages ******************/

    testVA = leafVABlock;

    for (int i = 0; i < virtualMemPages; i++) {


        #ifdef CHECK_PAGEFILE
        // "leaks" pages in order to force standby->pf repurposing
        for (int j = 0; j < 3; j++) {
            getPage(FALSE);
        }
        #endif

        faultStatus testStatus = accessVA(testVA, READ_WRITE);        // to TEST VAs

        PRINT("tested (VA = %llu), return status = %u\n", (ULONG_PTR) testVA, testStatus);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }


    /***************** DECOMMITTING AND CHECKING VAs **************/

    testVA = leafVABlock;
    bRes = decommitVA(testVA, virtualMemPages << PAGE_SHIFT);

    if (bRes != TRUE) {

        DebugBreak();

    }


    return TRUE;

}


DWORD WINAPI
faultAndAccessTestThread(HANDLE terminationHandle) 
{

    #ifdef CONTINUOUS_FAULT_TEST

    while (TRUE) {
        
        // write out modified pages to pagefile until modified page list empty
        BOOLEAN bres;
        bres = faultAndAccessTest();


        DWORD waitRes;
        waitRes = WaitForSingleObject(terminationHandle, 100);

        if (waitRes == WAIT_TIMEOUT) {
            continue;
        }
        else if (waitRes == WAIT_OBJECT_0) {
            PRINT_ALWAYS("faultAndAccessTest thread exiting\n");
            return 0;
        } else if (waitRes == WAIT_ABANDONED) {
            PRINT_ERROR("wait abandoned\n");

        } else if (waitRes == WAIT_FAILED) {
            PRINT_ERROR("wait failed\n");
        }

    }

    #else

    // write out modified pages to pagefile until modified page list empty
    BOOLEAN bres;
    bres = faultAndAccessTest();

    WaitForSingleObject(terminationHandle, INFINITE);
    return 0;

    #endif
}


ULONG_PTR
trimValidPTEs()
{
    
    PPTE currPTE;
    PPTE endPTE;
    ULONG_PTR numTrimmed;
    ULONG_PTR PTEsInRange;
    BOOLEAN trimActive;
    
    ULONG_PTR numAvailablePages;

    trimActive = FALSE;

    //
    // Calculate PTEs in range
    //

    PTEsInRange = (((ULONG_PTR)leafVABlockEnd - (ULONG_PTR)leafVABlock) / PAGE_SIZE);

    endPTE = PTEarray + PTEsInRange;

    //
    // Randomize starting PTE
    //

    currPTE = PTEarray;

    currPTE += (GetTickCount() % PTEsInRange);

    numTrimmed = 0;

    // for (currPTE; currPTE < endPTE; currPTE++ ) {
    for (int i = 0; i < PTEsInRange; i++) {

        if (currPTE == endPTE) {

            currPTE = PTEarray;

        }

        //
        // If currPTE is valid/active
        //

        acquirePTELock(currPTE);        // TODO - make a copy of snap PTE and edit that indivisibly


        if (currPTE->u1.hPTE.validBit == 1) {

            //
            // If aging bit remains set
            //

            if (currPTE->u1.hPTE.agingBit == 1) {

                BOOLEAN bRes;
                bRes = trimPTE(currPTE);

                if (bRes == FALSE) {

                    DebugBreak();

                } else {

                    numTrimmed++;
                    
                }
            }
            else {

                currPTE->u1.hPTE.agingBit = 1;

            }

        }

        releasePTELock(currPTE);
        
        currPTE++;

    }

    //
    // Randomize starting PTE
    //

    currPTE = PTEarray;

    currPTE += (GetTickCount() % PTEsInRange);

    //
    // Calculate approximate number of available pages
    // (no listhead lock acquisition, so subject to error)
    //

    numAvailablePages = 0;
    for (int i = 0; i < STANDBY + 1; i ++ ) {
        EnterCriticalSection(&listHeads[i].lock);

        numAvailablePages += listHeads[i].count;
    }

    for (int i = STANDBY; i >= 0; i--) {

        LeaveCriticalSection(&listHeads[i].lock);

    }


    if (numAvailablePages < MIN_AVAILABLE_PAGES) {

        ULONG_PTR numPTEsToTrim;

        numPTEsToTrim = MIN_AVAILABLE_PAGES - numAvailablePages + 20;

        //
        // Randomize starting PTE
        //

        currPTE = PTEarray;

        currPTE += (GetTickCount() % PTEsInRange);

        for (int i = 0; i < numPTEsToTrim; i++) {

            if (currPTE == endPTE) {

                currPTE = PTEarray;

            }

            //
            // If currPTE is valid/active
            //

            acquirePTELock(currPTE);        // TODO - make a copy of snap PTE and edit that indivisibly


            if (currPTE->u1.hPTE.validBit == 1) {

                BOOLEAN bRes;
                bRes = trimPTE(currPTE);

                if (bRes == FALSE) {

                    DebugBreak();

                } else {

                    numTrimmed++;
                    
                }


            }

            releasePTELock(currPTE);
        
            currPTE++;
        }

    }


    return numTrimmed;


}


DWORD WINAPI
trimValidPTEThread(HANDLE terminationHandle) 
{

    // write out modified pages to pagefile until modified page list empty
    ULONG_PTR numTrimmed;
    numTrimmed = 0;

    
    // create local handle array
    HANDLE handleArray[2];
    handleArray[0] = terminationHandle;
    handleArray[1] = wakeTrimHandle;

    while (TRUE) {

        DWORD retVal;

        //
        // Note: efficacy is tied to implementation of checkAvailablePages,
        // where this could deadlock if there was no timeout while
        // waiting on this event. This scenario could occur if all
        // trimming threads were actively trimming and this dequeue 
        // was called by a pagefault, that in turn waited infinitely
        // on new page events.
        //

        retVal = WaitForMultipleObjects(2, handleArray, FALSE, 200);

        DWORD index = retVal - WAIT_OBJECT_0;

        if (index == 0) {

            PRINT_ALWAYS("trimPTEthread - numPTEs trimmed : %llu\n", numTrimmed);
            return 0;

        } 

        ULONG_PTR currNum;
        currNum = trimValidPTEs();

        numTrimmed += currNum;

    }

    return 0;
}

#ifdef CHECK_PFNS

volatile BOOLEAN checkPages;


DWORD WINAPI
checkPageStatusThread(HANDLE terminationHandle) 
{


    while (TRUE) {

        if (checkPages) {

            PPFNdata currPFN;

            ULONG_PTR numPages = 0;

            for (int j = 0; j < numPagesReturned; j++) {

                currPFN = PFNarray + aPFNs[j];

                acquireJLock(&currPFN->lockBits);
                
                if (currPFN->statusBits != MODIFIED) {

                    DebugBreak();

                }

                numPages++;

                releaseJLock(&currPFN->lockBits);
            }

            PRINT_ALWAYS("numpages: %llu\n", numPages);


            #ifdef PAGEFILE_PFN_CHECK
            PPageFileDebug currPF;

            EnterCriticalSection(&pageFileLock);

            for (int k = 0; k < PAGEFILE_PAGES; k++) {

                currPF = pageFileDebugArray + k;

                PPFNdata PFN;
                PFN = currPF->currPFN;

                ULONG_PTR PFNindex;
                PFNindex = PFN - PFNarray;

                PPageFileDebug secondCurr;
                PPFNdata secondCurrPFN;

                for (int l = k + 1; l < PAGEFILE_PAGES - 1; l++) {

                    secondCurr = pageFileDebugArray + l;

                    secondCurrPFN = secondCurr->currPFN;


                    ASSERT (memcmp(PFN, secondCurrPFN, sizeof(PFNdata) != 0) );
                }

            }

            LeaveCriticalSection(&pageFileLock);

            #endif


        }

        DWORD retVal;

        retVal = WaitForSingleObject(terminationHandle, 200);

        if (retVal == WAIT_TIMEOUT) {

            continue;

        }
        else if (retVal == WAIT_OBJECT_0) {

            PRINT_ALWAYS("checkPageStatus thread exiting\n");
            return 0;

        } else if (retVal == WAIT_ABANDONED) {

            PRINT_ERROR("wait abandoned\n");

        } else if (retVal == WAIT_FAILED) {

            PRINT_ERROR("wait failed\n");

        }



    }

    return 0;
}

#endif


VOID
initLinkHead(PLIST_ENTRY headLink)
{
    headLink->Flink = headLink;
    headLink->Blink = headLink;
}


VOID
initListHead(PlistData headData)
{

    // initialize lock field
    InitializeCriticalSection(&(headData->lock));

    // initialize head
    initLinkHead(&(headData->head));

    // initialize count
    headData->count = 0;

    HANDLE pagesCreatedHandle;
    pagesCreatedHandle = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (pagesCreatedHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
        exit(-1);
    }

    headData->newPagesEvent = pagesCreatedHandle;

}


VOID 
initListHeads(PlistData listHeadArray)
{
    // initialize free/standby/modified lists
    for (int status = 0; status < ACTIVE; status++) {
        initListHead(&listHeadArray[status]);
    }

}
 

VOID
initVAList(PlistData VAListHead, ULONG_PTR numVAs)
{

    if (numVAs < 1) {
        PRINT_ERROR("[initVAList] Cannot initialize list of VAs with length 0\n");
        exit (-1);
    }


    initListHead(VAListHead);

    //
    // Call VirtualAlloc for VAs
    //

    void* baseVA;

    #ifdef MULTIPLE_MAPPINGS

    MEM_EXTENDED_PARAMETER extendedParameters = {0};

    
    extendedParameters.Type = MemExtendedParameterUserPhysicalHandle;
    extendedParameters.Handle = physicalPageHandle;

    baseVA = VirtualAlloc2(NULL, NULL, numVAs << PAGE_SHIFT, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE, &extendedParameters, 1);      // equiv to numVAs*PAGE_SIZE

    #else

    baseVA = VirtualAlloc(NULL, numVAs << PAGE_SHIFT, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);      // equiv to numVAs*PAGE_SIZE

    #endif


    // alloc for nodes
    PVANode baseNode;
    baseNode = VirtualAlloc(NULL, numVAs * sizeof(VANode), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    PVANode currNode;
    void* currVA;

    // for each VA, allocate memory and link onto head

    for (int i = 0; i < numVAs; i++) {

        //get address of current node
        currNode = baseNode + i;

        // get address of current address
        currVA = (void*) ( (ULONG_PTR)baseVA + ( i << PAGE_SHIFT ) );   // equiv to i*PAGE_SIZE

        currNode->VA = currVA;

        // enqueue node to list
        enqueueVA(VAListHead, currNode);
    }
    
}


VOID
freeVAList(PlistData VAListHead)
{

    PVANode currNode;
    PLIST_ENTRY currLinks;
    PLIST_ENTRY nextLinks;
    PVANode baseNode;

    baseNode = NULL;
    currLinks = VAListHead->head.Flink;

    //
    // Iterate through nodes to find the original base address
    //

    while (currLinks != &VAListHead->head) {
    
        currNode = CONTAINING_RECORD(currLinks, VANode, links);

        // VirtualFree(currNode->VA, 0, MEM_RELEASE);

        if (baseNode == NULL || (PVOID) currNode < (PVOID) baseNode) {

            baseNode = currNode;
            
        }

        nextLinks = currNode->links.Flink;

        currLinks = nextLinks;

    }

    //
    // Free VA's from the nodes
    //

    VirtualFree(baseNode->VA, 0, MEM_RELEASE);

    //
    // Free nodes from the original base address
    //

    VirtualFree(baseNode, 0, MEM_RELEASE);

}


VOID
initEventList(PlistData eventListHead, ULONG_PTR numEvents)
{

    PeventNode baseNode;
    PeventNode currNode;

    if (numEvents < 1) {
        PRINT_ERROR("[initEventList] cannot initialize list of Events with length < 1\n");
        exit (-1);
    }
    
    initListHead(eventListHead);

    baseNode = VirtualAlloc(NULL, numEvents * sizeof(eventNode), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    if (baseNode == NULL) {
        PRINT_ERROR("failed to alloc for baseNode handle\n")
        exit(-1);
    }
    
    for (int i = 0; i < numEvents; i++) {

        currNode = baseNode + i;

        currNode->event = CreateEvent(NULL, TRUE, FALSE, NULL);

        if (currNode->event == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create event handle\n")
        }

        enqueueEvent(eventListHead, currNode);

    }

}


VOID
freeEventList(PlistData eventListHead)
{

    PeventNode currNode;
    PLIST_ENTRY currLinks;
    PLIST_ENTRY nextLinks;
    PVOID baseAddress;

    baseAddress = NULL;
    currLinks = eventListHead->head.Flink;

    //
    // Iterate through nodes to find the original base address
    //

    while (currLinks != &eventListHead->head) {
    
        currNode = CONTAINING_RECORD(currLinks, eventNode, links);

        BOOL bRes;
        bRes = CloseHandle(currNode->event);

        if (bRes != TRUE){

            PRINT_ERROR("Failed to close handle\n");

        }

        if (baseAddress == NULL || (PVOID) currNode < baseAddress) {

            baseAddress = (PVOID) currNode;

        }

        nextLinks = currNode->links.Flink;

        currLinks = nextLinks;

    }

    //
    // Free from the original base address
    //

    VirtualFree(baseAddress, 0, MEM_RELEASE);

}


VOID
initVADList()
{

    InitializeCriticalSection(&(VADListHead.lock));

    initLinkHead(&(VADListHead.head));

    VADListHead.count = 0;

}


VOID
testRoutine()
{
    PRINT_ALWAYS("[testRoutine]\n");


    #ifdef MULTITHREADING

    /************* Creating handles/threads *************/
    PRINT_ALWAYS("Creating threads\n");

    //
    // Handle to terminate all threads
    //

    HANDLE terminateWorkingThreadsHandle;
    terminateWorkingThreadsHandle =  CreateEvent(NULL, TRUE, FALSE, NULL);

    if (terminateWorkingThreadsHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
    }

    HANDLE terminateTestingThreadsHandle;
    terminateTestingThreadsHandle =  CreateEvent(NULL, TRUE, FALSE, NULL);

    if (terminateTestingThreadsHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
    }


    PRINT_ALWAYS(" - %d zeroPage threads\n", NUM_THREADS);

    HANDLE zeroPageThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        zeroPageThreadHandles[i] = CreateThread(NULL, 0, zeroPageThread, terminateWorkingThreadsHandle, 0, NULL);
            
        if (zeroPageThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create zeroPage handle\n");
        }
  
    }  


    #ifdef TESTING_ZERO
    /************ for testing of zeropagethread **************/

    PRINT_ALWAYS(" - %d freePage threads\n", NUM_THREADS);

    HANDLE freePageTestThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        freePageTestThreadHandles[i] = CreateThread(NULL, 0, freePageTestThread, terminateWorkingThreadsHandle, 0, NULL);

        if (freePageTestThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create freePage handle\n");
        }
    }
    #endif

    #ifdef TESTING_MODIFIED

    PRINT_ALWAYS(" - %d modifiedWriter threads\n", NUM_THREADS);

    HANDLE modifiedPageWriterThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        modifiedPageWriterThreadHandles[i] = CreateThread(NULL, 0, modifiedPageThread, terminateWorkingThreadsHandle, 0, NULL);
            
        if (modifiedPageWriterThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create modifiedPageWriting handle\n");
        }
  
    }
    #endif

    PRINT_ALWAYS(" - %d PTE trimming threads\n", NUM_THREADS);

    HANDLE trimThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        trimThreadHandles[i] = CreateThread(NULL, 0, trimValidPTEThread, terminateWorkingThreadsHandle, 0, NULL);
            
        if (trimThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create modifiedPageWriting handle\n");
        }
  
    }

    /**************************************/


    PRINT_ALWAYS(" - %d access/fault test threads\n", NUM_THREADS);

    HANDLE testHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        testHandles[i] = CreateThread(NULL, 0, faultAndAccessTestThread, terminateTestingThreadsHandle, 0, NULL);
            
        if (testHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create modifiedPageWriting handle\n");
        }
  
    }

    #ifdef CHECK_PFNS
    HANDLE pageTestThread;

    pageTestThread = CreateThread(NULL, 0, checkPageStatusThread, terminateWorkingThreadsHandle, 0 , NULL);
    #endif

    char quitChar;
    quitChar = 'a';
    while (quitChar != 'q' && quitChar != 'f') {
        quitChar = (char) getchar();

        #ifdef CHECK_PFNS
        if (quitChar == 'b') {
            checkPages = !checkPages;
        }
        #endif
    }
    PRINT_ALWAYS("qchar: %c, %d\n", quitChar, quitChar);

    PRINT_ALWAYS("Ending program\n");




    //
    // Testing threads MUST exit prior to working threads
    //

    SetEvent(terminateTestingThreadsHandle);

    WaitForMultipleObjects(NUM_THREADS, testHandles, TRUE, INFINITE);

    //
    // Set event for working threads
    //

    SetEvent(terminateWorkingThreadsHandle);

    WaitForMultipleObjects(NUM_THREADS, zeroPageThreadHandles, TRUE, INFINITE);

    #ifdef TESTING_ZERO
    WaitForMultipleObjects(NUM_THREADS, freePageTestThreadHandles, TRUE, INFINITE);
    
    #endif

    #ifdef TESTING_MODIFIED
    WaitForMultipleObjects(NUM_THREADS, modifiedPageWriterThreadHandles, TRUE, INFINITE);
    #endif

    #ifdef CHECK_PFNS
    WaitForSingleObject(pageTestThread, INFINITE);
    #endif


    WaitForMultipleObjects(NUM_THREADS, trimThreadHandles, TRUE, INFINITE);

    #endif

    deleteVAD(leafVABlock, 0);

    /********** Verify no PFNs remain active *********/

    #ifdef CHECK_PFNS
    PPFNdata currPFN;

    for (int j = 0; j < numPagesReturned; j++) {

        currPFN = PFNarray + aPFNs[j];

        acquireJLock(&currPFN->lockBits);
        if (currPFN->statusBits > MODIFIED) {

            DebugBreak();

        }

        releaseJLock(&currPFN->lockBits);


    }
    #endif

    ULONG_PTR pageCount;
    pageCount = 0;

    for (int i = 0; i < ACTIVE; i++) {

        pageCount += listHeads[i].count;

    }

    PRINT_ALWAYS("total page count %llu\n", pageCount);
    ASSERT(numPagesReturned == pageCount);  // plus one for the pagetrade???

}


VOID
initHandles()
{
    wakeTrimHandle = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (wakeTrimHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
        exit(-1);
    }

    wakeModifiedWriterHandle = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (wakeModifiedWriterHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
        exit(-1);
    }
}


BOOL
closeHandles()
{
    BOOL bRes;
    bRes = CloseHandle(wakeTrimHandle);

    if (bRes != TRUE) {

        PRINT_ERROR("Unable to close handle\n");

    }
    bRes = CloseHandle(wakeModifiedWriterHandle);

    if (bRes != TRUE) {

        PRINT_ERROR("Unable to close handle\n");
        
    }

    return (BOOLEAN)bRes;
}


ULONG_PTR
initializeVirtualMemory()
{

    // initialize zero/free/standby lists 
    initListHeads(listHeads);

    #ifndef PAGEFILE_OFF

        #ifndef PAGEFILE_PFN_CHECK

        // memset the pageFile bit array
            memset(&pageFileBitArray, 0, PAGEFILE_PAGES/(8*sizeof(ULONG_PTR) ) );

        #else
            pageFileDebugArray = VirtualAlloc(NULL, sizeof(pageFileDebug)*PAGEFILE_PAGES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
            memset(pageFileDebugArray, 0, PAGEFILE_PAGES * sizeof(pageFileDebug ));
        #endif

    #else

        memset(&pageFileBitArray, 1, PAGEFILE_PAGES/(8*sizeof(ULONG_PTR) ) );
    
    #endif

    //
    // Initialize pagefile critical section
    //
    InitializeCriticalSection(&pageFileLock);

    // allocate an array of PFNs that is returned by AllocateUserPhysPages

    #ifndef CHECK_PFNS

    //
    // if CHECK_PFNS flag is not set, declare aPFNs locally (since it can be freed locally as well)
    //

    PULONG_PTR aPFNs;  
    #endif

    aPFNs = VirtualAlloc(NULL, NUM_PAGES*(sizeof(ULONG_PTR)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if (aPFNs == NULL) {

        PRINT_ERROR("failed to allocate PFNarray\n");

    }

    numPagesReturned = allocatePhysPages(NUM_PAGES, aPFNs);

    // to achieve a greater VM address range than PM would otherwise allow
    virtualMemPages = numPagesReturned * VM_MULTIPLIER;


    // initialize zeroVAList, consisting of AWE addresses for zeroing pages
    initVAList(&zeroVAListHead, NUM_THREADS + 3);

    initVAList(&writeVAListHead, NUM_THREADS + 3);

    initVAList(&readPFVAListHead, NUM_THREADS + 3);

    initVAList(&pageTradeVAListHead, 2*NUM_THREADS + 3);

    initEventList(&readInProgEventListHead, NUM_THREADS + 3);


    /******************* initialize data structures ****************/

    // create virtual address block
    initVABlock(virtualMemPages);

    initVADList();

    //
    // Initialize VAD bit array to denote availability across entire
    // VM range
    //

    VADBitArray = VirtualAlloc(NULL, virtualMemPages/(sizeof(ULONG_PTR) * 8) + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    // createVAD(NULL, virtualMemPages, READ_WRITE, TRUE);         // MEM_COMMIT vad
    createVAD(NULL, virtualMemPages, READ_WRITE, FALSE);        // MEM_RESERVE vad


    // create local PFN metadata array
    initPFNarray(aPFNs, numPagesReturned);

    #ifndef CHECK_PFNS
    // Free PFN array (no longer used)
    VirtualFree(aPFNs, 0, MEM_RELEASE);
    #endif

    // create local PTE array to map VAs to pages
    initPTEarray(virtualMemPages);

    // create PageFile section of memory
    initPageFile(PAGEFILE_SIZE);

    // initialize availablePagesLow & wakeModifiedWriter handles
    initHandles();

    initPTELocks(virtualMemPages);

    return numPagesReturned;

}


VOID
freeVirtualMemory()
{

    /******************* free allocated memory ***************/
    VirtualFree(leafVABlock, 0, MEM_RELEASE);
    VirtualFree(PFNarray, 0, MEM_RELEASE);
    VirtualFree(PTEarray, 0, MEM_RELEASE);
    VirtualFree(pageFileVABlock, 0, MEM_RELEASE);

    freeEventList(&readInProgEventListHead);

    freeVAList(&zeroVAListHead);
    freeVAList(&writeVAListHead);
    freeVAList(&readPFVAListHead);
    freeVAList(&pageTradeVAListHead);

    // close availablePagesLow & wakeModifiedWriter handles
    closeHandles();

    freePTELocks(PTELockArray, virtualMemPages);

}


VOID 
main(int argc, char** argv) 
{

    /*********** switch to toggle verbosity (print statements) *************/
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        debugMode = TRUE;
    }
    
    // ULONG_PTR numPagesReturned;

    numPagesReturned = initializeVirtualMemory();


    /******************** call test routine ******************/
    testRoutine();


    /******************* free allocated memory ***************/
    freeVirtualMemory();

    PRINT_ALWAYS("----------------\nprogram complete\n----------------");


}


