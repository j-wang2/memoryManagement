#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h" 
#include "getPage.h"


faultStatus
validPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    // check permissions
    PTEpermissions tempRWEpermissions = getPTEpermissions(snapPTE);
    if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

        fprintf(stderr, "Invalid permissions\n");
        return ACCESS_VIOLATION;

    } 
    
    // check for Write permissions - if write, set dirty bit and clear PF
    else if (permissionMasks[RWEpermissions] & writeMask) {

        // set PTE to dirty
        snapPTE.u1.hPTE.dirtyBit = 1;

        // get PFN 
        PPFNdata PFN;
        PFN = PFNarray + snapPTE.u1.hPTE.PFN;

        // free PF location
        clearPFBitIndex(PFN->pageFileOffset);

        // clear pagefile pointer out of PFN
        PFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        * (volatile PTE *) masterPTE = snapPTE;

    }
    printf("PFN is already valid\n");
    return SUCCESS;

}


faultStatus
transPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{
    printf(" - trans page fault\n");

    // declare pageNum (PFN) ULONG
    ULONG_PTR pageNum;

    PTE newPTE;
    newPTE.u1.ulongPTE = 0;

    // check permissions
    PTEpermissions transRWEpermissions = snapPTE.u1.tPTE.permissions;
    if (!checkPTEpermissions(transRWEpermissions, RWEpermissions)) {

        fprintf(stderr, "Invalid permissions\n");
        return ACCESS_VIOLATION;

    } 

    pageNum = snapPTE.u1.tPTE.PFN;

    PPFNdata transitionPFN;
    transitionPFN = PFNarray + pageNum;

    if (permissionMasks[RWEpermissions] & writeMask) { // if attempting to write, set dirty bit & clear PF location if it exists

        // set PTE to dirty
        newPTE.u1.hPTE.dirtyBit = 1;

        // free PF location
        clearPFBitIndex(transitionPFN->pageFileOffset);

        // clear pagefile pointer out of PFN
        transitionPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

    }

    // copy permissions from transition PTE into our soon-to-be-active PTE
    transferPTEpermissions(&newPTE, transRWEpermissions);

    // dequeue from either standby or modified list
    dequeueSpecificPage(transitionPFN);

    // & set status to active
    transitionPFN->statusBits = ACTIVE;

    ULONG_PTR PTEindex;
    PTEindex = masterPTE - PTEarray;

    // set PFN PTE index
    transitionPFN->PTEindex = PTEindex;

    // put PFN into PTE
    newPTE.u1.hPTE.PFN = pageNum;

    // set PTE valid bit to 1
    newPTE.u1.hPTE.validBit = 1;

    // compiler writes out as indivisible store             // TODO UPDATE OTHERS (ORDER OF INDIVISIBLE STORE AND MAPUSER)
    * (volatile PTE *) masterPTE = newPTE;

    // assign VA to point at physical page, mirroring our local PTE change
    MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    return SUCCESS;

}


