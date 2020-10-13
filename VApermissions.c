#include "userMode-AWE-pageFile.h"
#include "pageFault.h"
#include "pageFile.h"
#include "PTEpermissions.h"
#include "enqueue-dequeue.h"
#include "jLock.h"


faultStatus 
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{

    // initialize PFstatus to success - only changes on pagefault return val
    faultStatus PFstatus;
    PFstatus = SUCCESS;

    // while (PFstatus == SUCCESS || PFstatus == PAGE_STATE_CHANGE) {
    while (PFstatus == SUCCESS) {


    #ifdef TEMP_TESTING

        PPTE currPTE;
        currPTE = getPTE(virtualAddress);

        PTE snapPTE;
        snapPTE = *currPTE;

        if (snapPTE.u1.hPTE.validBit == 1) {

            ULONG_PTR currPFNindex;

            currPFNindex = snapPTE.u1.hPTE.PFN;

            PPFNdata currPFN;
            currPFN = PFNarray + currPFNindex;

            acquireJLock(&currPFN->lockBits);


            if (snapPTE.u1.ulongPTE == currPTE->u1.ulongPTE) {

                PTEpermissions tempRWEpermissions = getPTEpermissions(snapPTE);

                if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

                    releaseJLock(&currPFN->lockBits);
                    PFstatus = pageFault(virtualAddress, RWEpermissions);
                    PRINT_ERROR("Invalid permissions\n");
                    return PFstatus;

                } 

                releaseJLock(&currPFN->lockBits);
                return SUCCESS;

            }
            else {

                releaseJLock(&currPFN->lockBits);

                continue;
                
            }

        } else {

            PFstatus = pageFault(virtualAddress, RWEpermissions);

        }


    #else
        _try {

            if (RWEpermissions == READ_ONLY || READ_EXECUTE) {

                *(volatile CHAR *)virtualAddress;                                       // read

            } else if (RWEpermissions == READ_WRITE || READ_WRITE_EXECUTE) {

                *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress;    // write

            } else {

                PRINT_ERROR("invalid permissions\n");    

            }

            break;

        } _except (EXCEPTION_EXECUTE_HANDLER) {

            PFstatus = pageFault(virtualAddress, RWEpermissions);

        }

    #endif

    }

    return PFstatus;

}


faultStatus
writeVA(PVOID virtualAddress, PVOID str) 
{
    faultStatus PFstatus;
    PFstatus = accessVA(virtualAddress, READ_WRITE);

    // if accessing VA is successful, write parameter str to the VA
    if (PFstatus == SUCCESS) {

        * (PVOID *) virtualAddress = str;
        
    }

    return PFstatus;
    
}


faultStatus 
isVAaccessible (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{
    _try {
        if (RWEpermissions == READ_ONLY || RWEpermissions == READ_EXECUTE) {
            *(volatile CHAR *)virtualAddress;                                                      // read

        } else if (RWEpermissions == READ_WRITE || RWEpermissions == READ_WRITE_EXECUTE) {
            *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress;                   // write

        }  else {
            PRINT_ERROR("invalid permissions\n");
        }
        return SUCCESS;
    } _except (EXCEPTION_EXECUTE_HANDLER) {
        return ACCESS_VIOLATION;
    }
}


BOOLEAN
commitVA (PVOID startVA, PTEpermissions RWEpermissions, ULONG_PTR commitSize)
{

    PPTE startPTE;
    PVOID endVA;
    PPTE endPTE;
    PPTE currPTE;

    startPTE = getPTE(startVA);

    // inclusive (use <= as condition)
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    endPTE = getPTE(endVA);

    acquirePTELock(startPTE);

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        // invalid VA (not in range)
        if (currPTE == NULL) {

            releasePTELock(startPTE);
            PRINT_ERROR("[commitVA] Invalid PTE address - fatal error\n");
            return FALSE;
        }
        
        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;

        // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)
        if (tempPTE.u1.hPTE.validBit == 1 || tempPTE.u1.tPTE.transitionBit == 1 || tempPTE.u1.pfPTE.permissions != NO_ACCESS) {
            PRINT("[commitVA] PTE is already valid, transition, or demand zero\n");
            continue;
        }


        //
        // Below code to syncrhonize the increment of totalCommittedPages
        //

        LONG oldVal;
        LONG tempVal;

        oldVal = totalCommittedPages;

        while (TRUE) {

            ASSERT( oldVal <= totalMemoryPageLimit);

            if (oldVal == totalMemoryPageLimit) {

                // no remaining pages
                releasePTELock(startPTE);

                PRINT("[commitVA] All commit charge used (no remaining pages) - unable to commit PTE\n");
                return FALSE;

            }

            tempVal = InterlockedCompareExchange(&totalCommittedPages, oldVal + 1, oldVal);

            //
            // Compare exchange successful
            //

            if (tempVal == oldVal) {

                PVOID currVA;

                //
                // commit with priviliges param (commits PTE)
                //

                tempPTE.u1.dzPTE.permissions = RWEpermissions;

                tempPTE.u1.dzPTE.pageFileIndex = INVALID_PAGEFILE_INDEX;

                currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );  // equiv to PTEindex*page_size
                PRINT("[commitVA] Committed PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, RWEpermissions);

                break;

            } else {

                oldVal = tempVal;

            }

        }

        //
        // Write out newPTE contents indivisibly
        //

        * (volatile PTE *) currPTE = tempPTE;
            
    }

    releasePTELock(startPTE);

    return TRUE;

}


