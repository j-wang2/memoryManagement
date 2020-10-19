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


/******************************************************
 *********************** GLOBALS **********************
 *****************************************************/
void* leafVABlock;                      // starting address of virtual memory block
void* leafVABlockEnd;                   // ending address of virtual memory block

PPFNdata PFNarray;                      // starting address of PFN metadata array
PPTE PTEarray;                          // starting address of page table

void* pageTradeDestVA;                  // specific VA used for page trading destination
void* pageTradeSourceVA;                // specific VA used for page trading source

LONG totalCommittedPages;               // count of committed pages (initialized to zero)
ULONG_PTR totalMemoryPageLimit = NUM_PAGES + (PAGEFILE_SIZE >> PAGE_SHIFT);    // limit of committed pages (memory block + pagefile space)

void* pageFileVABlock;                  // starting address of pagefile "disk" (memory)
ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];

// Execute-Write-Read (bit ordering)
ULONG_PTR permissionMasks[] = { 0, readMask, (readMask | writeMask), (readMask | executeMask), (readMask | writeMask | executeMask) };

/************ List declarations *****************/
listData listHeads[ACTIVE];             // page listHeads array

listData zeroVAListHead;                // listHead of zeroVAs used for zeroing PFNs (via AWE mapping)
listData writeVAListHead;               // listHead of writeVAs used for writing to page file
listData readPFVAListHead;              // listHead of pagefile read VAs used for reading from pagefile
listData pageTradeVAListHead;

listData VADListHead;                   // list of VADs

listData readInProgEventListHead;

CRITICAL_SECTION PTELock;

PCRITICAL_SECTION PTELockArray;

HANDLE physicalPageHandle;              // for multi-mapped pages (to support multithreading)

BOOLEAN debugMode;                      // toggled by -v flag on cmd line

HANDLE availablePagesLowHandle;

HANDLE wakeModifiedWriterHandle;

ULONG_PTR numPagesReturned;




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
        // Atomically increment committed page count
        //

        InterlockedIncrement(&totalCommittedPages);

        //
        // note: no lock needed functionally (simply to satisfy assert in enqueuePage)
        //

        acquireJLock(&newPFN->lockBits);

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

    InitializeCriticalSection(&PTELock);


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

    //TODO - reserves extra spot for page trade?
    InterlockedIncrement(&totalCommittedPages);

}


ULONG_PTR
allocatePhysPages(ULONG_PTR numPages, PULONG_PTR aPFNs) {

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


    // acquires & releases both list and page locks
    PPFNdata PFNtoZero;
    PFNtoZero = dequeueLockedPage(&freeListHead, TRUE);


    if (PFNtoZero == NULL) {
        PRINT("free list empty - could not write out\n");
        return FALSE;
    }

    // set overloaded "writeinprogress" bit (to signify page is currently being zeroed)
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

        // enqueue to zeroList (updates status bits in PFN metadata)
        enqueuePage(&zeroListHead, PFNtoZero);

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

    // enqueue to freeList (updates status bits in PFN metadata)
    enqueuePage(&freeListHead, PFNtoFree);

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

    ASSERT(PFNtoFree->pageFileOffset != INVALID_PAGEFILE_INDEX);
        
    clearPFBitIndex(PFNtoFree->pageFileOffset);

    PFNtoFree->pageFileOffset = INVALID_PAGEFILE_INDEX;

    // enqueue Page to free list
    enqueuePage(&freeListHead, PFNtoFree);

    PRINT("[releaseAwaitingFreePFN] VA decommitted during PF write\n");

}


