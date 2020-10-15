#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h" 
#include "getPage.h"
#include "PTEpermissions.h"
#include "jLock.h"
#include "pageFile.h"

// Array used to convert PTEpermissions enum to standard windows permissions
DWORD windowsPermissions[] = { PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE };


faultStatus
validPageFault(PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    PTEpermissions tempRWEpermissions;

    //
    // Check requested permissions against PTE permissions
    //

    tempRWEpermissions = getPTEpermissions(snapPTE);

    if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

        PRINT_ERROR("Invalid permissions\n");
        return ACCESS_VIOLATION;

    } 
    
    //
    // Set aging bit to 1 (recently accessed)
    //

    snapPTE.u1.hPTE.agingBit = 0;

    //
    // Check for Write permissions - if write, set dirty bit and clear PF
    //

    if (permissionMasks[RWEpermissions] & writeMask) {

        PPFNdata PFN;

        snapPTE.u1.hPTE.dirtyBit = 1;

        PFN = PFNarray + snapPTE.u1.hPTE.PFN;

        acquireJLock(&PFN->lockBits);

        //
        // If the write in progress bit is clear, immediately free pf location 
        // and pf PFN pointer
        //

        if (PFN->writeInProgressBit == 0) {

            // free PF location
            clearPFBitIndex(PFN->pageFileOffset);

            // clear pagefile pointer out of PFN
            PFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        }

        //
        // Otherwise, if the write in progress bit is set, the modified writer is 
        // still writing to the pagefile, and so the bit index and pagefileOffset field
        // are cleared by the modified page writer once it finishes writing
        //

        releaseJLock(&PFN->lockBits);

        * (volatile PTE *) masterPTE = snapPTE;

    }

    PRINT("PFN is already valid\n");

    return SUCCESS;

}


faultStatus
transPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    ULONG_PTR PTEindex;
    PTE newPTE;
    PTEpermissions transRWEpermissions;
    ULONG_PTR pageNum;
    PPFNdata transitionPFN;
    DWORD oldPermissions;
    BOOL bResult;

    PRINT(" - trans page fault\n");

    //
    // Initialize new PTE to be written out
    //

    newPTE.u1.ulongPTE = 0;

    //
    // Check requested permissions against PTE permissions
    //

    transRWEpermissions = snapPTE.u1.tPTE.permissions;

    if (!checkPTEpermissions(transRWEpermissions, RWEpermissions)) {

        PRINT_ERROR("Invalid permissions\n");
        return ACCESS_VIOLATION;

    }

    //
    // Get page number and PFN data from transition PTE
    //

    pageNum = snapPTE.u1.tPTE.PFN;

    transitionPFN = PFNarray + pageNum;

    acquireJLock(&transitionPFN->lockBits);

    //
    // If page has been repurposed since lock acquisition, repeat the fault
    //

    if (snapPTE.u1.ulongPTE != masterPTE->u1.ulongPTE) {

        releaseJLock(&transitionPFN->lockBits);

        PRINT("[transPageFault] page has been repurposed from standby list\n");

        return PAGE_STATE_CHANGE;

    }

    //
    // If readInProgress bit is set, wait for event to signal completion and retry fault
    //

    if (transitionPFN->readInProgressBit == 1) {

        PHANDLE currEvent;
        PeventNode currEventNode;

        currEvent = transitionPFN->readInProgEvent;

        currEventNode = CONTAINING_RECORD(currEvent, eventNode, event);

        InterlockedIncrement(&currEventNode->refCount);

        releaseJLock(&transitionPFN->lockBits);

        releasePTELock(masterPTE);

        //
        // Since event is manually reset, this thread will not wait on an event that has already been set
        //

        WaitForSingleObject(transitionPFN->readInProgEvent, INFINITE);

        //
        // If refcount is non-zero, the event can no longer be edited
        //

        if (InterlockedDecrement(&currEventNode->refCount) == 0){

            DebugBreak();

            transitionPFN->readInProgEvent = NULL;

            enqueueEvent(&readInProgEventListHead, currEventNode);

        }

        acquirePTELock(masterPTE);

        return PAGE_STATE_CHANGE;

    }

    //
    // Verify PFN has not changed state (should not occur, since both PTE and page lock are held)
    //

    ASSERT(transitionPFN->statusBits == STANDBY || transitionPFN->statusBits == MODIFIED);

    //
    // If requested permissions are write, set dirty bit & clear PF location (if it exists)
    //
    
    if (permissionMasks[RWEpermissions] & writeMask) { 


        // if the write in progress bit is clear, immediately free pf location 
        // and pf PFN pointer
        // if the write in progress bit is set, the bit index and pagefile field
        // are cleared by the smodified page writer once it finishes writing
        if (transitionPFN->writeInProgressBit == 0) {

            // clear PF bit index
            clearPFBitIndex(transitionPFN->pageFileOffset);


            // clear pagefile pointer out of PFN
            transitionPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        }


    } else {
        
        // if PFN is modified, maintain dirty bit when switched back
        if (transitionPFN->statusBits == MODIFIED) {

            newPTE.u1.hPTE.dirtyBit = 1;

        }
        
    }

    //
    // Page is only dequeued from standby/modified if write in progress bit is zero
    //

    if (transitionPFN->writeInProgressBit == 0) {

        dequeueSpecificPage(transitionPFN);

    }

    //
    // Update PFN state with PTE index and status bits to active
    //

    PTEindex = masterPTE - PTEarray;

    transitionPFN->PTEindex = PTEindex;

    transitionPFN->statusBits = ACTIVE;

    // 
    // Update local newPTE and write out
    //

    transferPTEpermissions(&newPTE, transRWEpermissions);

    newPTE.u1.hPTE.PFN = pageNum;

    newPTE.u1.hPTE.validBit = 1;

    //
    // Set aging bit to 1 (recently accessed)
    //

    newPTE.u1.hPTE.agingBit = 0;


    * (volatile PTE *) masterPTE = newPTE;
    
    // warning - "window" between MapUserPhysicalPages and VirtualProtect may result in a brief lack of permissions protection


    // assign VA to point at physical page, mirroring our local PTE change
    bResult = MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    if (bResult != TRUE) {
        PRINT_ERROR("[trans PageFault] Kernel state issue: Error mapping user physical pages \n");
    }

    // update physical permissions of hardware PTE to match our software reference.
    bResult = VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[transRWEpermissions], &oldPermissions);

    if (bResult != TRUE) {
        PRINT_ERROR("[trans PageFault] Kernel state issue: Error virtual protecting VA with permissions %u\n", transRWEpermissions);
    }


    //
    // Both PFN and PTE have completed modification. PFN lock can be released, PTE lock is released by caller.
    //

    releaseJLock(&transitionPFN->lockBits);

    return SUCCESS;

}


