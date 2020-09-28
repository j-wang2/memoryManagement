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


/******* GLOBALS *****/
void* leafVABlock;                  // starting address of memory block
void* leafVABlockEnd;               // ending address of memory block

PPFNdata PFNarray;                  // starting address of PFN metadata array
PPTE PTEarray;                      // starting address of page table


void* pageTradeDestVA;                  // specific VA used for page trading destination
void* pageTradeSourceVA;                // specific VA used for page trading source

ULONG_PTR totalCommittedPages;      // count of committed pages (initialized to zero)
ULONG_PTR totalMemoryPageLimit = NUM_PAGES + PAGEFILE_SIZE / PAGE_SIZE;    // limit of committed pages (memory block + pagefile space)

void* pageFileVABlock;              // starting address of pagefile "disk" (memory)
void* pageFileFormatVA;             // specific VA used for copying in page contents from pagefile
ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];

// Execute-Write-Read (bit ordering)
ULONG_PTR permissionMasks[] = { 0, readMask, (readMask | writeMask), (readMask | executeMask), (readMask | writeMask | executeMask) };

listData listHeads[ACTIVE];         // intialization of listHeads array

listData zeroVAListHead;            // list of zeroVAs used for zeroing PFNs (via AWE mapping)
listData writeVAListHead;           // list of writeVAs used for writing to page file

listData VADListHead;               // list of VADs

BOOLEAN debugMode;

#define TESTING_ZERO
#define TESTING_MODIFIED


