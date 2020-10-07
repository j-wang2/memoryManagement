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

    // check permissions
    PTEpermissions tempRWEpermissions = getPTEpermissions(snapPTE);
    if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

        PRINT_ERROR("Invalid permissions\n");
        return ACCESS_VIOLATION;

    } 
    
    // check for Write permissions - if write, set dirty bit and clear PF
    if (permissionMasks[RWEpermissions] & writeMask) {

        // set PTE to dirty
        snapPTE.u1.hPTE.dirtyBit = 1;

        // get PFN 
        PPFNdata PFN;
        PFN = PFNarray + snapPTE.u1.hPTE.PFN;

        // if the write in progress bit is clear, immediately free pf location 
        // and pf PFN pointer
        // if the write in progress bit is set, the bit index and pagefile field
        // are cleared by the smodified page writer once it finishes writing
        if (PFN->writeInProgressBit == 0) {

            // free PF location
            clearPFBitIndex(PFN->pageFileOffset);

            // clear pagefile pointer out of PFN
            PFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        }

        * (volatile PTE *) masterPTE = snapPTE;

    }

    PRINT("PFN is already valid\n");

    return SUCCESS;

}


faultStatus
transPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{


    PRINT(" - trans page fault\n");


    // initialize new PTE to be written
    PTE newPTE;
    newPTE.u1.ulongPTE = 0;


    // check permissions
    PTEpermissions transRWEpermissions = snapPTE.u1.tPTE.permissions;
    if (!checkPTEpermissions(transRWEpermissions, RWEpermissions)) {

        PRINT_ERROR("Invalid permissions\n");
        return ACCESS_VIOLATION;

    }


    // declare pageNum (PFN) ULONG and get page number from transition PTE
    ULONG_PTR pageNum;
    pageNum = snapPTE.u1.tPTE.PFN;


    // get PFN data from that pagenum
    PPFNdata transitionPFN;
    transitionPFN = PFNarray + pageNum;


    if (transitionPFN->statusBits != STANDBY && transitionPFN->statusBits != MODIFIED) {
        
        PRINT("[transPageFault] - page has changed state. Retrying\n");
        return PAGE_STATE_CHANGE;

    }


    // if attempting to write, set dirty bit & clear PF location if it exists
    if (permissionMasks[RWEpermissions] & writeMask) { 


        // set PTE to dirty
        newPTE.u1.hPTE.dirtyBit = 1;


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


    // copy permissions into our soon-to-be-active PTE from transition PTE
    transferPTEpermissions(&newPTE, transRWEpermissions);


    // only dequeue if write in progress bit is zero
    if (transitionPFN->writeInProgressBit == 0) {

        // dequeue from either standby or modified list
        dequeueSpecificPage(transitionPFN);

    }


    // & set page status to active
    transitionPFN->statusBits = ACTIVE;

    ULONG_PTR PTEindex;
    PTEindex = masterPTE - PTEarray;

    // set PFN PTE index
    transitionPFN->PTEindex = PTEindex;

    // put PFN into PTE
    newPTE.u1.hPTE.PFN = pageNum;

    // set PTE valid bit to 1
    newPTE.u1.hPTE.validBit = 1;

    // compiler writes out as indivisible store
    * (volatile PTE *) masterPTE = newPTE;

    // TODO - reduce lock duration/ increase multithreading granularity

    DWORD oldPermissions;

    // warning - "window" between MapUserPhysicalPages and VirtualProtect may result in a brief lack of permissions protection


    // assign VA to point at physical page, mirroring our local PTE change
    BOOL bResult;
    bResult = MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    if (bResult != TRUE) {

        PRINT("[pageFault] two mappings at one page, not an error\n");
        ASSERT(FALSE);

    }

    // update physical permissions of hardware PTE to match our software reference.
    VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[transRWEpermissions], &oldPermissions);

    return SUCCESS;

}