faultStatus
pageFilePageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{
    
    ULONG_PTR pageNum;
    PTE newPTE;                                    
    PTEpermissions pageFileRWEpermissions;
    PPFNdata freedPFN;
    ULONG_PTR PTEindex;
    BOOL bResult;

    PRINT(" - pf page fault\n");

    //
    // check snapPTE permissions
    //

    pageFileRWEpermissions = snapPTE.u1.pfPTE.permissions;

    
    if (!checkPTEpermissions(pageFileRWEpermissions, RWEpermissions)) {

        PRINT_ERROR("Invalid permissions\n");
        return ACCESS_VIOLATION;

    }


    //
    // TODO - FORWARD PROG issue. PTE lock held and getPageAlways waits for an event, need aging/trimming thread
    // However, the issue is that the PTE lock remains held through getPageAlways, thus preventing trimming
    //

    freedPFN = getPageAlways(TRUE);         

    //
    // Set PFN status bits to standby and read in progress bit to signal to another faulter an IO read is occurring
    // - decommitVA and pageTrade as well
    // - DOES NOT enqueue to standby list since data is still being tranferred in via filesystem read
    //

    freedPFN->statusBits = STANDBY;

    freedPFN->readInProgressBit = 1;

    //
    // Create a read in progress event for a prospective faulter/decommitter/pagetrader to wait on
    //

    PeventNode readInProgEventNode;
    readInProgEventNode = dequeueLockedEvent(&readInProgEventListHead);

    while (readInProgEventNode == NULL) {

        PRINT_ERROR("failed to get event handle\n");

        WaitForSingleObject(readInProgEventListHead.newPagesEvent, INFINITE);

        readInProgEventNode = dequeueLockedEvent(&readInProgEventListHead);

    }

    freedPFN->readInProgEvent = readInProgEventNode->event;


    freedPFN->PTEindex = masterPTE - PTEarray;

    //
    // Does not need to be an atomic operation since event node cannot yet be referenced via PTE
    //

    readInProgEventNode->refCount = 1;

    //
    // RELEASE pfn lock so that it can now be viewed/modified by other aforementioned threads
    //

    releaseJLock(&freedPFN->lockBits);

    //
    // set PTE to transition state so it can be faulted by other threads
    //

    newPTE.u1.ulongPTE = 0;
    newPTE.u1.tPTE.validBit = 0;
    newPTE.u1.tPTE.transitionBit = 1;
    newPTE.u1.tPTE.permissions = pageFileRWEpermissions;

    pageNum = freedPFN - PFNarray;
    newPTE.u1.tPTE.PFN = pageNum;

    * (volatile PTE *) masterPTE = newPTE;
    
    //
    // Release PTE lock (has been held since call)
    //

    releasePTELock(masterPTE);


    //
    // Read in from filesystem
    //

    bResult = FALSE;
    
    while (bResult != TRUE) {

        bResult = readPageFromFileSystem(pageNum, snapPTE.u1.pfPTE.pageFileIndex);

    }

    //
    // Re-acquire PTE lock (post read from filesystem)
    //

    acquirePTELock(masterPTE);

    //
    // While page is being written out, PTE can be decommitted
    //

    if (newPTE.u1.ulongPTE != masterPTE->u1.ulongPTE) {

        ASSERT(freedPFN->statusBits == AWAITING_FREE);

        ASSERT(masterPTE->u1.ulongPTE == 0);

        acquireJLock(&freedPFN->lockBits);

        releaseAwaitingFreePFN(freedPFN);

        releaseJLock(&freedPFN->lockBits);

        //
        // VA has been decommitted during fault by another thread
        //

        return ACCESS_VIOLATION;

    }
    
    //
    // Get PTE index, set in PFN field, and set status bits to active
    // (prepping to validate PTE)
    //  

    PTEindex = masterPTE - PTEarray;

    freedPFN->PTEindex = PTEindex;

    freedPFN->statusBits = ACTIVE;

    //
    // Zero out local newPTE so it can be changed to active once again
    //

    newPTE.u1.ulongPTE = 0;

    //
    // Set local newPTE validBit
    //

    newPTE.u1.hPTE.validBit = 1;


    //
    // Set aging bit to 1 (recently accessed)
    //

    newPTE.u1.hPTE.agingBit = 0;

    //
    // If attempting to write, set dirty bit & clear PF location if it exists
    //

    if (permissionMasks[RWEpermissions] & writeMask) {

        newPTE.u1.hPTE.dirtyBit = 1;

        //
        // free PF location 
        // Note: locking is required, since if two threads clear near-simultaneously, 
        // the PFbit index could be used for another unrelated purpose and then the 
        // second thread would clear data that is unrelated and unexpected
        //

        clearPFBitIndex(snapPTE.u1.pfPTE.pageFileIndex);

        // clear pagefile pointer out of PFN
        freedPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

    }


    // transfer permissions bit from pfPTE, if it exists
    transferPTEpermissions(&newPTE, pageFileRWEpermissions);

    // put PFN's corresponding pageNum into PTE
    newPTE.u1.hPTE.PFN = pageNum;
    
    // compiler writes out as indivisible store
    * (volatile PTE *) masterPTE = newPTE;

    DWORD oldPermissions;

    // warning - "window" between MapUserPhysicalPages and VirtualProtect may result in a lack of permissions protection

    
    // assign VA to point at physical page, mirroring our local PTE change
    bResult = MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    if (bResult != TRUE) {
        PRINT_ERROR("[pf PageFault] Kernel state issue: Error mapping user physical pages\n");
    }


    // update physical permissions of hardware PTE to match our software reference.
    bResult = VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[pageFileRWEpermissions], &oldPermissions);

    if (bResult != TRUE) {
        PRINT_ERROR("[pf PageFault] Kernel state issue: Error virtual protecting VA with permissions %u\n", pageFileRWEpermissions);
    }

    //
    // set readInProgressBit to 0 and set event so other threads that have transition faulted on this PTE can proceed
    //

    ASSERT(freedPFN->readInProgressBit == 1);
    freedPFN->readInProgressBit = 0;
    
    SetEvent(freedPFN->readInProgEvent);

    if (InterlockedDecrement(&readInProgEventNode->refCount) == 0){

        freedPFN->readInProgEvent = NULL;

        enqueueEvent(&readInProgEventListHead, readInProgEventNode);

    }

    return SUCCESS;

}


