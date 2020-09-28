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
    startPTE = getPTE(startVA);

    // inclusive (use <= as condition)
    PVOID endVA;
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    PPTE endPTE;
    endPTE = getPTE(endVA);
    
    PPTE currPTE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        // invalid VA (not in range)
        if (currPTE == NULL) {
            PRINT_ERROR("[commitVA] Invalid PTE address - fatal error\n")
            return FALSE;
        }
        
        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;

        // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)
        if (tempPTE.u1.hPTE.validBit == 1 || tempPTE.u1.tPTE.transitionBit == 1 || tempPTE.u1.pfPTE.permissions != NO_ACCESS) {
            PRINT("[commitVA] PTE is already valid, transition, or demand zero\n");
            // return FALSE;
            continue;
        }

        if (totalCommittedPages < totalMemoryPageLimit) {

            // commit with priviliges param (commits PTE)
            tempPTE.u1.dzPTE.permissions = RWEpermissions;

            tempPTE.u1.dzPTE.pageFileIndex = INVALID_PAGEFILE_INDEX;
            totalCommittedPages++;

            PVOID currVA;
            currVA = (PVOID) ( (ULONG_PTR) startVA + PAGE_SIZE* (currPTE - startPTE) );
            PRINT("[commitVA] Committed PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, RWEpermissions);

        
        } else {
            // no remaining pages
            PRINT("[commitVA] no remaining pages - unable to commit PTE\n");
            return FALSE;
        }

        * (volatile PTE *) currPTE = tempPTE;
            
    }
    return TRUE;

}


BOOLEAN
protectVA(PVOID startVA, PTEpermissions newRWEpermissions, ULONG_PTR commitSize) {

    PPTE startPTE;
    startPTE = getPTE(startVA);

    // inclusive (use <= as condition)
    PVOID endVA;
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    PPTE endPTE;
    endPTE = getPTE(endVA);
    
    PPTE currPTE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        // invalid VA (not in range)
        if (currPTE == NULL) {
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
        currVA = (PVOID) ( (ULONG_PTR) startVA + PAGE_SIZE* (currPTE - startPTE) );
        PRINT("[protectVA] Updated permissions for PTE at VA %llx with permissions %d\n", (ULONG_PTR) currVA, newRWEpermissions);

        * (volatile PTE *) currPTE = tempPTE;
                
    }

    return TRUE;

}


BOOLEAN
decommitVA (PVOID startVA, ULONG_PTR commitSize) {


    PPTE startPTE;
    startPTE = getPTE(startVA);

    // inclusive (use <= as condition)
    PVOID endVA;
    endVA = (PVOID) ((ULONG_PTR) startVA + commitSize - 1);

    PPTE endPTE;
    endPTE = getPTE(endVA);
    
    PPTE currPTE;

    // temp fix - TODO. Limits to a single decommit at a given time.
    endPTE = startPTE;

    for (currPTE = startPTE; currPTE <= endPTE; currPTE++ ) {

        if (currPTE == NULL) {
            PRINT_ERROR("[decommitVA] Invalid PTE address - fatal error\n")
            return FALSE;
        }

        // make a shallow copy/"snapshot" of the PTE to edit and check
        PTE tempPTE;
        tempPTE = *currPTE;

        // check if PTE is already zeroed
        if (tempPTE.u1.ulongPTE == 0) {
            PRINT("VA is already decommitted\n");
            continue;
        }

        // PVOID currVA;
        // currVA = (PVOID) ( (ULONG_PTR) startVA + PAGE_SIZE* (currPTE - startPTE) );
        // PRINT("decommiting (VA = 0x%llx) with contents 0x%llx\n", (ULONG_PTR) currVA, * (ULONG_PTR*) currVA);
        // PRINT("decommiting (VA = 0x%llx) with contents %s\n", (ULONG_PTR) currVA, * (PCHAR *) currVA);


        // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)

        else if (tempPTE.u1.hPTE.validBit == 1) {                       // valid/hardware format

            // get PFN
            PPFNdata currPFN;
            currPFN = PFNarray + tempPTE.u1.hPTE.PFN;

            // unmap VA from page
            MapUserPhysicalPages(startVA, 1, NULL);

            if (currPFN->writeInProgressBit == 1) {

                currPFN->statusBits = AWAITING_FREE;
                
            }
            else {
                // if the PFN contents is also stored in pageFile
                if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
                    clearPFBitIndex(currPFN->pageFileOffset);
                }

                // enqueue Page to free list
                enqueuePage(&freeListHead, currPFN);        // TODO 
            }



        } 
        else if (tempPTE.u1.tPTE.transitionBit == 1) {                  // transition format

            // get PFN
            PPFNdata currPFN;
            currPFN = PFNarray + tempPTE.u1.hPTE.PFN;


            acquireJLock(&currPFN->lockBits);

            // verify is still transition and pointed to by PTE index
            if ( (currPFN->statusBits != STANDBY && currPFN->statusBits != MODIFIED)
            || currPFN->PTEindex != (ULONG64) (currPTE - PTEarray) ) {
                
                releaseJLock(&currPFN->lockBits);
                PRINT("[decommitVA] currPFN has changed\n");

                // reprocess same PTE since it has since been changed
                currPTE--;
                continue;

            }
            
            // only dequeue if write in progress bit is zero

            if (currPFN->writeInProgressBit == 1) {

                currPFN->statusBits = AWAITING_FREE;

            }
            else { 
                // dequeue from standby/modified list
                dequeueSpecificPage(currPFN);

                // if the PFN contents are also stored in pageFile
                if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
                    clearPFBitIndex(currPFN->pageFileOffset);
                }
            }
           
            


            
            // enqueue Page to free list (setting status bits in process)
            enqueuePage(&freeListHead, currPFN);

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
            PRINT_ERROR("[decommitVA] already decommitted\n");
            return TRUE;
        }

        // decrement count of committed pages
        if (totalCommittedPages > 0)  {

            totalCommittedPages--;

        } else {

            PRINT_ERROR("[decommitVA] bookkeeping error - no committed pages\n");
            return FALSE;

        }

        memset(&tempPTE, 0, sizeof(PTE));

        * (volatile PTE *) currPTE = tempPTE;
    }
    return TRUE;

}