PPTE
getPTE(void* virtualAddress)
{
    // verify VA param is within the range of the VA block
    if (virtualAddress < leafVABlock || virtualAddress >= leafVABlockEnd) {
        PRINT_ERROR("access violation \n");
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
    PRINT("[trimPage] trimming page with VA %llu\n", (ULONG_PTR) virtualAddress);

    PPTE PTEaddress;
    PTEaddress = getPTE(virtualAddress);

    if (PTEaddress == NULL) {
        PRINT_ERROR("could not trim VA %llu - no PTE associated with address\n", (ULONG_PTR) virtualAddress);
        return FALSE;
    }
    
    // take snapshot of old PTE
    PTE oldPTE;
    oldPTE = *PTEaddress;

    // check if PTE's valid bit is set - if not, can't be trimmed and return failure
    if (oldPTE.u1.hPTE.validBit == 0) {
        PRINT("could not trim VA %llu - PTE is not valid\n", (ULONG_PTR) virtualAddress);
        return FALSE;
    }

    // get pageNum
    ULONG_PTR pageNum;
    pageNum = oldPTE.u1.hPTE.PFN;

    // get PFN
    PPFNdata PFNtoTrim;
    PFNtoTrim = PFNarray + pageNum;

    acquireJLock(&PFNtoTrim->lockBits);

    if (oldPTE.u1.ulongPTE != PTEaddress->u1.ulongPTE) {
        releaseJLock(&PFNtoTrim->lockBits);
        PRINT("[trimPage] PTE has been changed\n");
        return FALSE;
    }

    if (PFNtoTrim->statusBits != ACTIVE) {
        releaseJLock(&PFNtoTrim->lockBits);
        PRINT("[trimPage] page has already been trimmed\n");
        return FALSE;
    }


    // zero new PTE
    PTE PTEtoTrim;
    PTEtoTrim.u1.ulongPTE = 0;


    // unmap page from VA (invalidates hardwarePTE)
    MapUserPhysicalPages(virtualAddress, 1, NULL);

    // if write in progress bit is set, modified writer re-enqueues page
    if (PFNtoTrim->writeInProgressBit == 1) {

        if (oldPTE.u1.hPTE.dirtyBit == 0) {
            PFNtoTrim->statusBits = STANDBY;
        }

        else if (oldPTE.u1.hPTE.dirtyBit == 1) {
            
            // to notify modified writer that page has since been re-modified
            PFNtoTrim->remodifiedBit = 1;

            PFNtoTrim->statusBits = MODIFIED;

        }

    }
    else {
        // check dirtyBit to see if page has been modified
        if (oldPTE.u1.hPTE.dirtyBit == 0) {

            // add given VA's page to standby list
            enqueuePage(&standbyListHead, PFNtoTrim);

        } 
        else if (oldPTE.u1.hPTE.dirtyBit == 1) {

            // add given VA's page to modified list;
            enqueuePage(&modifiedListHead, PFNtoTrim);

        }
    }


    // set transitionBit to 1
    PTEtoTrim.u1.tPTE.transitionBit = 1;  
    PTEtoTrim.u1.tPTE.PFN = pageNum;

    PTEtoTrim.u1.tPTE.permissions = getPTEpermissions(oldPTE);

    * (volatile PTE *) PTEaddress = PTEtoTrim;

    releaseJLock(&PFNtoTrim->lockBits);

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
    BOOLEAN bResult;
    bResult = (BOOLEAN) LoggedSetLockPagesPrivilege( GetCurrentProcess(), TRUE );
    return bResult;
}


VOID 
initVABlock(ULONG_PTR numPages, ULONG_PTR pageSize)
{

    // creates a VAD node that we can define (i.e. is not pagefaulted by underlying kernel mm)
    leafVABlock = VirtualAlloc(NULL, numPages*pageSize, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    if (leafVABlock == NULL) {
        exit(-1);
    }

    // TODO - do i need to avoid overflow?
    leafVABlockEnd = (PVOID) ( (ULONG_PTR)leafVABlock + NUM_PAGES*PAGE_SIZE );

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
    for (int i = 0; i < numPages; i++){
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


BOOLEAN
zeroPage(ULONG_PTR PFN)
{

    PVANode zeroVANode;
    zeroVANode = dequeueVA(&zeroVAListHead);

    if (zeroVANode == NULL) {
        PRINT("[zeroPage] TODO: waiting for release\n");
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
    //  PRINT("freeListCount == %llu\n", freeListHead.count);

    PPFNdata PFNtoZero;

    // lock whole list (to avoid other threads pulling at same times)
    PFNtoZero = dequeueLockedPage(&freeListHead, FALSE);


    if (PFNtoZero == NULL) {
        // PRINT_ERROR("free list empty - could not write out\n");
        return FALSE;
    }

    // get page number, rather than PFN metadata
    ULONG_PTR pageNumtoZero;
    pageNumtoZero = PFNtoZero - PFNarray;

    // zeroPage (does not update status bits in PFN metadata)
    zeroPage(pageNumtoZero);

    acquireJLock(&PFNtoZero->lockBits);

    // enqueue to zeroList (updates status bits in PFN metadata)
    enqueuePage(&zeroListHead, PFNtoZero);

    releaseJLock(&PFNtoZero->lockBits);

    // TODO - unlock page after move
    //  PRINT(" - Moved page from free -> zero \n");

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
        // PRINT_ERROR("zero list empty - could not write out\n");
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
            
        clearPFBitIndex(PFNtoWrite->pageFileOffset);

        PFNtoWrite->pageFileOffset = INVALID_PAGEFILE_INDEX;


        if (PFNtoWrite->statusBits != ACTIVE) {
            enqueuePage(&modifiedListHead, PFNtoWrite);                     // TODO - CHECK (reenqueing if unable to write)
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


    if (PFNtoWrite->statusBits != ACTIVE) {
        ASSERT(currPTE->u1.hPTE.validBit != 1 && currPTE->u1.tPTE.transitionBit == 1);

        // enqueue page to standby (since has not been redirtied)
        enqueuePage(&standbyListHead, PFNtoWrite);

        PRINT(" - Moved page from modified -> standby (wrote out to PageFile successfully)\n");


    } else {
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
    baseVA = VirtualAlloc(NULL, numVAs * PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

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
        currVA = (void*) ( (ULONG_PTR)baseVA + i*PAGE_SIZE );

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
testRoutine()
{
    PRINT_ALWAYS("[testRoutine]\n");
    
    /************** TESTING *****************/
    void* testVA = leafVABlock;


    PRINT_ALWAYS("--------------------------------\n");


    /************* FAULTING in and WRITING/ACCESSING testVAs *****************/

    ULONG_PTR testNum = 100;
    PRINT_ALWAYS("Committing and then faulting in %llu pages\n", testNum);


    for (int i = 0; i < testNum; i++) {

        faultStatus testStatus;

        commitVA(testVA, READ_ONLY, PAGE_SIZE*3);    // commits with READ_ONLY permissions
        protectVA(testVA, READ_WRITE, PAGE_SIZE *3);   // converts to read write

        // commitVA(testVA, READ_WRITE, 1);    // commits with read/write permissions (VirtualProtect does not allow execute permissions)
        testStatus = writeVA(testVA, testVA);
    
        // if (i % 2 == 1) {

        //     // write VA to the VA location (should remain same throughout entire course of program despite physical page changes)
        //     testStatus = writeVA(testVA, testVA);

        // } else {

        //     testStatus = accessVA(testVA, READ_ONLY);
            
        // }

        /************ TRADING pages ***********/

        #ifdef TRADE_PAGES
        PRINT("Attempting to trade VA\n");
        tradeVA(testVA);

        #endif


        PRINT("tested (VA = %llu), return status = %u\n", (ULONG_PTR) testVA, testStatus);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);

    }


    PRINT_ALWAYS("--------------------------------\n");


    /************ TRIMMING tested VAs (active -> standby/modified) **************/

    PRINT_ALWAYS("Trimming %llu pages, modified writing half of them\n", testNum);

    // reset testVa
    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {

        // trim the VAs we have just tested (to transition, from active)
        trimPage(testVA);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);


        // #ifndef MULTITHREADING                      // TODO: move when we have modifiedpagewriter thread

        #ifndef TESTING_MODIFIED
        // alternate calling modifiedPageWriter and zeroPageWriter
        if (i % 2 == 0) {

            modifiedPageWriter();

        } 
        #endif

        #ifndef MULTITHREADING                      // TODO: move when we have modifiedpagewriter thread

        else {
            
            zeroPageWriter();

        }
        #endif
    
    }



    #ifdef MULTITHREADING

    PRINT_ALWAYS("--------------------------------\n");

    /************* Creating handles/threads *************/
    PRINT_ALWAYS("Creating threads (zeropage, freepage)\n");


    HANDLE terminateZeroPageHandle;
    terminateZeroPageHandle =  CreateEvent(NULL, TRUE, FALSE, NULL);

    if (terminateZeroPageHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create event handle\n");
    }


    HANDLE zeroPageThreadHandles[NUM_ZERO_THREADS];
    for (int i = 0; i < NUM_ZERO_THREADS; i++) {

        zeroPageThreadHandles[i] = CreateThread(NULL, 0, zeroPageThread, terminateZeroPageHandle, 0, NULL);
            
        if (zeroPageThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create zeroPage handle\n");
        }
  
    }


    #ifdef TESTING_ZERO
    /************ for testing of zeropagethread **************/

    HANDLE freePageTestThreadHandles[NUM_ZERO_THREADS];
    for (int i = 0; i < NUM_ZERO_THREADS; i++) {
        freePageTestThreadHandles[i] = CreateThread(NULL, 0, freePageTestThread, terminateZeroPageHandle, 0, NULL);

        if (freePageTestThreadHandles[i] == INVALID_HANDLE_VALUE) {
            PRINT_ERROR("failed to create freePage handle\n");
        }
    }
    #endif

    #ifdef TESTING_MODIFIED
    HANDLE modifiedPageWriterThreadHandles[NUM_ZERO_THREADS];
    for (int i = 0; i < NUM_ZERO_THREADS; i++) {

        modifiedPageWriterThreadHandles[i] = CreateThread(NULL, 0, modifiedPageThread, terminateZeroPageHandle, 0, NULL);
            
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
        // trimPage(testVA);

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

    for (int i = 0; i < testNum; i++) {

        decommitVA(testVA, PAGE_SIZE*3);

        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }

    #ifdef MULTITHREADING

    SetEvent(terminateZeroPageHandle);

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

}


VOID 
main(int argc, char** argv) 
{

    /*********** switch to toggle verbosity (print statements) *************/
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        debugMode = TRUE;
    }

    // initialize zero/free/standby.. lists 
    initListHeads(listHeads);

    // initialize zeroVAList, consisting of AWE addresses for zeroing pages
    initVAList(&zeroVAListHead, NUM_ZERO_THREADS + 3);

    initVAList(&writeVAListHead, NUM_ZERO_THREADS + 3);


    // reserve AWE addresses for page trading
    pageTradeDestVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    pageTradeSourceVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    // reserve AWE address for pageFileFormatVA
    pageFileFormatVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

    // memset the pageFile bit array
    memset(&pageFileBitArray, 0, PAGEFILE_PAGES/(8*sizeof(ULONG_PTR) ) );

    // allocate an array of PFNs that are returned by AllocateUserPhysPages
    ULONG_PTR *aPFNs;
    aPFNs = VirtualAlloc(NULL, NUM_PAGES*(sizeof(ULONG_PTR)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if (aPFNs == NULL) {
        PRINT_ERROR("failed to allocate PFNarray\n");
    }

    // secure privilege for the code
    BOOLEAN privResult;
    privResult = getPrivilege();
    if (privResult != TRUE) {
        PRINT_ERROR("could not get privilege successfully \n");
        exit(-1);
    }    

    BOOLEAN bResult;

    ULONG_PTR numPagesReturned = NUM_PAGES;
    bResult = (BOOLEAN) AllocateUserPhysicalPages(GetCurrentProcess(), &numPagesReturned, aPFNs);
    if (bResult != TRUE) {
        PRINT_ERROR("could not allocate pages successfully \n");
        exit(-1);
    }

    if (numPagesReturned != NUM_PAGES) {
        PRINT("allocated only %llu pages out of %u pages requested\n", numPagesReturned, NUM_PAGES);
    }

    // create VAD
    initVABlock(numPagesReturned, PAGE_SIZE);     // will be the starting VA of the memory I've allocated

    // create local PFN metadata array
    initPFNarray(aPFNs, numPagesReturned);

    // create local PTE array to map VAs to pages
    initPTEarray(numPagesReturned);

    // create PageFile section of memory
    initPageFile(PAGEFILE_SIZE);

    // call test routine
    testRoutine();

    PRINT_ALWAYS("----------------\nprogram complete\n----------------");

    //  free memory allocated
    VirtualFree(leafVABlock, 0, MEM_RELEASE);
    VirtualFree(PFNarray, 0, MEM_RELEASE);
    VirtualFree(PTEarray, 0, MEM_RELEASE);

}
