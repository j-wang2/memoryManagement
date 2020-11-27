#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h" 
#include "getPage.h"
#include "PTEpermissions.h"
#include "jLock.h"
#include "pageFile.h"
#include "VADNodes.h"

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

        PRINT("Invalid permissions\n");
        return ACCESS_VIOLATION;

    } 
    
    //
    // Set aging bit to 0 (recently accessed)
    //

    snapPTE.u1.hPTE.agingBit = 0;

    //
    // Check for Write permissions - if write, clear pagefile space 
    // OR, if write in progress, set remodified bit to signal modified
    // writer to clear space upon completion
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

            //
            // Free pagefile space
            //

            clearPFBitIndex(PFN->pageFileOffset);

            //
            // Clear pagefile pointer out of PFN
            //

            PFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        }         
        else {

            //
            // If PFN write in progress bit is set on write fault,
            // set remodified bit in PFN to signal modified page writer
            // to clear PF space and PF offset field in PFN upon 
            // write completion (since contents are now stale)
            //

            PFN->remodifiedBit = 1; 

        }

        //
        // Otherwise, if the write in progress bit is set, the modified writer is 
        // still writing to the pagefile, and so the bit index and pagefileOffset field
        // are cleared by the modified page writer once it finishes writing
        //

        releaseJLock(&PFN->lockBits);

        writePTE(masterPTE, snapPTE);

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

        PRINT("Invalid permissions\n");
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
    // Verify PFN has not changed state (should not occur, since both PTE and page lock are held)
    //

    ASSERT(transitionPFN->statusBits == STANDBY || transitionPFN->statusBits == MODIFIED);

    //
    // If readInProgress bit is set, wait for event to signal completion and retry fault
    //

    if (transitionPFN->readInProgressBit == 1) {

        HANDLE currEvent;
        PeventNode currEventNode;
        ULONG_PTR currPTEIndex;

        currPTEIndex = transitionPFN->PTEindex;

        currEventNode = transitionPFN->readInProgEventNode;

        currEvent = currEventNode->event;

        ASSERT(currEventNode != NULL && currEvent != NULL);

        //
        // Bump PFN refCount, since a transition-faulting thread is now awaiting the 
        // pagefile read completion
        //

        transitionPFN->refCount++;

        releaseJLock(&transitionPFN->lockBits);

        releasePTELock(masterPTE);

        //
        // Since event is manually reset, this thread will not wait on an
        // event that has already been set (thus avoiding potential deadlock)
        //

        WaitForSingleObject(currEvent, INFINITE);

        //
        // If refcount is non-zero, the event can no longer be edited
        //

        acquireJLock(&transitionPFN->lockBits);

        //
        // Assert PFN has not changed since release/re-acquisition of page lock,
        // since refcount is bumped (and this count should mirror the cleared status
        // of the read in progress bit, which is checked before making any
        // page changes in the trim/decommit/other pagefault functions)
        //

        ASSERT(currEventNode == transitionPFN->readInProgEventNode);

        ASSERT(currPTEIndex == transitionPFN->PTEindex);

        transitionPFN->refCount--;

        //
        // If current thread is the last waiting thread, clear the 
        // readInProg node from the PFN field and re-enqueue
        // to event list.
        //

        if (transitionPFN->refCount == 0) {  

            transitionPFN->readInProgEventNode = NULL;

            enqueueEvent(&readInProgEventListHead, currEventNode);

            //
            // If current thread was last waiting thread, the page was 
            // not enqueued to a list. Therefore, this fault must re-enqueue
            // the page in accordance with status bits.
            //

            if (transitionPFN->statusBits == AWAITING_FREE) {

                releaseAwaitingFreePFN(transitionPFN);

            } else if (transitionPFN->statusBits == MODIFIED
                       || (transitionPFN->statusBits == STANDBY && transitionPFN->remodifiedBit) ) {

                //
                // Clear remodified bit and enqueue to modified list
                //

                transitionPFN->remodifiedBit = 0;

                enqueuePage(&modifiedListHead, transitionPFN);

            } else if (transitionPFN->statusBits == STANDBY) {
                
                enqueuePage(&standbyListHead, transitionPFN);

            }

        }

        releaseJLock(&transitionPFN->lockBits);

        acquirePTELock(masterPTE);

        return PAGE_STATE_CHANGE;

    }

    //
    // If requested permissions are write, set dirty bit & clear PF location (if it exists)
    //
    
    if (permissionMasks[RWEpermissions] & writeMask) { 

        //
        // Set PTE dirty bit - the requested permission are write
        //

        newPTE.u1.hPTE.dirtyBit = 1;

        //
        // If the write in progress bit is clear and PFN status bits are standby,
        // immediately free pagefile location and pagefile offset field from PFN.
        // In the case of a modified page, since the PFN lock is held, no pagefile
        // space corresponds with the provided PFN
        //

        if (transitionPFN->writeInProgressBit == 0 && transitionPFN->statusBits == STANDBY) {

            //
            // Clear pagefile space (reliant on PFN pagefile offset field 
            // accuracy)
            //

            clearPFBitIndex(transitionPFN->pageFileOffset);

            //
            // Clear pagefile pointer out of PFN
            //

            transitionPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

        } 
        else {
 
            //
            // If PFN write in progress bit is set on write fault,
            // set remodified bit in PFN to signal modified page writer
            // to clear PF space and PF offset field in PFN upon 
            // write completion (since contents are now stale)
            //

            transitionPFN->remodifiedBit = 1;

        }


    } 
    
    //
    // Page is only dequeued from standby/modified if write in progress bit is zero
    // AND refCount is zero (indicating no read is currently occuring or being completed)
    //

    if (transitionPFN->writeInProgressBit == 0 && transitionPFN->refCount == 0) {

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
    // Set aging bit to 0 (recently accessed)
    //

    newPTE.u1.hPTE.agingBit = 0;


    writePTE(masterPTE, newPTE);
    
    
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
    BOOL clearIndex;
    PeventNode readInProgEventNode;

    clearIndex = FALSE;

    PRINT(" - pf page fault\n");

    //
    // check snapPTE permissions
    //

    pageFileRWEpermissions = snapPTE.u1.pfPTE.permissions;

    
    if (!checkPTEpermissions(pageFileRWEpermissions, RWEpermissions)) {

        PRINT("Invalid permissions\n");
        return ACCESS_VIOLATION;

    }

    //
    // Create a read in progress event for a prospective faulter/decommitter/pagetrader to wait on.
    // Acquired pre-page lock acquisition to avoid waiting while holding page lock (although PTE lock
    // remains held, which could be improved in future optimizations)
    //

    readInProgEventNode = dequeueLockedEvent(&readInProgEventListHead);

    while (readInProgEventNode == NULL) {

        PRINT("failed to get event handle\n");

        WaitForSingleObject(readInProgEventListHead.newPagesEvent, INFINITE);

        readInProgEventNode = dequeueLockedEvent(&readInProgEventListHead);

    }

    //
    // dequeue a page of memory from freed list
    //

    freedPFN = getPage(TRUE);

    if (freedPFN == NULL) {

        //
        // Return immediately to caller so PTE lock can be released
        // No page lock is held since getPage call failed
        // Signals to caller to wait for new pages and then re-fault post 
        // release of PTE lock
        //

        enqueueEvent(&readInProgEventListHead, readInProgEventNode);

        return NO_AVAILABLE_PAGES;

    }        

    //
    // Set PFN status bits to standby and read in progress bit to signal to another faulter an IO read is occurring
    // - decommitVA and pageTrade as well
    // - DOES NOT enqueue to standby list since data is still being tranferred in via filesystem read
    //

    freedPFN->statusBits = STANDBY;

    freedPFN->readInProgressBit = 1;

    freedPFN->readInProgEventNode = readInProgEventNode;


    freedPFN->PTEindex = masterPTE - PTEarray;

    //
    // Assqign PFN pagefile offset field to PTE pagefile index
    // so that upon possible failure it can be freed
    //

    freedPFN->pageFileOffset = snapPTE.u1.pfPTE.pageFileIndex;

    //
    // Initialize PFN refCount to 1, since page is being referenced upon read by
    // current thread until completion of pagefile read. Does not need to be an
    // atomic operation since page lock is held and event node cannot yet be 
    // referenced via PTE (since PTE lock is held untnil transition state
    // is set)
    //

    ASSERT(freedPFN->refCount == 0);

    freedPFN->refCount = 1;

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

    //
    // Calculate PFN index
    //

    pageNum = freedPFN - PFNarray;

    newPTE.u1.tPTE.PFN = pageNum;

    writePTE(masterPTE, newPTE);
    
    
    //
    // Release PTE lock (has been held since call)
    //

    releasePTELock(masterPTE);


    //
    // Read page contents in from filesystem
    //

    bResult = FALSE;

    //
    // If address verification is enabled (asserts that the contents
    // of a given page corresponds to the virtual address it is mapped
    // or standby at)
    //

    ULONG_PTR signature;

    #ifdef TESTING_VERIFY_ADDRESSES

        signature = (ULONG_PTR) virtualAddress & ~(PAGE_SIZE - 1);
    
    #else

        signature = 0;

    #endif

    while (bResult != TRUE) {

        bResult = readPageFromFileSystem(pageNum, snapPTE.u1.pfPTE.pageFileIndex, signature);

    }

    //
    // Re-acquire PTE lock (post read from filesystem)
    //

    acquirePTELock(masterPTE);

    //
    // While page is being read in from filesystem, the only PTE state change that 
    // can occur is a decommit. However, a subsequent recommit/pagefault/trim/etc 
    // can very well occur post-initial decommit
    //

    if (newPTE.u1.ulongPTE != masterPTE->u1.ulongPTE) {

        acquireJLock(&freedPFN->lockBits);

        ASSERT(freedPFN->statusBits == AWAITING_FREE);

        //
        // Clear readInProgressBit and set event to notify any other threads
        // in transition pagefault that have waited on this PTE's
        // pagefile read completion.
        //

        ASSERT(freedPFN->readInProgressBit == 1);

        freedPFN->readInProgressBit = 0;
        
        SetEvent(readInProgEventNode->event);

        freedPFN->refCount--;

        //
        // If current thread is the last waiting thread, clear the 
        // readInProg node from the PFN field and re-enqueue
        // to event list. Also enqueue page to free list
        //

        if (freedPFN->refCount == 0) {

            freedPFN->readInProgEventNode = NULL;

            enqueueEvent(&readInProgEventListHead, readInProgEventNode);

            //
            // Release PFN in awaiting free state (i.e. clear
            // pagefile space, reset pagefileoffset field, 
            // clear remodified bit)
            //

            releaseAwaitingFreePFN(freedPFN);

        }

        releaseJLock(&freedPFN->lockBits);

        //
        // VA has been decommitted during fault by another thread
        //

        return PAGE_STATE_CHANGE;

    }
    
    //
    // Get PTE index, set in PFN field, and set status bits to active
    // (prepping to validate PTE)
    //  

    PTEindex = masterPTE - PTEarray;

    //
    // Zero out local newPTE so it can be changed to active once again
    //

    newPTE.u1.ulongPTE = 0;

    //
    // Set local newPTE validBit
    //

    newPTE.u1.hPTE.validBit = 1;

    //
    // Set aging bit to 0 (recently accessed)
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
        // second thread would clear data that is unrelated and unexpected..
        // Pagefile lock is acquired and held in the clearPFBitIndex code
        //

        clearPFBitIndex(snapPTE.u1.pfPTE.pageFileIndex);

        //
        // Set flag to clear index once PFN lock has been acquired
        //

        clearIndex = TRUE;

    }

    //
    // Transfer permissions bit from pfPTE, if it exists
    //

    transferPTEpermissions(&newPTE, pageFileRWEpermissions);

    //
    // put PFN's corresponding pageNum into PTE
    //

    newPTE.u1.hPTE.PFN = pageNum;

    DWORD oldPermissions;

    // warning - "window" between MapUserPhysicalPages and VirtualProtect may result in a lack of permissions protection

    //
    // Assign VA to point at physical page, mirroring our local PTE change
    //

    bResult = MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    if (bResult != TRUE) {

        PRINT_ERROR("[pf PageFault] Kernel state issue: Error mapping user physical pages\n");

    }

    //
    // Update physical permissions of hardware PTE to match our software reference.
    //

    bResult = VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[pageFileRWEpermissions], &oldPermissions);

    if (bResult != TRUE) {

        PRINT_ERROR("[pf PageFault] Kernel state issue: Error virtual protecting VA with permissions %u\n", pageFileRWEpermissions);

    }

    acquireJLock(&freedPFN->lockBits);

    //
    // Clear readInProgressBit and set event to notify any other threads
    // in transition pagefault that have waited on this PTE's
    // pagefile read completion.
    //

    ASSERT(freedPFN->readInProgressBit == 1);

    freedPFN->readInProgressBit = 0;

    freedPFN->PTEindex = PTEindex;

    freedPFN->statusBits = ACTIVE;

    ASSERT(freedPFN->pageFileOffset == snapPTE.u1.pfPTE.pageFileIndex);

    //
    // If current thread is the last waiting thread, clear the 
    // readInProg node from the PFN field and re-enqueue
    // to event list. PFN itself is not enqueued onto a list since
    // it is active, and since no PTE change has occurred since
    // PTE lock was acquired
    //    

    freedPFN->refCount--;

    if (freedPFN->refCount == 0) {

        freedPFN->readInProgEventNode = NULL;

        enqueueEvent(&readInProgEventListHead, readInProgEventNode);

    } else {

        SetEvent(readInProgEventNode->event);

    }

    //
    // Before PFN lock is released, check clearIndex flag in order to 
    // decide whether pageFileOffset field must be cleared from PFN
    //

    if (clearIndex == TRUE) {

        freedPFN->pageFileOffset = INVALID_BITARRAY_INDEX;

    }

    releaseJLock(&freedPFN->lockBits);
    
    //
    // Compiler writes out as indivisible store
    //
    
    writePTE(masterPTE, newPTE);

    return SUCCESS;

}


