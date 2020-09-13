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

void* pageTradeDestVA;                  // specific VA used for page trading destination
void* pageTradeSourceVA;                // specific VA used for page trading source

ULONG_PTR totalCommittedPages;      // count of committed pages (initialized to zero)
ULONG_PTR totalMemoryPageLimit = NUM_PAGES + PAGEFILE_SIZE / PAGE_SIZE;    // limit of committed pages (memory block + pagefile space)

void* pageFileVABlock;              // starting address of pagefile "disk" (memory)
void* modifiedWriteVA;              // specific VA used for writing out page contents to pagefile
void* pageFileFormatVA;             // specific VA used for copying in page contents from pagefile
ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];

// Execute-Write-Read (bit ordering)
ULONG_PTR permissionMasks[] = { 0, readMask, (readMask | writeMask), (readMask | executeMask), (readMask | writeMask | executeMask) };

listData listHeads[ACTIVE];         // intialization of listHeads array

listData zeroVAListHead;

BOOLEAN debugMode;

#define TESTING_ZERO


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
    PRINT("trimming page with VA %llu\n", (ULONG_PTR) virtualAddress);

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
        PRINT_ERROR("could not trim VA %llu - PTE is not valid\n", (ULONG_PTR) virtualAddress);
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
        PRINT_ERROR("Could not allocate for PFN metadata array\n");
        exit(-1);
    }

    // loop through all PFNs, MEM_COMMITTING PFN metadata subsections for each in the aPFNs array
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
        PRINT_ERROR("Could not allocate for PTEarray\n");
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
        PRINT_ERROR("Could not allocate for pageFile\n");
        exit(-1);
    }
    return pageFileAddress;
}