/*
* TODO (future): move to standby before active (read in progress)
* 
*/
faultStatus
pageFilePageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    PRINT(" - pf page fault\n");

    // declare pageNum (PFN) ULONG
    ULONG_PTR pageNum;

    PTE newPTE;                                    // copy of contents from tempPTE (use as "old" reference)
    newPTE.u1.ulongPTE = 0;


    // check permissions
    PTEpermissions pageFileRWEpermissions = snapPTE.u1.pfPTE.permissions;
    if (!checkPTEpermissions(pageFileRWEpermissions, RWEpermissions)) {

        PRINT_ERROR("Invalid permissions\n");
        return ACCESS_VIOLATION;

    }


    // dequeue a page of memory from freed list
    PPFNdata freedPFN;

    while (TRUE) {
        freedPFN = getPage();

        if (freedPFN == NULL) {
            // PRINT_ERROR("[pageFilePageFault] failed to successfully acquire PFN in getPage\n");

            // TODO - fix
            // HANDLE pageEventHandles[] = {&zeroListHead.newPagesEvent, &freeListHead.newPagesEvent, &standbyListHead.newPagesEvent};


            // WaitForMultipleObjects(3, pageEventHandles, TRUE, INFINITE);

            // return NO_FREE_PAGES;
            continue;
        } else {
            break;
        }
    }


    // setting status bits to standby (read in progress)
    freedPFN->statusBits = STANDBY;

    // get page number of the new page we/re allocating
    pageNum = freedPFN - PFNarray;


    BOOL bResult;
    bResult = FALSE;
    
    while (bResult != TRUE) {

        bResult = readPageFromFileSystem(pageNum, snapPTE.u1.pfPTE.pageFileIndex);

    }


    // PTELOCK
    EnterCriticalSection(&PTELock);
    
    ULONG_PTR PTEindex;
    PTEindex = masterPTE - PTEarray;


    // set PFN PTE index
    freedPFN->PTEindex = PTEindex;


    // set status to active
    freedPFN->statusBits = ACTIVE;


    // set hardware PTE to valid
    newPTE.u1.hPTE.validBit = 1;


    // if attempting to write, set dirty bit & clear PF location if it exists
    if (permissionMasks[RWEpermissions] & writeMask) {

        // set PTE to dirty
        newPTE.u1.hPTE.dirtyBit = 1;

        // free PF location 
        // TODO: if two threads clear near-simultaneously, the PFbit index could be 
        // used for another unrelated purpose and then the second thread would clear
        // data that is unrelated and unexpected
        clearPFBitIndex(snapPTE.u1.pfPTE.pageFileIndex);

        // clear pagefile pointer out of PFN
        freedPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

    }


    // transfer permissions bit from pfPTE, if it exists
    transferPTEpermissions(&newPTE, pageFileRWEpermissions);

    // put PFN's corresponding pageNum into PTE
    newPTE.u1.hPTE.PFN = pageNum;
    
    // compiler writes out as indivisible store
    // TODO - multithreading issue
    * (volatile PTE *) masterPTE = newPTE;

    DWORD oldPermissions;

    // warning - "window" between MapUserPhysicalPages and VirtualProtect may result in a lack of permissions protection

    // assign VA to point at physical page, mirroring our local PTE change
    // TODO - multithreading issue
    MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    // update physical permissions of hardware PTE to match our software reference.
    VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[pageFileRWEpermissions], &oldPermissions);

    //PTELOCK - CHECK
    LeaveCriticalSection(&PTELock);


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

    // dequeue a page of memory from freed list, setting status bits to active
    PPFNdata freedPFN;
    freedPFN = getPage();

    if (freedPFN == NULL) {
        PRINT_ERROR("[dzPageFault] failed to successfully acquire PFN in getPage\n");
        return NO_FREE_PAGES;
    }

    // set PFN status to active
    freedPFN->statusBits = ACTIVE;
    
    ULONG_PTR PTEindex;
    PTEindex = masterPTE - PTEarray;

    // set PFN PTE index
    freedPFN->PTEindex = PTEindex;

    // assign currPFN as calculated
    pageNum = freedPFN - PFNarray;
    newPTE.u1.hPTE.PFN = pageNum;

    // change PTE to validBit;
    newPTE.u1.hPTE.validBit = 1;

    // copy permissions from transition PTE into our soon-to-be-active PTE
    transferPTEpermissions(&newPTE, dZeroRWEpermissions);

    // compiler writes out as indivisible store
    * (volatile PTE *) masterPTE = newPTE;
    
    DWORD oldPermissions;

    // warning - "window" between MapUserPhysicalPages and VirtualProtect may result in a lack of permissions protection

    BOOLEAN bresult;

    // assign VA to point at physical page, mirroring our local PTE change
    bresult = (BOOLEAN) MapUserPhysicalPages(virtualAddress, 1, &pageNum);
    if (bresult != TRUE) {
        PRINT_ERROR("[dz PageFault] Error mapping user physical pages\n");
    }

    // update physical permissions of hardware PTE to match our software reference.
    bresult = (BOOLEAN) VirtualProtect(virtualAddress, PAGE_SIZE, windowsPermissions[dZeroRWEpermissions], &oldPermissions);
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

    PRINT("[pageFault]");

    // get the PTE from the VA
    PPTE currPTE;
    currPTE = getPTE(virtualAddress);

    // invalid VA (not in range)
    if (currPTE == NULL) {
        return ACCESS_VIOLATION;
    }
    
    // make a shallow copy/"snapshot" of the PTE to edit and check
    PTE oldPTE;

    faultStatus status;

    while (TRUE) {
        oldPTE = *currPTE;

        PPFNdata currPFN;

        if (oldPTE.u1.hPTE.validBit == 1) {                                 // VALID STATE PTE

            currPFN = PFNarray + oldPTE.u1.hPTE.PFN;

            acquireJLock(&currPFN->lockBits);

            // if another thread has since changed the PTE
            if (oldPTE.u1.ulongPTE != currPTE->u1.ulongPTE) {

                releaseJLock(&currPFN->lockBits);
                continue;

            }

            status = validPageFault(RWEpermissions, oldPTE, currPTE);

            releaseJLock(&currPFN->lockBits);

            return status;

        } 
        else if (oldPTE.u1.tPTE.transitionBit == 1) {                       // TRANSITION STATE PTE

            currPFN = PFNarray + oldPTE.u1.tPTE.PFN;

            acquireJLock(&currPFN->lockBits);

            // if another thread has since changed the PTE
            if (oldPTE.u1.ulongPTE != currPTE->u1.ulongPTE) {

                releaseJLock(&currPFN->lockBits);
                continue;

            }            

            status = transPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

            releaseJLock(&currPFN->lockBits);


            // if page has since changed status, resnap PTE and check again
            if (status == PAGE_STATE_CHANGE) {

                continue;

            }

            return status;

        }
        else if (oldPTE.u1.pfPTE.permissions != NO_ACCESS && oldPTE.u1.pfPTE.pageFileIndex != INVALID_PAGEFILE_INDEX) {          // PAGEFILE STATE PTE

            // TODO - still needs to be locked (to prevent other threads faulting OR decommitting)
            EnterCriticalSection(&PTELock);
            status = pageFilePageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);  

            LeaveCriticalSection(&PTELock);

            return status;


        }
        else if (oldPTE.u1.pfPTE.permissions != NO_ACCESS) {                        // DEMAND ZERO STATE PTE  

            // TODO - still needs to be locked (to prevent other threads faulting OR decommitting)
            EnterCriticalSection(&PTELock);

            status = demandZeroPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

            LeaveCriticalSection(&PTELock);

            return status;

            //LEFT OFF HERE


        }
        else if (oldPTE.u1.ulongPTE == 0) {                                 // ZERO STATE PTE

            // TODO (future): may need to check if vad is mem commit and bring in permissions
            // status = checkVADPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);

            PRINT("access violation - PTE is zero\n");
            return ACCESS_VIOLATION;

        }

        else {
            PRINT_ERROR("ERROR - not in any recognized PTE state\n");
            exit (-1);
        }

    }

    
}
