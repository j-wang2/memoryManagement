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


/******************************************************
 *********************** GLOBALS **********************
 *****************************************************/
void* leafVABlock;                  // starting address of virtual memory block
void* leafVABlockEnd;               // ending address of virtual memory block

PPFNdata PFNarray;                  // starting address of PFN metadata array
PPTE PTEarray;                      // starting address of page table

void* pageTradeDestVA;                  // specific VA used for page trading destination
void* pageTradeSourceVA;                // specific VA used for page trading source

ULONG_PTR totalCommittedPages;      // count of committed pages (initialized to zero)
ULONG_PTR totalMemoryPageLimit = NUM_PAGES + (PAGEFILE_SIZE >> PAGE_SHIFT);    // limit of committed pages (memory block + pagefile space)

void* pageFileVABlock;              // starting address of pagefile "disk" (memory)
ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];

// Execute-Write-Read (bit ordering)
ULONG_PTR permissionMasks[] = { 0, readMask, (readMask | writeMask), (readMask | executeMask), (readMask | writeMask | executeMask) };

/************ List declarations *****************/
listData listHeads[ACTIVE];         // page listHeads array

listData zeroVAListHead;            // listHead of zeroVAs used for zeroing PFNs (via AWE mapping)
listData writeVAListHead;           // listHead of writeVAs used for writing to page file
listData readPFVAListHead;


listData VADListHead;               // list of VADs

CRITICAL_SECTION PTELock;

HANDLE physicalPageHandle;          // for shared pages


BOOLEAN debugMode;



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
    BOOLEAN bResult;
    bResult = (BOOLEAN) LoggedSetLockPagesPrivilege( GetCurrentProcess(), TRUE );
    return bResult;
}