faultStatus
demandZeroPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    PRINT(" - dz page fault\n");

    // declare pageNum (PFN) ULONG
    ULONG_PTR pageNum;

    PTE newPTE;
    newPTE.u1.ulongPTE = 0;

    // check permissions
    PTEpermissions dZeroRWEpermissions;
    dZeroRWEpermissions = snapPTE.u1.dzPTE.permissions;

    if (!checkPTEpermissions(dZeroRWEpermissions, RWEpermissions)) {

        PRINT_ERROR("Invalid permissions\n");
        return ACCESS_VIOLATION;

    } 
    // if write, set dirty bit 
    else if (permissionMasks[RWEpermissions] & writeMask) {

        // set PTE to dirty
        newPTE.u1.hPTE.dirtyBit = 1;

    }


    // calculate PTEindex
    ULONG_PTR PTEindex;
    PTEindex = masterPTE - PTEarray;


    // dequeue a page of memory from freed list, setting status bits to active
    PPFNdata freedPFN;
    freedPFN = getPageAlways(TRUE);


    // set PFN status to active
    freedPFN->statusBits = ACTIVE;

    // set PFN PTE index
    freedPFN->PTEindex = PTEindex;

    // RELEASE pfn lock so that it can now be viewed/modified by other threads
    releaseJLock(&freedPFN->lockBits);


    // calculate PFN index and assign in PTE field
    pageNum = freedPFN - PFNarray;
    newPTE.u1.hPTE.PFN = pageNum;

    //
    // change PTE to valid (setting valid bit)
    // 

    newPTE.u1.hPTE.validBit = 1;

    //
    // Set aging bit to 1 (recently accessed)
    //

    newPTE.u1.hPTE.agingBit = 0;

    // copy permissions from transition PTE into our soon-to-be-active PTE
    transferPTEpermissions(&newPTE, dZeroRWEpermissions);

    // compiler writes out as indivisible store
    * (volatile PTE *) masterPTE = newPTE;
    
    DWORD oldPermissions;

    // warning - "window" between MapUserPhysicalPages and VirtualProtect may result in a lack of permissions protection

    BOOL bresult;

    // assign VA to point at physical page, mirroring our local PTE change
    bresult = MapUserPhysicalPages(virtualAddress, 1, &pageNum);
    if (bresult != TRUE) {
        PRINT_ERROR("[dz PageFault] Error mapping user physical pages\n");
    }

    // update physical permissions of hardware PTE to match our software reference.
    bresult = VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[dZeroRWEpermissions], &oldPermissions);
    if (bresult != TRUE) {
        PRINT_ERROR("[dz PageFault] Error virtual protecting VA with permissions %u\n", dZeroRWEpermissions);
    }

    return SUCCESS;                                             // return value of 2; 

}