faultStatus
demandZeroPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    PRINT(" - dz page fault\n");

    // declare pageNum (PFN) ULONG
    ULONG_PTR pageNum;
    ULONG_PTR PTEindex;
    PPFNdata freedPFN;
    PTE newPTE;
    PTEpermissions dZeroRWEpermissions;
    DWORD oldPermissions;
    BOOL bresult;   

    newPTE.u1.ulongPTE = 0;

    //
    // Verify requested (RWE) permissions against PTE permissions
    //

    dZeroRWEpermissions = snapPTE.u1.dzPTE.permissions;

    if (!checkPTEpermissions(dZeroRWEpermissions, RWEpermissions)) {

        PRINT("[dzPageFault] Invalid permissions\n");
        return ACCESS_VIOLATION;

    } 

    //
    // If permissions requested are write, set dirty bit 
    //

    if (permissionMasks[RWEpermissions] & writeMask) {

        newPTE.u1.hPTE.dirtyBit = 1;

    }

    //
    // calculate PTEindex
    //

    PTEindex = masterPTE - PTEarray;

    //
    // dequeue a page of memory from freed list, setting status bits to active
    //

    freedPFN = getPage(TRUE);

    if (freedPFN == NULL) {

        //
        // Return immediately to caller so PTE lock can be released
        // No page lock is held since getPage call failed
        // Signals to caller to wait for new pages and then re-fault post 
        // release of PTE lock
        //

        return NO_AVAILABLE_PAGES;

    }   

    //
    // Set PFN status to active and PTE index field within PFN
    //

    freedPFN->statusBits = ACTIVE;

    freedPFN->PTEindex = PTEindex;

    //
    // Release pfn lock so that it can now be viewed/modified by other threads
    //

    releaseJLock(&freedPFN->lockBits);

    //
    // Calculate PFN index and assign in PTE field
    //

    pageNum = freedPFN - PFNarray;

    newPTE.u1.hPTE.PFN = pageNum;

    //
    // Change PTE state to valid (setting valid bit)
    // 

    newPTE.u1.hPTE.validBit = 1;

    //
    // Set aging bit to 0 (recently accessed)
    //

    newPTE.u1.hPTE.agingBit = 0;

    //
    // Copy permissions from demand zero PTE into our soon-to-be-active PTE
    //

    transferPTEpermissions(&newPTE, dZeroRWEpermissions);

    //
    // Compiler writes out new PTE indivisibly (although PTE lock is held anyway)
    //

    writePTE(masterPTE, newPTE);
    
    //
    // Note: Without PTE locking/synchronization, the time between MapUserPhysicalPages
    // and VirtualProtect calls would result in an exploitable lack of permissions protection
    //

    //
    // assign VA to point at physical page, mirroring our local PTE change
    //

    bresult = MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    if (bresult != TRUE) {

        PRINT_ERROR("[dz PageFault] Error mapping user physical pages\n");

    }

    //
    // update physical permissions of hardware PTE to match our software reference.
    //

    bresult = VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[dZeroRWEpermissions], &oldPermissions);

    if (bresult != TRUE) {
        PRINT_ERROR("[dz PageFault] Error virtual protecting VA with permissions %u\n", dZeroRWEpermissions);
    }

    return SUCCESS;                                             // return value of 2; 

}



