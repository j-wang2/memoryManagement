#include "userMode-AWE-pageFile.h"

faultStatus 
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{
    // initialize PFstatus to success - only changes on pagefault return val
    faultStatus PFstatus;
    PFstatus = SUCCESS;

    while (PFstatus == SUCCESS) {

        _try {

            if (RWEpermissions == READ_ONLY || READ_EXECUTE) {

                *(volatile CHAR *)virtualAddress;                                       // read

            } else if (RWEpermissions == READ_WRITE || READ_WRITE_EXECUTE) {

                *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress;    // write

            } else {

                fprintf(stderr, "invalid permissions\n");    

            }

            break;

        } _except (EXCEPTION_EXECUTE_HANDLER) {

            PFstatus = pageFault(virtualAddress, RWEpermissions);

        }

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
            fprintf(stderr, "invalid permissions\n");
        }
        return SUCCESS;
    } _except (EXCEPTION_EXECUTE_HANDLER) {
        return ACCESS_VIOLATION;
    }
}


BOOLEAN
commitVA (PVOID startVA, PTEpermissions RWEpermissions, ULONG_PTR commitSize)
{

    // get the PTE from the VA
    PPTE currPTE;
    currPTE = getPTE(startVA);

    // invalid VA (not in range)
    if (currPTE == NULL) {
        return FALSE;
    }
    
    // make a shallow copy/"snapshot" of the PTE to edit and check
    PTE tempPTE;
    tempPTE = *currPTE;

    // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)
    if (tempPTE.u1.hPTE.validBit == 1 || tempPTE.u1.tPTE.transitionBit == 1 || tempPTE.u1.pfPTE.permissions != NO_ACCESS) {
        printf("PTE is already valid, transition, or demand zero\n");
        return FALSE;
    }

    if (totalCommittedPages < totalMemoryPageLimit) {

        // commit with priviliges param
        tempPTE.u1.dzPTE.permissions = RWEpermissions;

        // set demand zero bit (commit)
        tempPTE.u1.dzPTE.pageFileBit = 1;

        tempPTE.u1.dzPTE.pageFileIndex = INVALID_PAGEFILE_INDEX;
        totalCommittedPages++;
        printf("Committed VA at %d with permissions %d\n", (ULONG) startVA, RWEpermissions);
    
    } else {
        // no remaining pages
        fprintf(stderr, "no remaining pages\n");
        return FALSE;
    }

    * (volatile PTE *) currPTE = tempPTE;
    return TRUE;
}


PTEpermissions
protectVA(PVOID startVA, PTEpermissions newRWEpermissions, ULONG_PTR commitSize) {
        
    // get the PTE from the VA
    PPTE currPTE;
    currPTE = getPTE(startVA);

    // invalid VA (not in range)
    if (currPTE == NULL) {
        return NO_ACCESS;
    }
    
    // make a shallow copy/"snapshot" of the PTE to edit and check
    PTE tempPTE;
    tempPTE = *currPTE;

    PTEpermissions oldPermissions;

    // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)
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

        printf("PTE is not already valid, transition, or demand zero\n");
        return NO_ACCESS;

    }

    printf("[protectVA] updated VA permissions\n");

    * (volatile PTE *) currPTE = tempPTE;
    return oldPermissions;

}


BOOLEAN
decommitVA (PVOID startVA, ULONG_PTR commitSize) {
    PPTE currPTE;
    currPTE = getPTE(startVA);

    if (currPTE == NULL) {
        return FALSE;
    }

    // make a shallow copy/"snapshot" of the PTE to edit and check
    PTE tempPTE;
    tempPTE = *currPTE;

    // check if PTE is already zeroed
    if (tempPTE.u1.ulongPTE == 0) {
        fprintf(stderr, "VA is already decommitted\n");
        return FALSE;
    }

    // check if valid/transition/demandzero bit  is already set (avoids double charging if transition)

    else if (tempPTE.u1.hPTE.validBit == 1) {                       // valid/hardware format

        // get PFN
        PPFNdata currPFN;
        currPFN = PFNarray + tempPTE.u1.hPTE.PFN;

        // unmap VA from page
        MapUserPhysicalPages(startVA, 1, NULL);

        // if the PFN contents is also stored in pageFile
        if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
            clearPFBitIndex(currPFN->pageFileOffset);
        }

        // enqueue Page to free list
        enqueuePage(&freeListHead, currPFN);

    } 
    else if (tempPTE.u1.tPTE.transitionBit == 1) {                  // transition format

        // get PFN
        PPFNdata currPFN;
        currPFN = PFNarray + tempPTE.u1.hPTE.PFN;


        // dequeue from standby/modified list
        dequeueSpecificPage(currPFN);
        
        // if the PFN contents is also stored in pageFile
        if (currPFN->pageFileOffset != INVALID_PAGEFILE_INDEX ) {
            clearPFBitIndex(currPFN->pageFileOffset);
        }

        // enqueue Page to free list
        enqueuePage(&freeListHead, currPFN);

    }  
    else if (tempPTE.u1.pfPTE.pageFileIndex < PAGEFILE_PAGES) {     // pagefile format

        clearPFBitIndex(tempPTE.u1.pfPTE.pageFileIndex);
        tempPTE.u1.ulongPTE = 0;

    }
    else if (tempPTE.u1.pfPTE.pageFileBit == 1) {                   // demand zero format

        tempPTE.u1.ulongPTE = 0;

    }

    else if (tempPTE.u1.ulongPTE == 0) {                            // zero PTE
        fprintf(stderr, "already decommitted\n");
        return TRUE;
    }

    // decrement count of committed pages
    if (totalCommittedPages != 0)  {

        totalCommittedPages--;

    } else {

        fprintf(stderr, "error - no committed pages\n");
        return FALSE;

    }

    memset(&tempPTE, 0, sizeof(PTE));

    * (volatile PTE *) currPTE = tempPTE;
    return TRUE;
}