// faultStatus
// checkVADPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
// {

//     // snapPTE should be zeroed out (to even get to this function)
//     PTE newPTE;
//     newPTE.u1.ulongPTE = 0;

//     PVADNode currVAD;
//     currVAD = getVAD(virtualAddress);

//     if (currVAD == NULL) {
//         PRINT_ERROR ("not in VAD\n");
//         return ACCESS_VIOLATION;
//     }

//     // get permissions from the VAD
//     PTEpermissions VADpermissions;
//     VADpermissions = currVAD->permissions;

//     // transfer permissions
//     transferPTEpermissions(&snapPTE, VADpermissions);


//     // TODO: make sure this checks out
//     return demandZeroPageFault(virtualAddress, RWEpermissions, snapPTE, masterPTE);

// }



faultStatus
pageFault(void* virtualAddress, PTEpermissions RWEpermissions)
{

    PPTE currPTE;
    PTE oldPTE;
    faultStatus status;

    PRINT("[pageFault]");

    //
    // get the PTE from the VA
    //

    currPTE = getPTE(virtualAddress);

    //
    // invalid VA (not in range)
    //

    if (currPTE == NULL) {

        PRINT_ERROR(" VA not in range\n");
        return ACCESS_VIOLATION;

    }

    //
    // Accquire PTE lock (held through all save pf pagefault)
    //

    acquirePTELock(currPTE);

    //
    // make a shallow copy/"snapshot" of the PTE to edit and check
    //

    oldPTE = *currPTE;

    if (oldPTE.u1.hPTE.validBit == 1) {                                 // VALID STATE PTE

        //
        // PFN lock is acquired within validPageFault solely to check write in progress bit
        // - in the case of modified write ->faulted back to valid
        //

        status = validPageFault(RWEpermissions, oldPTE, currPTE);

    } 
    else if (oldPTE.u1.tPTE.transitionBit == 1) {                       // TRANSITION STATE PTE


        //
        // PFN lock is acquired by transPageFault since it may be released in readinprog case
        //

        status = transPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

    }
    else if (oldPTE.u1.pfPTE.permissions != NO_ACCESS &&                // PAGEFILE STATE PTE
             oldPTE.u1.pfPTE.pageFileIndex != INVALID_PAGEFILE_INDEX) { 

        //
        // Note: PTE lock released pre-filesystem read and re-acquired post read
        //

        status = pageFilePageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);  

    }
    else if (oldPTE.u1.pfPTE.permissions != NO_ACCESS) {                // DEMAND ZERO STATE PTE  

        status = demandZeroPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

    }
    else if (oldPTE.u1.ulongPTE == 0) {                                 // ZERO STATE PTE

        // TODO (future): may need to check if vad is mem commit and bring in permissions
        // status = checkVADPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

        PRINT("access violation - PTE is zero\n");
        status = ACCESS_VIOLATION;

    }

    else {

        PRINT_ERROR("ERROR - not in any recognized PTE state\n");
        status = ACCESS_VIOLATION;

    }

    //
    // Release PTE lock
    //

    releasePTELock(currPTE);
    
    return status;

}