VOID 
initVABlock(ULONG_PTR numPages)
{


    #ifdef MULTIPLE_MAPPINGS

    MEM_EXTENDED_PARAMETER extendedParameters;
    extendedParameters.Type = MemExtendedParameterUserPhysicalHandle;
    extendedParameters.Handle = physicalPageHandle;


    leafVABlock = VirtualAlloc2(NULL, NULL, numPages << PAGE_SHIFT, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE, &extendedParameters, 1);      // equiv to numVAs*PAGE_SIZE

    #else
    // creates a VAD node that we can define (i.e. is not pagefaulted by underlying kernel mm)
    leafVABlock = VirtualAlloc(NULL, numPages*pageSize, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    #endif

    if (leafVABlock == NULL) {
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

        // increment committed page count
        totalCommittedPages++; 

        // add page to free list
        enqueuePage(&freeListHead, newPFN);     // TODO - although no lock is needed functionally, adjust if an assert is added into enqueue    

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

    //TODO - reserves extra spot
    totalCommittedPages++;

}


ULONG_PTR
allocatePhysPages(ULONG_PTR numPages, PULONG_PTR aPFNs) {

    // secure privilege for the code
    BOOLEAN privResult;
    privResult = getPrivilege();
    if (privResult != TRUE) {
        PRINT_ERROR("could not get privilege successfully \n");
        exit(-1);
    }    


    ULONG_PTR numPagesReturned;
    numPagesReturned = numPages;



    #ifdef MULTIPLE_MAPPINGS

    MEM_EXTENDED_PARAMETER extendedParameters;

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
    bResult = AllocateUserPhysicalPages(physicalPageHandle, &numPagesReturned, aPFNs);

    if (bResult != TRUE) {
        PRINT_ERROR("could not allocate pages successfully \n");
        exit(-1);
    }

    if (numPagesReturned != numPages) {
        PRINT("allocated only %llu pages out of %u pages requested\n", numPagesReturned, NUM_PAGES);
    }

    return numPagesReturned;


}


BOOLEAN
zeroPage(ULONG_PTR PFN)
{

    PVANode zeroVANode;
    zeroVANode = dequeueLockedVA(&zeroVAListHead);

    if (zeroVANode == NULL) {
        PRINT("[zeroPage] TODO: waiting for release\n");
        DebugBreak();
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
    // PRINT("zeroListCount == %llu\n", zeroListHead.count);

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


    //  PRINT(" - Moved page from zero -> free \n");

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

        // return TRUE;
    }
}


BOOLEAN
modifiedPageWriter()
{

    PRINT("[modifiedPageWriter] modifiedListCount == %llu\n", modifiedListHead.count);
    PPFNdata PFNtoWrite;


    // return page that is locked
    PFNtoWrite = dequeueLockedPage(&modifiedListHead, TRUE);


    if (PFNtoWrite == NULL) {
        PRINT("[modifiedPageWriter] modified list empty - could not write out\n");
        return FALSE;
    }


    // lock has previuosly been acquired - set write in progress bit to 1
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


    // check if PFN has been decommitted
    if (PFNtoWrite->statusBits == AWAITING_FREE) {
        
        ASSERT(FALSE);          // TEMP TODO : check this (someone has decommitted in meantime). delete after fixed
        ASSERT(PFNtoWrite->pageFileOffset != INVALID_PAGEFILE_INDEX);
            
        clearPFBitIndex(PFNtoWrite->pageFileOffset);

        PFNtoWrite->pageFileOffset = INVALID_PAGEFILE_INDEX;

        // enqueue Page to free list
        enqueuePage(&freeListHead, PFNtoWrite);

        releaseJLock(&PFNtoWrite->lockBits);

        PRINT("[modifiedPageWriter] VA decommitted during PF write\n");
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

            enqueuePage(&modifiedListHead, PFNtoWrite);

        }
        
        releaseJLock(&PFNtoWrite->lockBits);

        if (bResult != TRUE) {
            PRINT_ERROR("[modifiedPageWriter] error writing out page\n");
        }  else {
            PRINT("[modifiedPageWriter] Page has since been modified, clearing PF space\n");
        }
        
        return TRUE;
    }


    // currPTE must be "good" since PFN has not been decommited (we hold lock)
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

    // clear write in progress bit (since page has both been written and re-enqueued)

    PFNtoWrite->writeInProgressBit = 0;

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
    handleArray[1] = modifiedListHead.newPagesEvent;

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
    pagesCreatedHandle =  CreateEvent(NULL, FALSE, FALSE, NULL);

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

    // initialize lock field
    InitializeCriticalSection(&(VAListHead->lock));

    // initialize head
    initLinkHead(&(VAListHead->head));

    VAListHead->count = 0;

    HANDLE newVAsHandle;
    newVAsHandle =  CreateEvent(NULL, FALSE, FALSE, NULL);

    if (newVAsHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("[initVAList] failed to create event handle\n");
        exit(-1);
    }

    VAListHead->newPagesEvent = newVAsHandle;


    // alloc for VAs
    void* baseVA;

    #ifdef MULTIPLE_MAPPINGS
    MEM_EXTENDED_PARAMETER ExtendedParameters;
    ExtendedParameters.Type = MemExtendedParameterUserPhysicalHandle;
    ExtendedParameters.Handle = physicalPageHandle;

    baseVA = VirtualAlloc2(NULL, NULL, numVAs << PAGE_SHIFT, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE, &ExtendedParameters, 1);      // equiv to numVAs*PAGE_SIZE

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
initVADList()
{
    InitializeCriticalSection(&(VADListHead.lock));

    initLinkHead(&(VADListHead.head));

    VADListHead.count = 0;

    // do i need to do an event here?
}


VOID
testRoutine(ULONG_PTR numPagesReturned)
{
    PRINT_ALWAYS("[testRoutine]\n");
    
    /************** TESTING *****************/
    void* testVA;
    testVA = leafVABlock;


    PRINT_ALWAYS("--------------------------------\n");


    /************* FAULTING in and WRITING/ACCESSING testVAs *****************/

    ULONG_PTR testNum = numPagesReturned * VM_MULTIPLIER;      //TEMPORARY

    PRINT_ALWAYS("Committing and then faulting in %llu pages\n", testNum);


    commitVA(testVA, READ_ONLY, testNum << PAGE_SHIFT);    // commits with READ_ONLY permissions


    for (int i = 0; i < testNum; i++) {

        faultStatus testStatus;

        // commitVA(testVA, READ_ONLY, PAGE_SIZE);    // commits with READ_ONLY permissions
        protectVA(testVA, READ_WRITE, PAGE_SIZE);   // converts to read write

        testStatus = writeVA(testVA, testVA);
            
        // trimVA(testVA);

        /************ TRADING pages ***********/

        #ifdef TRADE_PAGES
        PRINT("Attempting to trade VA\n");
        tradeVA(testVA);

        #endif


        PRINT("tested (VA = %llu), return status = %u\n", (ULONG_PTR) testVA, testStatus);

        // iterate to next VA
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);

    }


    PRINT_ALWAYS("--------------------------------\n");


    /************ TRIMMING tested VAs (active -> standby/modified) **************/

    PRINT_ALWAYS("Trimming %llu pages, modified writing half of them\n", testNum);

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



    #ifdef MULTITHREADING

    PRINT_ALWAYS("--------------------------------\n");

    /************* Creating handles/threads *************/
    PRINT_ALWAYS("Creating threads (zeropage, freepage)\n");


    HANDLE terminateThreadsHandle;
    terminateThreadsHandle =  CreateEvent(NULL, TRUE, FALSE, NULL);

    if (terminateThreadsHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
    }


    HANDLE zeroPageThreadHandles[NUM_ZERO_THREADS];
    for (int i = 0; i < NUM_ZERO_THREADS; i++) {

        zeroPageThreadHandles[i] = CreateThread(NULL, 0, zeroPageThread, terminateThreadsHandle, 0, NULL);
            
        if (zeroPageThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create zeroPage handle\n");
        }
  
    }


    #ifdef TESTING_ZERO
    /************ for testing of zeropagethread **************/

    HANDLE freePageTestThreadHandles[NUM_ZERO_THREADS];
    for (int i = 0; i < NUM_ZERO_THREADS; i++) {
        freePageTestThreadHandles[i] = CreateThread(NULL, 0, freePageTestThread, terminateThreadsHandle, 0, NULL);

        if (freePageTestThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create freePage handle\n");
        }
    }
    #endif

    #ifdef TESTING_MODIFIED
    HANDLE modifiedPageWriterThreadHandles[NUM_ZERO_THREADS];
    for (int i = 0; i < NUM_ZERO_THREADS; i++) {

        modifiedPageWriterThreadHandles[i] = CreateThread(NULL, 0, modifiedPageThread, terminateThreadsHandle, 0, NULL);
            
        if (modifiedPageWriterThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create modifiedPageWriting handle\n");
        }
  
    }
    #endif


    #endif

    PRINT_ALWAYS("--------------------------------\n");


    /****************** FAULTING back in trimmed pages ******************/

    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {


        #ifdef CHECK_PAGEFILE
        // "leaks" pages in order to force standby->pf repurposing
        for (int j = 0; j < 3; j++) {
            getPage();
        }
        #endif

        faultStatus testStatus = pageFault(testVA, READ_WRITE);        // to TEST VAs
        // faultStatus testStatus = pageFault(testVA, READ_ONLY);      // to FAULT VAs

        PRINT("tested (VA = %llu), return status = %u\n", (ULONG_PTR) testVA, testStatus);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }


    /***************** DECOMMITTING AND CHECKING VAs **************/

    testVA = leafVABlock;
    decommitVA(testVA, testNum << PAGE_SHIFT);


    #ifdef MULTITHREADING

    SetEvent(terminateThreadsHandle);

    WaitForMultipleObjects(NUM_ZERO_THREADS, zeroPageThreadHandles, TRUE, INFINITE);

    #ifdef TESTING_ZERO
    WaitForMultipleObjects(NUM_ZERO_THREADS, freePageTestThreadHandles, TRUE, INFINITE);
    
    #endif

    #ifdef TESTING_MODIFIED
    WaitForMultipleObjects(NUM_ZERO_THREADS, modifiedPageWriterThreadHandles, TRUE, INFINITE);
    #endif

    #endif

    ULONG_PTR pageCount;
    pageCount= 0;
    for (int i = 0; i < ACTIVE; i++) {

        pageCount += listHeads[i].count;

    }
    PRINT_ALWAYS("total page count %llu\n", pageCount);
    ASSERT(numPagesReturned == pageCount);

}


ULONG_PTR
initializeVirtualMemory()
{
    // initialize zero/free/standby lists 
    initListHeads(listHeads);

    // reserve AWE addresses for page trading
    pageTradeDestVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    pageTradeSourceVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    // memset the pageFile bit array
    memset(&pageFileBitArray, 0, PAGEFILE_PAGES/(8*sizeof(ULONG_PTR) ) );

    // allocate an array of PFNs that is returned by AllocateUserPhysPages
    ULONG_PTR *aPFNs;
    aPFNs = VirtualAlloc(NULL, NUM_PAGES*(sizeof(ULONG_PTR)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if (aPFNs == NULL) {
        PRINT_ERROR("failed to allocate PFNarray\n");
    }

    ULONG_PTR numPagesReturned;
    numPagesReturned = allocatePhysPages(NUM_PAGES, aPFNs);

    // to achieve a greater VM address range than PM would otherwise allow
    ULONG_PTR virtualMemPages;
    virtualMemPages = numPagesReturned * VM_MULTIPLIER;


    // initialize zeroVAList, consisting of AWE addresses for zeroing pages
    initVAList(&zeroVAListHead, NUM_ZERO_THREADS + 3);

    initVAList(&writeVAListHead, NUM_ZERO_THREADS + 3);

    initVAList(&readPFVAListHead, NUM_ZERO_THREADS + 3);


    /******************* initialize data structures ****************/

    // create virtual address block
    initVABlock(virtualMemPages);

    // create local PFN metadata array
    initPFNarray(aPFNs, numPagesReturned);

    // create local PTE array to map VAs to pages
    initPTEarray(virtualMemPages);

    // create PageFile section of memory
    initPageFile(PAGEFILE_SIZE);

    return numPagesReturned;
}


VOID 
main(int argc, char** argv) 
{

    /*********** switch to toggle verbosity (print statements) *************/
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        debugMode = TRUE;
    }
    
    ULONG_PTR numPagesReturned;

    numPagesReturned = initializeVirtualMemory();


    /******************** call test routine ******************/
    testRoutine(numPagesReturned);


    PRINT_ALWAYS("----------------\nprogram complete\n----------------");

    //  free memory allocated
    VirtualFree(leafVABlock, 0, MEM_RELEASE);
    VirtualFree(PFNarray, 0, MEM_RELEASE);
    VirtualFree(PTEarray, 0, MEM_RELEASE);
    VirtualFree(pageFileVABlock, 0, MEM_RELEASE);

}