faultStatus
checkVADPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    PVADNode currVAD;
    PTEpermissions VADpermissions;
    faultStatus dzStatus;

    ASSERT(masterPTE->u1.dzPTE.decommitBit == 0);

    //
    // Get VAD "write" lock (acquiring the other VAD lock causes
    // AB-BA deadlock issue with PTE lock, since PTE lock is acquired 
    // AFTER VAD "read" lock in commit/decommit)
    //

    EnterCriticalSection(&VADWriteLock);

    currVAD = getVAD(virtualAddress);

    //
    // If VAD is non-existent, reserve, or deleted, return an access violation
    //

    if (currVAD == NULL || currVAD->commitBit == 0 || currVAD->deleteBit) {

        LeaveCriticalSection(&VADWriteLock);

        PRINT("[checkVADPageFault] VA does not correspond to a VAD\n");

        return ACCESS_VIOLATION;

    }

    //
    // Get permissions from the VAD and insert into new "demand zero" PTE
    //

    VADpermissions = currVAD->permissions;

    snapPTE.u1.dzPTE.permissions = VADpermissions;

    dzStatus = demandZeroPageFault(virtualAddress, RWEpermissions, snapPTE, masterPTE);

    //
    // VAD "write" lock must be held throughout fault to avoid the VAD from 
    // being deleted from under the pagefault
    //

    LeaveCriticalSection(&VADWriteLock);

    return dzStatus;

}