BOOLEAN
modifiedPageWriter()
{

    BOOLEAN wakeModifiedWriter;
    wakeModifiedWriter = FALSE;

    PRINT("[modifiedPageWriter] modifiedListCount == %llu\n", modifiedListHead.count);
    PPFNdata PFNtoWrite;

    //
    // Return page with lock bits set
    //

    PFNtoWrite = dequeueLockedPage(&modifiedListHead, TRUE);


    if (PFNtoWrite == NULL) {
        PRINT("[modifiedPageWriter] modified list empty - could not write out\n");
        return FALSE;
    }

    // Lock has previuosly been acquired - set write in progress bit to 1
    ASSERT(PFNtoWrite->writeInProgressBit == 0);
    PFNtoWrite->writeInProgressBit = 1;

    // revert PFN status to modified so that it can once again be faulted
    PFNtoWrite->statusBits = MODIFIED;

    ASSERT(PFNtoWrite->pageFileOffset == INVALID_PAGEFILE_INDEX);
    
    releaseJLock(&PFNtoWrite->lockBits);


    // write page out - can no longer be accessed since write in progress is one
    BOOLEAN bResult;
    bResult = writePageToFileSystem(PFNtoWrite);


    // Acquire lock to view PFN
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


    // check if PFN has been decommitted
    if (PFNtoWrite->statusBits == AWAITING_FREE) {
        
        releaseAwaitingFreePFN(PFNtoWrite);

        releaseJLock(&PFNtoWrite->lockBits);

        return TRUE;
    }


    // if write failed or the PFN has since been modified
    if (bResult != TRUE || PFNtoWrite->remodifiedBit == 1) {
        

        ASSERT(PFNtoWrite->pageFileOffset != INVALID_PAGEFILE_INDEX);
            

        // either way (write failed/PFN modified), clear PF index if it exists
        clearPFBitIndex(PFNtoWrite->pageFileOffset);


        PFNtoWrite->pageFileOffset = INVALID_PAGEFILE_INDEX;


        // If the page has not since been faulted in, re-enqueue to modified list
        // (since page has either failed write or been re-modified, i.e. write->write fault->trim)
        if (PFNtoWrite->statusBits != ACTIVE) {

            wakeModifiedWriter = enqueuePage(&modifiedListHead, PFNtoWrite);

        }

        releaseJLock(&PFNtoWrite->lockBits);

        if (bResult != TRUE) {
            PRINT_ERROR("[modifiedPageWriter] error writing out page\n");
        }  else {
            PRINT("[modifiedPageWriter] Page has since been modified, clearing PF space\n");
        }
        
        return TRUE;
    }


    // currPTE must be "good" since PFN has not been decommitted (we hold lock)
    // can be either valid or transition (dangling since we hold write in progress bit)
    PPTE currPTE;
    currPTE = PTEarray + PFNtoWrite->PTEindex;    


    // if PFN has not since been faulted in to active, enqueue to standby list
    // if it has been write faulted in, check the dirty bit
    //  - if it is set, clear the PF bit index
    //  - otherwise, continue
    if (PFNtoWrite->statusBits != ACTIVE) {

        ASSERT(currPTE->u1.hPTE.validBit != 1 && currPTE->u1.tPTE.transitionBit == 1);

        // enqueue page to standby (since has not been redirtied)
        enqueuePage(&standbyListHead, PFNtoWrite);

        PRINT(" - Moved page from modified -> standby (wrote out to PageFile successfully)\n");


    } 
    else {

        ASSERT(currPTE->u1.hPTE.validBit == 1);     

        if (currPTE->u1.hPTE.dirtyBit == 1) {

            // free PF location from PFN
            clearPFBitIndex(PFNtoWrite->pageFileOffset);

            // clear pagefile pointer out of PFN
            PFNtoWrite->pageFileOffset = INVALID_PAGEFILE_INDEX;
            
            PRINT(" - Page has since been write faulted (discarding PF space)\n");

        }
    }

    releaseJLock(&PFNtoWrite->lockBits);

    if (wakeModifiedWriter == TRUE) {
        
        BOOL bRes;
        bRes = SetEvent(wakeModifiedWriterHandle);

        if (bRes != TRUE) {
            PRINT_ERROR("[trimPTE] failed to set event\n");
        }
    }


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
    // handleArray[1] = modifiedListHead.newPagesEvent;
    handleArray[1] = wakeModifiedWriterHandle;


    // write out modified pages to pagefile until modified page list empty
    while (TRUE) {
        BOOLEAN bres;
        bres = modifiedPageWriter();
        if (bres == FALSE) {

            DWORD retVal = WaitForMultipleObjects(2, handleArray, FALSE, INFINITE);
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
    PRINT_ALWAYS("Fault and access test\n");

    /************** TESTING *****************/
    void* testVA;
    testVA = leafVABlock;


    /************* FAULTING in and WRITING/ACCESSING testVAs *****************/

    ULONG_PTR testNum = numPagesReturned * VM_MULTIPLIER;      //TEMPORARY

    // PRINT_ALWAYS("Committing and then faulting in %llu pages\n", testNum);


    commitVA(testVA, READ_ONLY, testNum << PAGE_SHIFT);     // commits with READ_ONLY permissions
    protectVA(testVA, READ_WRITE, testNum << PAGE_SHIFT);   // converts to read write


    for (int i = 0; i < testNum; i++) {

        faultStatus testStatus;

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

    PRINT("Trimming %llu pages, modified writing half of them\n", testNum);

    // reset testVa
    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {

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

    for (int i = 0; i < testNum; i++) {


        #ifdef CHECK_PAGEFILE
        // "leaks" pages in order to force standby->pf repurposing
        for (int j = 0; j < 3; j++) {
            getPage(FALSE);
        }
        #endif

        faultStatus testStatus = accessVA(testVA, READ_WRITE);        // to TEST VAs
        // faultStatus testStatus = pageFault(testVA, READ_ONLY);      // to FAULT VAs

        PRINT("tested (VA = %llu), return status = %u\n", (ULONG_PTR) testVA, testStatus);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }


    /***************** DECOMMITTING AND CHECKING VAs **************/

    testVA = leafVABlock;
    decommitVA(testVA, testNum << PAGE_SHIFT);

    return TRUE;

}


DWORD WINAPI
faultAndAccessTestThread(HANDLE terminationHandle) {

    #ifdef CONTINUOUS_FAULT_TEST

    while (TRUE)
    {
        // write out modified pages to pagefile until modified page list empty
        BOOLEAN bres;
        bres = faultAndAccessTest();


        DWORD waitRes;
        waitRes = WaitForSingleObject(terminationHandle, 100);

        if (waitRes == WAIT_TIMEOUT) {
            continue;
        }
        else if (waitRes == WAIT_OBJECT_0) {
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
    currPTE = PTEarray;

    PPTE endPTE;
    endPTE = PTEarray + (((ULONG_PTR)leafVABlockEnd - (ULONG_PTR)leafVABlock) / PAGE_SIZE);

    ULONG_PTR numTrimmed;
    numTrimmed = 0;

    for (currPTE; currPTE < endPTE; currPTE++ ) {

        //
        // If currPTE is valid/active
        //

        acquirePTELock(currPTE);

        if (currPTE->u1.hPTE.validBit == 1) {

            // todo- add assert for pfn??

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

    }
    return numTrimmed;


}


DWORD WINAPI
trimValidPTEThread(HANDLE terminationHandle) {

    // write out modified pages to pagefile until modified page list empty
    ULONG_PTR numTrimmed;
    numTrimmed = 0;

    
    // create local handle array
    HANDLE handleArray[2];
    handleArray[0] = terminationHandle;
    handleArray[1] = availablePagesLowHandle;

    while (TRUE) {

        DWORD retVal;
        retVal = WaitForMultipleObjects(2, handleArray, FALSE, INFINITE);

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
    pagesCreatedHandle = CreateEvent(NULL, FALSE, FALSE, NULL);

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

    HANDLE terminateThreadsHandle;
    terminateThreadsHandle =  CreateEvent(NULL, TRUE, FALSE, NULL);

    if (terminateThreadsHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
    }

    PRINT_ALWAYS(" - %d zeroPage threads\n", NUM_THREADS);

    HANDLE zeroPageThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        zeroPageThreadHandles[i] = CreateThread(NULL, 0, zeroPageThread, terminateThreadsHandle, 0, NULL);
            
        if (zeroPageThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create zeroPage handle\n");
        }
  
    }  


    #ifdef TESTING_ZERO
    /************ for testing of zeropagethread **************/

    PRINT_ALWAYS(" - %d freePage threads\n", NUM_THREADS);

    HANDLE freePageTestThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        freePageTestThreadHandles[i] = CreateThread(NULL, 0, freePageTestThread, terminateThreadsHandle, 0, NULL);

        if (freePageTestThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create freePage handle\n");
        }
    }
    #endif

    #ifdef TESTING_MODIFIED

    PRINT_ALWAYS(" - %d modifiedWriter threads\n", NUM_THREADS);

    HANDLE modifiedPageWriterThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        modifiedPageWriterThreadHandles[i] = CreateThread(NULL, 0, modifiedPageThread, terminateThreadsHandle, 0, NULL);
            
        if (modifiedPageWriterThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create modifiedPageWriting handle\n");
        }
  
    }
    #endif

    PRINT_ALWAYS(" - %d PTE trimming threads\n", NUM_THREADS);

    HANDLE trimThreadHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        trimThreadHandles[i] = CreateThread(NULL, 0, trimValidPTEThread, terminateThreadsHandle, 0, NULL);
            
        if (trimThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create modifiedPageWriting handle\n");
        }
  
    }

    /**************************************/


    PRINT_ALWAYS(" - %d access/fault test threads\n", NUM_THREADS);

    HANDLE testHandles[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {

        testHandles[i] = CreateThread(NULL, 0, faultAndAccessTestThread, terminateThreadsHandle, 0, NULL);
            
        if (testHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create modifiedPageWriting handle\n");
        }
  
    }

    char quitChar;
    quitChar = 'a';
    while (quitChar != 'q') {
        quitChar = getchar();
    }
    PRINT_ALWAYS("Ending program\n");

    

    SetEvent(terminateThreadsHandle);

    WaitForMultipleObjects(NUM_THREADS, zeroPageThreadHandles, TRUE, INFINITE);

    #ifdef TESTING_ZERO
    WaitForMultipleObjects(NUM_THREADS, freePageTestThreadHandles, TRUE, INFINITE);
    
    #endif

    #ifdef TESTING_MODIFIED
    WaitForMultipleObjects(NUM_THREADS, modifiedPageWriterThreadHandles, TRUE, INFINITE);
    #endif

    WaitForMultipleObjects(NUM_THREADS, trimThreadHandles, TRUE, INFINITE);

    WaitForMultipleObjects(NUM_THREADS, testHandles, TRUE, INFINITE);

    #endif


    ULONG_PTR pageCount;
    pageCount= 0;
    for (int i = 0; i < ACTIVE; i++) {

        pageCount += listHeads[i].count;

    }
    PRINT_ALWAYS("total page count %llu\n", pageCount);
    ASSERT(numPagesReturned == pageCount);

}


VOID
initHandles()
{
    availablePagesLowHandle = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (availablePagesLowHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
        exit(-1);
    }

    wakeModifiedWriterHandle = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (wakeModifiedWriterHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
        exit(-1);
    }
}

BOOLEAN
closeHandles()
{
    BOOL bRes = CloseHandle(availablePagesLowHandle);       // TODO - check ret val and abstract

    if (bRes != TRUE) {

        PRINT_ERROR("Unable to close handle\n");

    }
    bRes = CloseHandle(wakeModifiedWriterHandle);       // TODO - check ret val and abstract

    if (bRes != TRUE) {

        PRINT_ERROR("Unable to close handle\n");
        
    }

    return bRes;
}


ULONG_PTR
initializeVirtualMemory()
{

    // initialize zero/free/standby lists 
    initListHeads(listHeads);

    // memset the pageFile bit array
    memset(&pageFileBitArray, 0, PAGEFILE_PAGES/(8*sizeof(ULONG_PTR) ) );

    // allocate an array of PFNs that is returned by AllocateUserPhysPages
    ULONG_PTR *aPFNs;
    aPFNs = VirtualAlloc(NULL, NUM_PAGES*(sizeof(ULONG_PTR)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if (aPFNs == NULL) {
        PRINT_ERROR("failed to allocate PFNarray\n");
    }

    numPagesReturned = allocatePhysPages(NUM_PAGES, aPFNs);

    // to achieve a greater VM address range than PM would otherwise allow
    ULONG_PTR virtualMemPages;
    virtualMemPages = numPagesReturned * VM_MULTIPLIER;


    // initialize zeroVAList, consisting of AWE addresses for zeroing pages
    initVAList(&zeroVAListHead, NUM_THREADS + 3);

    initVAList(&writeVAListHead, NUM_THREADS + 3);

    initVAList(&readPFVAListHead, NUM_THREADS + 3);

    initVAList(&pageTradeVAList, 2*NUM_THREADS + 3);

    initEventList(&readInProgEventListHead, NUM_THREADS + 3);


    /******************* initialize data structures ****************/

    // create virtual address block
    initVABlock(virtualMemPages);

    // create local PFN metadata array
    initPFNarray(aPFNs, numPagesReturned);

    // Free PFN array (no longer used)
    VirtualFree(aPFNs, 0, MEM_RELEASE);

    // create local PTE array to map VAs to pages
    initPTEarray(virtualMemPages);

    // create PageFile section of memory
    initPageFile(PAGEFILE_SIZE);

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

    VirtualFree(pageTradeDestVA, 0, MEM_RELEASE);
    VirtualFree(pageTradeSourceVA, 0, MEM_RELEASE);

    freeEventList(&readInProgEventListHead);

    freeVAList(&zeroVAListHead);
    freeVAList(&writeVAListHead);
    freeVAList(&readPFVAListHead);


    closeHandles();

    VirtualFree(PTELockArray, 0, MEM_RELEASE);

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
    testRoutine(numPagesReturned);


    /******************* free allocated memory ***************/
    freeVirtualMemory();

    PRINT_ALWAYS("----------------\nprogram complete\n----------------");


}