BOOLEAN
zeroPage(ULONG_PTR PFN)
{

    PVANode zeroVANode;
    zeroVANode = dequeueVA(&zeroVAListHead);

    if (zeroVANode == NULL) {
        PRINT("TODO: waiting for release\n");
    }

    PVOID zeroVA;
    zeroVA = zeroVANode->VA;

    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(zeroVA, 1, &PFN)) {
        PRINT_ERROR("error remapping zeroVA\n");
        return FALSE;
    }

    memset(zeroVA, 0, PAGE_SIZE);

    // unmap zeroVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(zeroVA, 1, NULL)) {
        PRINT_ERROR("error zeroing page\n");
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
    PFNtoZero = dequeuePage(&freeListHead);


    if (PFNtoZero == NULL) {
        // PRINT_ERROR("free list empty - could not write out\n");
        return FALSE;
    }

    // get page number, rather than PFN metadata
    ULONG_PTR pageNumtoZero;
    pageNumtoZero = PFNtoZero - PFNarray;

    // zeroPage (does not update status bits in PFN metadata)
    zeroPage(pageNumtoZero);

    // enqueue to zeroList (updates status bits in PFN metadata)
    enqueuePage(&zeroListHead, PFNtoZero);
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
    PFNtoFree = dequeuePage(&zeroListHead);


    if (PFNtoFree == NULL) {
        // PRINT_ERROR("zero list empty - could not write out\n");
        return FALSE;
    }

    // enqueue to freeList (updates status bits in PFN metadata)
    enqueuePage(&freeListHead, PFNtoFree);

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


    PRINT("modifiedListCount == %llu\n", modifiedListHead.count);
    PPFNdata PFNtoWrite;
    PFNtoWrite = dequeuePage(&modifiedListHead);

    if (PFNtoWrite == NULL) {
        PRINT("modified list empty - could not write out\n");
        return FALSE;
    }


    BOOLEAN bResult;
    bResult = writePage(PFNtoWrite);

    if (bResult != TRUE) {
        PRINT_ERROR("error writing out page\n");
        return FALSE;
    }

    // PFN status to standby (from modified)
    PFNtoWrite->statusBits = STANDBY;

    // enqueue page to standby
    enqueuePage(&standbyListHead, PFNtoWrite);

    PRINT(" - Moved page from modified -> standby (wrote out to PageFile successfully)\n");

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

    // zero pages until none left to zero
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
testRoutine()
{
    PRINT_ALWAYS("[testRoutine]\n");
    
    /************** TESTING *****************/
    PPTE currPTE = PTEarray;
    void* testVA = leafVABlock;


    PRINT_ALWAYS("--------------------------------\n");


    /************* FAULTING in and WRITING/ACCESSING testVAs *****************/

    ULONG_PTR testNum = 10;
    PRINT_ALWAYS("Committing and then faulting in %llu pages\n", testNum);


    for (int i = 0; i < testNum; i++) {

        faultStatus testStatus;

        commitVA(testVA, READ_ONLY, 1);    // commits with READ_ONLY permissions
        protectVA(testVA, READ_WRITE, 1);   // converts to read write

        // commitVA(testVA, READ_WRITE, 1);    // commits with read/write permissions (VirtualProtect does not allow execute permissions)
    
        if (i % 2 == 1) {

            // write VA to the VA location (should remain same throughout entire course of program despite physical page changes)
            testStatus = writeVA(testVA, testVA);

        } else {

            testStatus = accessVA(testVA, READ_ONLY);
            
        }

        /************ TRADING pages ***********/

        // PRINT("Attempting to trade VA\n");
        // tradeVA(testVA);


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

        // alternate calling modifiedPageWriter and zeroPageWriter
        if (i % 2 == 0) {

            modifiedPageWriter();

        } 
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

    HANDLE zeroPageHandle;
    zeroPageHandle = CreateThread(NULL, 0, zeroPageThread, terminateZeroPageHandle, 0, NULL);

    if (zeroPageHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create zeroPage handle\n");
    }


    #ifdef TESTING_ZERO
    /************ for testing of zeropagethread **************/

    HANDLE freePageTestHandle;
    freePageTestHandle = CreateThread(NULL, 0, freePageTestThread, terminateZeroPageHandle, 0, NULL);

    if (freePageTestHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("failed to create zeroPage handle\n");
    }
    #endif

    // HANDLE zeroPageHandle2;
    // zeroPageHandle2 = CreateThread(NULL, 0, zeroPageThread, handles, 0, NULL);
    // if (zeroPageHandle == INVALID_HANDLE_VALUE) {
    //     PRINT_ERROR("failed to create zeroPage handle\n");
    // }

    #endif

    PRINT_ALWAYS("--------------------------------\n");

    // testVA = leafVABlock;

    // // toggle - can either FAULT or TEST VAs 
    // for (int i = 0; i < testNum; i++) {
    //     faultStatus testStatus = accessVA(testVA, READ_WRITE);  // to TEST VAs
    //     // faultStatus testStatus = pageFault(testVA, READ_ONLY);      // to FAULT VAs
    
    //     PRINT("tested (VA = %d), return status = %u\n", (ULONG) testVA, testStatus);
    //     testVA = (void*) ( (ULONG) testVA + PAGE_SIZE);
    // }


    /****************** FAULTING back in trimmed pages ******************/

    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {
        // trimPage(testVA);

        #ifdef CHECK_PAGEFILE
        // to test PF fault - switch order in getPage and add this
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

        PRINT("decommiting (VA = 0x%llx) with contents 0x%llx\n", (ULONG_PTR) testVA, * (ULONG_PTR*) testVA);
        PRINT("decommiting (VA = 0x%llx) with contents %s\n", (ULONG_PTR) testVA, * (PCHAR *) testVA);

        decommitVA(testVA, 1);

        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }

    #ifdef MULTITHREADING
    SetEvent(terminateZeroPageHandle);

    WaitForSingleObject(zeroPageHandle, INFINITE);


    #ifdef TESTING_ZERO
    WaitForSingleObject(freePageTestHandle, INFINITE);
    #endif
    #endif

    // WaitForSingleObject(zeroPageHandle2, INFINITE);

}


VOID
initZeroVAList(ULONG_PTR numVAs)
{
    if (numVAs < 1) {
        PRINT_ERROR("[initZeroVAList] Cannot initialize list of zeroVAs with length 0\n");
        exit (-1);
    }

    // initialize lock field
    InitializeCriticalSection(&(zeroVAListHead.lock));

    // initialize head
    initLinkHead(&(zeroVAListHead.head));

    zeroVAListHead.count = 0;

    HANDLE newVAsHandle;
    newVAsHandle =  CreateEvent(NULL, FALSE, FALSE, NULL);

    if (newVAsHandle == INVALID_HANDLE_VALUE) {
        PRINT_ERROR("[initZeroVAList] failed to create event handle\n");
        exit(-1);
    }

    zeroVAListHead.newPagesEvent = newVAsHandle;


    // alloc for VAs
    void* baseZeroVA;
    baseZeroVA = VirtualAlloc(NULL, numVAs * PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

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
        currVA = (void*) ( (ULONG_PTR)baseZeroVA + i*PAGE_SIZE );

        currNode->VA = currVA;

        // enqueue node to list
        enqueueVA(&zeroVAListHead, currNode);
    }

    
}



VOID 
main(int argc, char** argv) 
{

    /*********** switch to toggle verbosity (print statements) *************/
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        debugMode = TRUE;
    }

    initListHeads(listHeads);

    // reserve AWE address for zeroVA
    zeroVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    initZeroVAList(3);

    pageTradeDestVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    pageTradeSourceVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);


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
    bResult = AllocateUserPhysicalPages(GetCurrentProcess(), &numPagesReturned, aPFNs);
    if (bResult != TRUE) {
        PRINT_ERROR("could not allocate pages successfully \n");
        exit(-1);
    }

    if (numPagesReturned != NUM_PAGES) {
        PRINT("allocated only %llu pages out of %u pages requested\n", numPagesReturned, NUM_PAGES);
    }

    // create VAD
    leafVABlock = initVABlock(numPagesReturned, PAGE_SIZE);     // will be the starting VA of the memory I've allocated

    // create local PFN metadata array
    PFNarray = initPFNarray(aPFNs, numPagesReturned, PAGE_SIZE);

    // create local PTE array to map VAs to pages
    PTEarray = initPTEarray(numPagesReturned, PAGE_SIZE);

    // create PageFile section of memory
    pageFileVABlock = initPageFile(PAGEFILE_SIZE);

    // call test routine
    testRoutine();

    PRINT_ALWAYS("----------------\nprogram complete\n----------------");

    //  free memory allocated
    VirtualFree(leafVABlock, 0, MEM_RELEASE);
    VirtualFree(PFNarray, 0, MEM_RELEASE);
    VirtualFree(PTEarray, 0, MEM_RELEASE);

}