BOOLEAN
trimVA(void* virtualAddress)
{

    PRINT("[trimVA] trimming page with VA %llu\n", (ULONG_PTR) virtualAddress);

    PPTE PTEaddress;
    PTEaddress = getPTE(virtualAddress);

    if (PTEaddress == NULL) {
        PRINT_ERROR("could not trim VA %llu - no PTE associated with address\n", (ULONG_PTR) virtualAddress);
        return FALSE;
    }
    
    // take snapshot of old PTE
    PTE oldPTE;

    // acquire PTE lock
    acquirePTELock(PTEaddress);

    oldPTE = *PTEaddress;

    // check if PTE's valid bit is set - if not, can't be trimmed and return failure
    if (oldPTE.u1.hPTE.validBit == 0) {

        releasePTELock(PTEaddress);
        PRINT("could not trim VA %llu - PTE is not valid\n", (ULONG_PTR) virtualAddress);
        return FALSE;

    }

    // get pageNum
    ULONG_PTR pageNum;
    pageNum = oldPTE.u1.hPTE.PFN;

    // get PFN
    PPFNdata PFNtoTrim;
    PFNtoTrim = PFNarray + pageNum;


    ASSERT(PFNtoTrim->statusBits == ACTIVE);


    // zero new PTE
    PTE PTEtoTrim;
    PTEtoTrim.u1.ulongPTE = 0;


    // unmap page from VA (invalidates hardwarePTE)
    MapUserPhysicalPages(virtualAddress, 1, NULL);


    // acquire page lock (prior to viewing/editing PFN fields)
    acquireJLock(&PFNtoTrim->lockBits);


    // if write in progress bit is set, modified writer re-enqueues page
    if (PFNtoTrim->writeInProgressBit == 1) {

        if (oldPTE.u1.hPTE.dirtyBit == 0) {
            PFNtoTrim->statusBits = STANDBY;
        }

        else if (oldPTE.u1.hPTE.dirtyBit == 1) {
            
            // notify modified writer that page has been re-modified since initial write began
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

    releaseJLock(&PFNtoTrim->lockBits);


    // set transitionBit to 1
    PTEtoTrim.u1.tPTE.transitionBit = 1;  
    PTEtoTrim.u1.tPTE.PFN = pageNum;

    PTEtoTrim.u1.tPTE.permissions = getPTEpermissions(oldPTE);

    * (volatile PTE *) PTEaddress = PTEtoTrim;

    releasePTELock(PTEaddress);


    return TRUE;

}


BOOLEAN
protectVA(PVOID startVA, PTEpermissions newRWEpermissions, ULONG_PTR commitSize) {

    PPTE startPTE;
    PVOID endVA;
    PPTE endPTE;
    PPTE currPTE;

    startPTE = getPTE(startVA);

    // inclusive (use <= as condition)
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    endPTE = getPTE(endVA);
    
    acquirePTELock(startPTE);

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        // invalid VA (not in range)
        if (currPTE == NULL) {
            releasePTELock(startPTE);

            PRINT_ERROR("[protectVA] Invalid PTE address - fatal error\n")

            return FALSE;
        }
        
        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;

        PTEpermissions oldPermissions;

        if (tempPTE.u1.hPTE.validBit == 1) {

            oldPermissions = getPTEpermissions(tempPTE);
            transferPTEpermissions(&tempPTE, newRWEpermissions);

        }
        else if ( tempPTE.u1.tPTE.transitionBit == 1 ) {

            oldPermissions = tempPTE.u1.tPTE.permissions;
            tempPTE.u1.tPTE.permissions = newRWEpermissions;

        }
        else if ( tempPTE.u1.pfPTE.permissions != NO_ACCESS) {      // handles both pfPTE and dzPTE formats (since are identical in format)
            
            oldPermissions = tempPTE.u1.pfPTE.permissions;
            tempPTE.u1.pfPTE.permissions = newRWEpermissions;

        }
        else {

            PRINT("[protectVA] PTE is not already valid, transition, or demand zero\n");
            continue;

        }


        PVOID currVA;
        currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );  // equiv to PTEindex*page_size

        PRINT("[protectVA] Updated permissions for PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, newRWEpermissions);

        * (volatile PTE *) currPTE = tempPTE;
                
    }
    releasePTELock(startPTE);

    return TRUE;

}


BOOLEAN
decommitVA (PVOID startVA, ULONG_PTR commitSize) {


    PPTE startPTE;
    PVOID endVA;
    PPTE endPTE;
    PPTE currPTE;
    PVOID currVA;

    startPTE = getPTE(startVA);

    // inclusive (use <= as condition)
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    endPTE = getPTE(endVA);

    acquirePTELock(startPTE);

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {


        if (currPTE == NULL) {
            releasePTELock(startPTE);

            PRINT_ERROR("[decommitVA] Invalid PTE address - fatal error\n")

            return FALSE;
        }


        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;


        currVA = (PVOID) ( (ULONG_PTR) startVA + ( (currPTE - startPTE) << PAGE_SHIFT ) );  // equiv to PTEindex*page_size


        // check if PTE is already zeroed
        if (tempPTE.u1.ulongPTE == 0) {

            PRINT("VA is already decommitted\n");
            continue;

        }

        // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)

        else if (tempPTE.u1.hPTE.validBit == 1) {                       // valid/hardware format
        

            // get PFN
            PPFNdata currPFN;
            currPFN = PFNarray + tempPTE.u1.hPTE.PFN;

            // acquire lock and check if PTE has changed
            acquireJLock(&currPFN->lockBits);

            if (tempPTE.u1.ulongPTE != currPTE->u1.ulongPTE) {

                // if PTE has changed, release lock, set currPTE back one, and continue (so that it rereads)
                releaseJLock(&currPFN->lockBits);
                currPTE--;
                PRINT("[decommitVA] PTE has since been changed from active state\n");

                continue;

            }

            #ifdef TESTING_VERIFY_ADDRESSES
            if (!(ULONG_PTR) currVA == * (ULONG_PTR*) currVA) {
                
                PRINT_ERROR("decommitting (VA = 0x%llx) with contents 0x%llx\n", (ULONG_PTR) currVA, * (ULONG_PTR*) currVA);
                
            }
            #endif

            PRINT_ALWAYS("decommitting (VA = 0x%llx) with contents 0x%llx\n", (ULONG_PTR) currVA, * (ULONG_PTR*) currVA);
            // PRINT("decommitting (VA = 0x%llx) with contents %s\n", (ULONG_PTR) currVA, * (PCHAR *) currVA);


            // unmap VA from page
            BOOL bResult;
            bResult = MapUserPhysicalPages(startVA, 1, NULL);

            if (bResult != TRUE) {
                PRINT_ERROR("[decommitVA] unable to decommit VA %llx\n", (ULONG_PTR) currVA);
            }


            if (currPFN->writeInProgressBit == 1) {

                currPFN->statusBits = AWAITING_FREE;
                
            }
            else {
                // if the PFN contents is also stored in pageFile
                if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
                    clearPFBitIndex(currPFN->pageFileOffset);
                }

                // enqueue Page to free list
                enqueuePage(&freeListHead, currPFN);
            }

            releaseJLock(&currPFN->lockBits);



        } 
        else if (tempPTE.u1.tPTE.transitionBit == 1) {                  // transition format

            // get PFN
            PPFNdata currPFN;
            currPFN = PFNarray + tempPTE.u1.hPTE.PFN;


            acquireJLock(&currPFN->lockBits);

            // verify is still transition and pointed to by PTE index
            if ( tempPTE.u1.ulongPTE != currPTE->u1.ulongPTE
            || (currPFN->statusBits != STANDBY && currPFN->statusBits != MODIFIED)
            || currPFN->PTEindex != (ULONG64) (currPTE - PTEarray) ) {
                
                releaseJLock(&currPFN->lockBits);
                PRINT("[decommitVA] currPFN has changed from transition state\n");

                // reprocess same PTE since it has since been changed
                currPTE--;
                continue;

            }
            
            //
            // only dequeue if both read in progress and write in progress bits are zero
            //

            if (currPFN->writeInProgressBit == 1 || currPFN->readInProgressBit == 1) {

                currPFN->statusBits = AWAITING_FREE;

            }
            else { 
                // dequeue from standby/modified list
                dequeueSpecificPage(currPFN);

                // if the PFN contents are also stored in pageFile
                if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
                    clearPFBitIndex(currPFN->pageFileOffset);
                }

                // enqueue Page to free list (setting status bits in process)
                enqueuePage(&freeListHead, currPFN);
            }
           

            releaseJLock(&currPFN->lockBits);

        }  
        else if (tempPTE.u1.pfPTE.pageFileIndex != INVALID_PAGEFILE_INDEX) {     // pagefile format

            clearPFBitIndex(tempPTE.u1.pfPTE.pageFileIndex);
            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.pfPTE.permissions != NO_ACCESS) {                   // demand zero format

            tempPTE.u1.ulongPTE = 0;

        }
        else if (tempPTE.u1.ulongPTE == 0) {                            // zero PTE
            
            releasePTELock(startPTE);
            PRINT_ERROR("[decommitVA] already decommitted\n");
            return TRUE;
            
        }

        // decrement count of committed pages
        if (totalCommittedPages > 0)  {

            InterlockedDecrement(&totalCommittedPages);

        } else {

            releasePTELock(startPTE);
            PRINT_ERROR("[decommitVA] bookkeeping error - no committed pages\n");
            return FALSE;

        }

        // zero and write PTE out
        memset(&tempPTE, 0, sizeof(PTE));

        * (volatile PTE *) currPTE = tempPTE;
    }

    releasePTELock(startPTE);

    return TRUE;

}