faultStatus
pageFault(void* virtualAddress, PTEpermissions RWEpermissions)
{

    PPTE currPTE;
    PTE oldPTE;
    faultStatus status;

    PRINT("[pageFault]");

    //
    // Get the current PTE from the VA param
    //

    currPTE = getPTE(virtualAddress);

    //
    // Check for invalid VA (not in range)
    //

    if (currPTE == NULL) {

        PRINT_ERROR(" VA not in range\n");
        return ACCESS_VIOLATION;

    }

    //
    // Acquire PTE lock (held through all save pf pagefault)
    //

    acquirePTELock(currPTE);

    //
    // Make a shallow copy/"snapshot" of the PTE to edit and check
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
        // If page changes state prior to PFN lock acquisition, retry fault
        //

        status = transPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

    }
    else if (oldPTE.u1.pfPTE.permissions != NO_ACCESS &&                // PAGEFILE STATE PTE
             oldPTE.u1.pfPTE.pageFileIndex != INVALID_BITARRAY_INDEX) { 

        //
        // Note: PTE is not held through - instead, it is released pre-filesystem read and re-acquired post read
        //

        status = pageFilePageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);  

    }
    else if (oldPTE.u1.dzPTE.permissions != NO_ACCESS) {                // DEMAND ZERO STATE PTE  

        status = demandZeroPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

    }
    else if (oldPTE.u1.dzPTE.decommitBit == 1) {

        PRINT("PTE has been decommitted\n");
        status = ACCESS_VIOLATION;

    }
    else if (oldPTE.u1.ulongPTE == 0) {                                 // ZERO STATE PTE

        status = checkVADPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

    }
    else {

        PRINT_ERROR("ERROR - not in any recognized PTE state\n");
        status = ACCESS_VIOLATION;

    }

    //
    // Release PTE lock
    //

    releasePTELock(currPTE);


    //
    // If a demand zero/pagefile fault failed due to lack of available pages, wait on new page events before 
    // return status and refaulting (but release PTE lock first so trimming/modified writing can occur)
    //

    if (status == NO_AVAILABLE_PAGES) {

        ULONG_PTR availablePages;
        availablePages = 0;

        //
        // Acquire all list locks to verify no pages are available
        //

        for (int i = 0; i < STANDBY + 1; i++) {

            EnterCriticalSection(&listHeads[i].lock);
            availablePages += listHeads[i].count;
            
        }

        if (availablePages == 0) {

            //
            // If all counts are zero, reset events and leave
            // critical sections
            //

            BOOL bRes;


            for (int i = 0; i < STANDBY + 1; i++) {

                bRes = ResetEvent(listHeads[i].newPagesEvent);

                if (bRes != TRUE) {
                    PRINT_ERROR("[dequeueLockedEvent] unable to reset event\n");
                }

                LeaveCriticalSection(&listHeads[i].lock);

            }

            HANDLE pageEventHandles[] = {zeroListHead.newPagesEvent, freeListHead.newPagesEvent, standbyListHead.newPagesEvent};

            WaitForMultipleObjects(STANDBY + 1, pageEventHandles, FALSE, INFINITE);


        } else {

            for (int i = STANDBY; i >= 0; i--) {


                LeaveCriticalSection(&listHeads[i].lock);

            }

        }

    }
    else if (status == PAGE_STATE_CHANGE) {

        PRINT("[pageFile] transition page has changed state\n");

        //
        // set status to SUCCESS so that caller will re-fault the PTE
        //

        status = SUCCESS;

    }
    
    return status;

}