faultStatus
pageFilePageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{

    printf(" - pf page fault\n");

    // declare pageNum (PFN) ULONG
    ULONG_PTR pageNum;

    PTE newPTE;                                    // copy of contents from tempPTE (use as "old" reference)
    newPTE.u1.ulongPTE = 0;


    // check permissions
    PTEpermissions pageFileRWEpermissions = snapPTE.u1.pfPTE.permissions;
    if (!checkPTEpermissions(pageFileRWEpermissions, RWEpermissions)) {

        fprintf(stderr, "Invalid permissions\n");
        return ACCESS_VIOLATION;

    }


    // dequeue a page of memory from freed list
    PPFNdata freedPFN;
    freedPFN = getPage();
    if (freedPFN == NULL) {
        fprintf(stderr, "failed to successfully dequeue PFN from freed list\n");
        return NO_FREE_PAGES;
    }

    // setting status bits to active
    freedPFN->statusBits = ACTIVE;

    // get page number of the new page we/re allocating
    pageNum = freedPFN - PFNarray;

    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(pageFileFormatVA, 1, &pageNum)) {
        fprintf(stderr, "error remapping pageFileFormatVA\n");
        return FALSE;
    }

    // get PFsourceVA from the pageFileIndex
    PVOID PFsourceVA;
    PFsourceVA = (PVOID) ( (ULONG_PTR) pageFileVABlock + (PAGE_SIZE * snapPTE.u1.pfPTE.pageFileIndex) );
    
    // copy contents from pagefile to our new page (via pageFileFormatVA)
    memcpy(pageFileFormatVA, PFsourceVA, PAGE_SIZE);

    // unmap pageFileFormatVA from page - PFN is now filled w contents from pagefile
    if (!MapUserPhysicalPages(pageFileFormatVA, 1, NULL)) {
        fprintf(stderr, "error copying into page\n");
        return FALSE;
    }
    
    ULONG_PTR PTEindex;
    PTEindex = masterPTE - PTEarray;

    // set PFN PTE index
    freedPFN->PTEindex = PTEindex;

    // set hardware PTE to valid
    newPTE.u1.hPTE.validBit = 1;

    if (permissionMasks[RWEpermissions] & writeMask) { // if attempting to write, set dirty bit & clear PF location if it exists

        // set PTE to dirty
        newPTE.u1.hPTE.dirtyBit = 1;

        // free PF location
        clearPFBitIndex(snapPTE.u1.pfPTE.pageFileIndex);

        // clear pagefile pointer out of PFN
        freedPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

    }


    // transfer permissions bit from pfPTE, if it exists
    transferPTEpermissions(&newPTE, pageFileRWEpermissions);

    // put PFN's corresponding pageNum into PTE
    newPTE.u1.hPTE.PFN = pageNum;
    
    // assign VA to point at physical page, mirroring our local PTE change              TODO CHECK
    MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    // compiler writes out as indivisible store
    * (volatile PTE *) masterPTE = newPTE;

    return SUCCESS;

}


faultStatus
demandZeroPageFault(void* virtualAddress, PTEpermissions RWEpermissions, PTE snapPTE, PPTE masterPTE)
{
    printf(" - dz page fault\n");

    // declare pageNum (PFN) ULONG
    ULONG_PTR pageNum;

    PTE newPTE;
    newPTE.u1.ulongPTE = 0;

    // check permissions
    PTEpermissions dZeroRWEpermissions = snapPTE.u1.dzPTE.permissions;
    if (!checkPTEpermissions(dZeroRWEpermissions, RWEpermissions)) {

        fprintf(stderr, "Invalid permissions\n");
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
        fprintf(stderr, "failed to successfully dequeue PFN from freed list\n");
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

    MapUserPhysicalPages(virtualAddress, 1, &pageNum);

    return SUCCESS;                                             // return value of 2; 

}


faultStatus
pageFault(void* virtualAddress, PTEpermissions RWEpermissions)
{

    printf("pageFault\n");

    // get the PTE from the VA
    PPTE currPTE;
    currPTE = getPTE(virtualAddress);

    // invalid VA (not in range)
    if (currPTE == NULL) {
        return ACCESS_VIOLATION;
    }
    
    // make a shallow copy/"snapshot" of the PTE to edit and check
    PTE oldPTE;
    oldPTE = *currPTE;

    faultStatus status;
    if (oldPTE.u1.hPTE.validBit == 1) {                                // VALID STATE PTE

        status = validPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);
        return status;

    } 
    else if (oldPTE.u1.tPTE.transitionBit == 1) {                       // TRANSITION STATE PTE

        // todo - set equal to a fault status that is returned
        status = transPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);
        return status;

    }
    else if (oldPTE.u1.pfPTE.pageFileIndex < PAGEFILE_PAGES) {   // PAGEFILE STATE PTE

        status = pageFilePageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);  
        return status;

    }
    else if (oldPTE.u1.pfPTE.pageFileBit == 1) {                // DEMAND ZERO STATE PTE  

        status = demandZeroPageFault(virtualAddress, RWEpermissions, oldPTE, currPTE);
        return status;

    }
    else if (oldPTE.u1.ulongPTE == 0) {                          // ZERO STATE PTE
        // TODO - need to check if vad is mem commit and bring in permissions if so
        fprintf(stderr, "access violation - PTE is zero\n");
        return ACCESS_VIOLATION;

    }

    else {
        fprintf(stderr, "ERROR - not in any recognized PTE state\n");
        exit (-1);
    }
    
}
