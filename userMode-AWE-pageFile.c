/*
 * Usermode virtual memory program intended as a rudimentary simulation
 * of Windows OS kernelmode memory management
 * 
 * Jason Wang, August 2020
 */

#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
#include "pageFile.h"


/******* GLOBALS *****/
void* leafVABlock;                  // starting address of memory block
void* leafVABlockEnd;               // ending address of memory block

PPFNdata PFNarray;                  // starting address of PFN metadata array
PPTE PTEarray;                      // starting address of page table

void* zeroVA;                       // specific VA used for zeroing PFNs (via AWE mapping)

ULONG_PTR totalCommittedPages;      // count of committed pages (initialized to zero)
ULONG_PTR totalMemoryPageLimit = NUM_PAGES + PAGEFILE_SIZE / PAGE_SIZE;    // limit of committed pages (memory block + pagefile space)

void* pageFileVABlock;              // starting address of pagefile "disk" (memory)
void* modifiedWriteVA;              // specific VA used for writing out page contents to pagefile
void* pageFileFormatVA;             // specific VA used for copying in page contents from pagefile
ULONG_PTR pageFileBitArray[PAGEFILE_PAGES/(8*sizeof(ULONG_PTR))];

// Execute-Write-Read (bit ordering)
ULONG_PTR permissionMasks[] = { 0, readMask, (readMask | writeMask), (readMask | executeMask), (readMask | writeMask | executeMask)};

listData listHeads[ACTIVE];         // intialization of listHeads array

/*
 * getPTEpermissions: function to convert input PTE's permissions (as separate bits) into a return PTEpermissions (3 consecutive bits)
 * 
 * 
 * 
 */
PTEpermissions
getPTEpermissions(PTE curr)
{
    if (curr.u1.hPTE.validBit == 1) {
        if (curr.u1.hPTE.writeBit && curr.u1.hPTE.executeBit) {
            return READ_WRITE_EXECUTE;
        } else if (curr.u1.hPTE.writeBit) {
            return READ_WRITE;
        } else if (curr.u1.hPTE.executeBit) {
            return READ_EXECUTE;
        } else {
            return READ_ONLY;
        }
    } else {
        fprintf(stderr, "getPTEpermissions called on an invalid PTE\n");
        return NO_ACCESS;
    }
}


/*
 * transferPTEpermissions: function to convert input PTEpermissions into destionation (valid) PTE separate bits
 * 
 * 
 */
VOID
transferPTEpermissions(PPTE activeDest, PTEpermissions sourceP)
{
    printf("%d\n", activeDest->u1.ulongPTE);
    printf("%d\n", sourceP);

    if (permissionMasks[sourceP] & writeMask) {
        activeDest->u1.hPTE.writeBit = 1;
    } else {
        activeDest->u1.hPTE.writeBit = 0;
    }
    printf("%d\n", activeDest->u1.ulongPTE);

    if (permissionMasks[sourceP] & executeMask) {
        activeDest->u1.hPTE.executeBit = 1;
    } else {
        activeDest->u1.hPTE.executeBit = 0;
    }
    printf("%d\n", activeDest->u1.ulongPTE);

    // if (permissionMasks[sourceP] & readMask) {

    // }
}

/*
 * checkPTEpermissions: function to check current PTE permissions with requested permissions
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
checkPTEpermissions(PTEpermissions currP, PTEpermissions checkP) 
{

    // printf("currP: %d")

    // get mask for permissions to check and logical and it with current mask
    if ( (permissionMasks[checkP] & permissionMasks[currP] ) != permissionMasks[checkP]) {
        return FALSE;
    }
    return TRUE;
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
    PTE tempPTE;
    tempPTE = *currPTE;

    // declare pageNum (PFN) ULONG
    ULONG_PTR pageNum;
    
    if (tempPTE.u1.hPTE.validBit == 1) {                                // VALID STATE PTE

        // check permissions
        PTEpermissions tempRWEpermissions = getPTEpermissions(tempPTE);
        if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

            fprintf(stderr, "Invalid permissions\n");
            return ACCESS_VIOLATION;

        } 
        
        // check for Write permissions - if write, set dirty bit and clear PF
        else if (permissionMasks[RWEpermissions] & writeMask) {

            // set PTE to dirty
            tempPTE.u1.hPTE.dirtyBit = 1;

            // get PFN 
            PPFNdata PFN;
            PFN = PFNarray + tempPTE.u1.hPTE.PFN;

            // free PF location
            clearPFBitIndex(PFN->pageFileOffset);

            // clear pagefile pointer out of PFN
            PFN->pageFileOffset = MAXULONG_PTR;

            * (volatile PTE *) currPTE = tempPTE;

        }
        printf("PFN is already valid\n");
        return SUCCESS;

    } 
    else if (tempPTE.u1.tPTE.transitionBit == 1) {                       // TRANSITION STATE PTE

        PTE transPTE;
        transPTE.u1.ulongPTE = tempPTE.u1.ulongPTE;

        tempPTE.u1.ulongPTE = 0;
        printf("about to check permissions\n");

        // check permissions
        PTEpermissions transRWEpermissions = transPTE.u1.tPTE.permissions;
        if (!checkPTEpermissions(transRWEpermissions, RWEpermissions)) {

            fprintf(stderr, "Invalid permissions\n");
            return ACCESS_VIOLATION;

        } 

        pageNum = transPTE.u1.hPTE.PFN;

        PPFNdata transitionPFN;
        transitionPFN = PFNarray + pageNum;

        if (permissionMasks[RWEpermissions] & writeMask) { // if attempting to write, set dirty bit & clear PF location if it exists

            // set PTE to dirty
            tempPTE.u1.hPTE.dirtyBit = 1;

            // free PF location
            clearPFBitIndex(transitionPFN->pageFileOffset);

            // clear pagefile pointer out of PFN
            transitionPFN->pageFileOffset = MAXULONG_PTR;

        }

        // copy permissions from transition PTE into our soon-to-be-active PTE
        transferPTEpermissions(&tempPTE, transRWEpermissions);

        printf("done transferring permissions to PTE %d\n", (INT) tempPTE.u1.ulongPTE);

        // dequeue from either standby or modified list
        dequeueSpecificPage(transitionPFN);

        // & set status to active
        transitionPFN->statusBits = ACTIVE;

        // set PTE valid bit to 1
        tempPTE.u1.hPTE.validBit = 1;
        tempPTE.u1.tPTE.transitionBit = 0;


        // compiler writes out as indivisible store             // TODO UPDATE OTHERS (ORDER OF INDIVISIBLE STORE AND MAPUSER)
        * (volatile PTE *) currPTE = tempPTE;

        // assign VA to point at physical page, mirroring our local PTE change
        MapUserPhysicalPages(virtualAddress, 1, &pageNum);

        return SUCCESS;

    }
    else if (tempPTE.u1.pfPTE.pageFileIndex < PAGEFILE_PAGES) {   // PAGEFILE STATE PTE
        printf("pagefile PF\n");

        // TODO: DO I NEED TO TRANSFER TRANSITION/PAGEFILE PERMISSIONS TO VALID
        PTE pageFilePTE;                                    // copy of contents from tempPTE (use as "old" reference)
        pageFilePTE.u1.ulongPTE = tempPTE.u1.ulongPTE;

        tempPTE.u1.ulongPTE = 0;                            // zero PTE to be copied in
        printf("about to check permissions\n");

        // check permissions
        PTEpermissions pageFileRWEpermissions = pageFilePTE.u1.tPTE.permissions;
        if (!checkPTEpermissions(pageFileRWEpermissions, RWEpermissions)) {

            fprintf(stderr, "Invalid permissions\n");
            return ACCESS_VIOLATION;

        }

        // pull in from pagefile and validate 

        // dequeue a page of memory from freed list
        PPFNdata freedPFN;
        freedPFN = getPage();
        if (freedPFN == NULL) {
            fprintf(stderr, "failed to successfully dequeue PFN from freed list\n");
            return NO_FREE_PAGES;
        }
        printf("Allocated PFN from zero/free/standby list\n");

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
        PFsourceVA = (PVOID) ( (ULONG_PTR) pageFileVABlock + (PAGE_SIZE * pageFilePTE.u1.pfPTE.pageFileIndex) );
        
        // copy contents from pagefile to our new page (via pageFileFormatVA)
        memcpy(pageFileFormatVA, PFsourceVA, PAGE_SIZE);

        // unmap pageFileFormatVA from page - PFN is now filled w contents from pagefile
        if (!MapUserPhysicalPages(pageFileFormatVA, 1, NULL)) {
            fprintf(stderr, "error copying into page\n");
            return FALSE;
        }

        // set hardware PTE to valid
        tempPTE.u1.hPTE.validBit = 1;

        if (permissionMasks[RWEpermissions] & writeMask) { // if attempting to write, set dirty bit & clear PF location if it exists

            // set PTE to dirty
            tempPTE.u1.hPTE.dirtyBit = 1;

            // free PF location
            clearPFBitIndex(pageFilePTE.u1.pfPTE.pageFileIndex);

            // clear pagefile pointer out of PFN
            freedPFN->pageFileOffset = MAXULONG_PTR;

        }


        // transfer permissions bit from pfPTE, if it exists
        transferPTEpermissions(&tempPTE, pageFileRWEpermissions);

        // put PFN's corresponding pageNum into PTE
        tempPTE.u1.hPTE.PFN = pageNum;
        
        // assign VA to point at physical page, mirroring our local PTE change              TODO CHECK
        MapUserPhysicalPages(virtualAddress, 1, &pageNum);

        // compiler writes out as indivisible store
        * (volatile PTE *) currPTE = tempPTE;

        return SUCCESS;

    }
    else if (tempPTE.u1.pfPTE.pageFileBit == 1) {                // DEMAND ZERO STATE PTE    

        // check permissions
        PTEpermissions tempRWEpermissions = tempPTE.u1.dzPTE.permissions;
        if (!checkPTEpermissions(tempRWEpermissions, RWEpermissions)) {

            fprintf(stderr, "Invalid permissions\n");
            return ACCESS_VIOLATION;

        } 
        // if write, set dirty bit 
        else if (permissionMasks[RWEpermissions] & writeMask) {

            // set PTE to dirty
            tempPTE.u1.hPTE.dirtyBit = 1;
        }

        // dequeue a page of memory from freed list, setting status bits to active
        PPFNdata freedPFN;
        freedPFN = getPage();

        if (freedPFN == NULL) {
            fprintf(stderr, "failed to successfully dequeue PFN from freed list\n");
            return NO_FREE_PAGES;
        }

        printf("Allocated PFN from zero/free/standby list\n");

        // set PFN status to active
        freedPFN->statusBits = ACTIVE;
        
        // assign currPFN as calculated
        pageNum = freedPFN - PFNarray;
        tempPTE.u1.hPTE.PFN = pageNum;

        // change PTE to validBit;
        tempPTE.u1.hPTE.validBit = 1;

        // compiler writes out as indivisible store
        * (volatile PTE *) currPTE = tempPTE;

        MapUserPhysicalPages(virtualAddress, 1, &pageNum);

        return SUCCESS;                                             // return value of 2; 

    }
    else if (tempPTE.u1.ulongPTE == 0) {                          // ZERO STATE PTE
        // TODO - need to check if vad is mem commit and bring in permissions if so
        fprintf(stderr, "access violation - PTE is zero\n");
        return ACCESS_VIOLATION;

    }
    else {
        fprintf(stderr, "ERROR - not in any recognized PTE state\n");
        exit (-1);
    }
    
}

PPTE
getPTE(void* virtualAddress)
{
    // verify VA param is within the range of the VA block
    if (virtualAddress < leafVABlock || virtualAddress >= leafVABlockEnd) {
        fprintf(stderr, "access violation \n");
        return NULL;
    }

    // get VA's offset into the leafVABlock
    ULONG_PTR offset;
    offset = (ULONG_PTR) virtualAddress - (ULONG_PTR) leafVABlock;

    // convert offset to pagetable index
    ULONG_PTR pageTableIndex;
    pageTableIndex = offset/PAGE_SIZE;     // can i just shift by 12 bytes here instead?

    // get the corresponding page table entry from the PTE array
    PPTE currPTE;
    currPTE = PTEarray + pageTableIndex;

    return currPTE;
}

PPFNdata
getPage()
{
    PPFNdata returnPFN;
    if (zeroListHead.count != 0) {
        returnPFN = dequeuePage(&zeroListHead);
        if (returnPFN == NULL) {
            fprintf(stderr, "Error in getPage(): unable to pull page off free\n");
            return NULL;
        }
        returnPFN->statusBits = ZERO;
        returnPFN->pageFileOffset = MAXULONG_PTR;
        return returnPFN;        
    } if (freeListHead.count != 0) {
        returnPFN = dequeuePage(&freeListHead);

        if (returnPFN == NULL) {
            fprintf(stderr, "Error in getPage(): unable to pull page off free\n");
            return NULL;
        }

    } else if (standbyListHead.count != 0) {

        returnPFN = dequeuePage(&standbyListHead);
        if (returnPFN == NULL) {
            fprintf(stderr, "Error in getPage(): unable to pull page off standby\n");
            return NULL;
        }

        // get PTE
        PPTE currPTE;
        currPTE = PTEarray + returnPFN->PTEindex;       // TODO: possible compatibility issues with multiple processes

        // create copy of the currPTE to reference
        PTE oldPTE;
        oldPTE = *currPTE;

        //  create newPTE that is zeroed
        PTE newPTE;
        newPTE.u1.ulongPTE = 0;

        // if page is not already in pagefile, it MUST be a zero page (i.e. faulted into active but never written, then trimmed to standby)
        // Therefore, the PTE can be set to demand zero
        if (returnPFN->pageFileOffset == MAXULONG_PTR) {
            BOOLEAN bResult;
            bResult = writePage(returnPFN);

            if (bResult != TRUE) {
                fprintf(stderr, "error writing out page\n");
                return NULL;
            }
        }

        newPTE.u1.tPTE.transitionBit = 0;      // TODO: MAY NEED TO FIX FOR MULTITHREADING synchro

        // copy permissions to pf format PTE
        newPTE.u1.pfPTE.permissions = oldPTE.u1.tPTE.permissions;

        // put PF index into pf format PTE
        newPTE.u1.pfPTE.pageFileIndex = returnPFN->pageFileOffset;

        returnPFN->statusBits = FREE;

        // copy newPTE back into currPTE
        * (volatile PTE *) currPTE = newPTE;

    } else {
        return NULL;
    }

    ULONG_PTR PFN;
    PFN = returnPFN - PFNarray;

    // zeroPage (this also updates the status bits in the PFN metadata)
    zeroPage(PFN);

    // set PF offset to our "null" value in the PFN metadata
    returnPFN->pageFileOffset = MAXULONG_PTR;

    return returnPFN;
}

faultStatus 
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{
    faultStatus PFstatus;
    PFstatus = SUCCESS;
    while (PFstatus == SUCCESS) {
        _try {
            if (RWEpermissions == READ_ONLY || READ_EXECUTE) {     // TODO1
                *(volatile CHAR *)virtualAddress; // read

            } else if (RWEpermissions == READ_WRITE || READ_WRITE_EXECUTE) {       //TODO1
                *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress; // write

            } else {
                fprintf(stderr, "invalid permissions\n");
            }
            break;
        } _except (EXCEPTION_EXECUTE_HANDLER) {
            PFstatus = pageFault(virtualAddress, RWEpermissions);
            // pageFault(virtualAddress, WRITE);
        }
    }

    return PFstatus;
}

faultStatus 
isVAaccessible (PVOID virtualAddress, PTEpermissions RWEpermissions) 
{
    _try {
        if (RWEpermissions == READ_ONLY || READ_EXECUTE) { // TODO1
            *(volatile CHAR *)virtualAddress; // read

        } else if (RWEpermissions == READ_WRITE || READ_WRITE_EXECUTE) { // TODO1
            *(volatile CHAR *)virtualAddress = *(volatile CHAR *)virtualAddress; // write

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
    if (tempPTE.u1.hPTE.validBit == 1 || tempPTE.u1.tPTE.transitionBit == 1 || tempPTE.u1.pfPTE.permissions != NO_ACCESS) { // check!! tempPTE.u1.dzPTE.demandZeroBit == 1
        printf("PFN is already valid, transition, or demand zero\n");
        return FALSE;
    }

    if (totalCommittedPages < totalMemoryPageLimit) {
        // commit with WRITE priviliges for TESTING (TODO)
        tempPTE.u1.dzPTE.permissions = RWEpermissions;

        // set demand zero bit (commit)
        tempPTE.u1.dzPTE.pageFileBit = 1;

        tempPTE.u1.dzPTE.pageFileIndex = MAXULONG_PTR;
        totalCommittedPages++;
        printf("Committed VA at %d\n", (ULONG) startVA);
    
    } else {
        // no remaining pages
        fprintf(stderr, "no remaining pages\n");
        return FALSE;
    }

    // * (volatile ULONG_PTR *) currPTE->u1.ulongPTE = tempPTE.u1.ulongPTE;

    * (volatile PTE *) currPTE = tempPTE;
    return TRUE;
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
    else if (tempPTE.u1.hPTE.validBit == 1) {
        // get PFN
        PPFNdata currPFN;
        currPFN = PFNarray + tempPTE.u1.hPTE.PFN;

        // unmap VA from page
        MapUserPhysicalPages(startVA, 1, NULL);

        // if the PFN contents is also stored in pageFile
        if (currPFN->pageFileOffset != MAXULONG_PTR ) {
            clearPFBitIndex(currPFN->pageFileOffset);
        }

        // enqueue Page to free list
        enqueuePage(&freeListHead, currPFN);
    } else if (tempPTE.u1.tPTE.transitionBit == 1) {

        // get PFN
        PPFNdata currPFN;
        currPFN = PFNarray + tempPTE.u1.hPTE.PFN;


        // dequeue from standby/modified list
        dequeueSpecificPage(currPFN);
        
        // if the PFN contents is also stored in pageFile
        if (currPFN->pageFileOffset != MAXULONG_PTR ) {
            clearPFBitIndex(currPFN->pageFileOffset);
        }

        // enqueue Page to free list
        enqueuePage(&freeListHead, currPFN);

    }  else if (tempPTE.u1.pfPTE.pageFileIndex < PAGEFILE_PAGES) {
        clearPFBitIndex(tempPTE.u1.pfPTE.pageFileIndex);
        tempPTE.u1.ulongPTE = 0;
    }
    else if (tempPTE.u1.ulongPTE == 0) {
        fprintf(stderr, "already decommitted\n");
        return TRUE;
    }
    // else if (tempPTE.u1.dzPTE)
    // TODO: need to handle dzPTE and pfPT

    if (totalCommittedPages != 0)  {
        // decrement count of committed pages and zero PTE
        totalCommittedPages--;
    } else {
        fprintf(stderr, "error - no committed pages\n");
        return FALSE;
    }

    memset(&tempPTE, 0, sizeof(PTE));

    * (volatile PTE *) currPTE = tempPTE;
    return TRUE;
}

BOOLEAN
trimPage(void* virtualAddress)
{
    printf("trimming page with VA %d\n", (ULONG) virtualAddress);

    PPTE PTEaddress;
    PTEaddress = getPTE(virtualAddress);

    if (PTEaddress == NULL) {
        fprintf(stderr, "could not trim VA %d - no PTE associated with address\n", (ULONG) virtualAddress);
        return FALSE;
    }
    
    PTE PTEtoTrim;
    PTEtoTrim = *PTEaddress;

    // check if PTE's valid bit is set - if not, can't be trimmed and return failure
    if (PTEtoTrim.u1.hPTE.validBit == 0) {
        fprintf(stderr, "could not trim VA %d - PTE is not valid\n", (ULONG) virtualAddress);
        return FALSE;
    }
    // invalidate PTE
    PTEtoTrim.u1.hPTE.validBit = 0;

    // unmap page from VA (invalidates hardwarePTE)
    MapUserPhysicalPages(virtualAddress, 1, NULL);

    // get pageNum
    ULONG_PTR pageNum;
    pageNum = PTEtoTrim.u1.hPTE.PFN;

    // get PFN
    PPFNdata PFNtoTrim;
    PFNtoTrim = PFNarray + pageNum;

    // check dirtyBit to see if page has been modified
    if (PTEtoTrim.u1.hPTE.dirtyBit == 0) {

        // add given VA's page to standby list
        enqueuePage(&standbyListHead, PFNtoTrim);

    } else if (PTEtoTrim.u1.hPTE.dirtyBit == 1) {

        // add given VA's page to modified list;
        enqueuePage(&modifiedListHead, PFNtoTrim);

    }

    // set transitionBit to 1
    PTEtoTrim.u1.tPTE.transitionBit = 1;  
    *PTEaddress = PTEtoTrim;

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

    // leafVABlock = VirtualAlloc(NULL, NUM_PAGES*PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
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
        fprintf(stderr, "Could not allocate for PFN metadata array\n");
        exit(-1);
    }

    // loop through all PFNs, MEM_COMMITTING PFN metadata subsections for each in the aPFNs array
    for (int i = 0; i < numPages; i++){
        PPFNdata newPFN;
        newPFN = PFNarray + aPFNs[i];

        commitCheckVA = VirtualAlloc(newPFN, sizeof (PFNdata), MEM_COMMIT, PAGE_READWRITE);
        if (commitCheckVA == NULL) {
            fprintf(stderr, "failed to commit subsection of PFN array at PFN %d\n", i);
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
        fprintf(stderr, "Could not allocate for PTEarray\n");
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
        fprintf(stderr, "Could not allocate for pageFile\n");
        exit(-1);
    }
    return pageFileAddress;
}

BOOLEAN
zeroPage(ULONG_PTR PFN)
{

    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(zeroVA, 1, &PFN)) {
        fprintf(stderr, "error remapping zeroVA\n");
        return FALSE;
    }

    memset(zeroVA, 0, PAGE_SIZE);

    // unmap zeroVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(zeroVA, 1, NULL)) {
        fprintf(stderr, "error zeroing page\n");
        return FALSE;
    }

    // TODO - CHECK SHOULD THIS (SETTING STATUS BITS) BE IN HERE

    // get associated PFN metadata
    PPFNdata metadata;
    metadata = PFNarray + PFN;

    // set statusBits to zero
    metadata->statusBits = ZERO;

    return TRUE;
}

BOOLEAN
zeroPageWriter()
{
    printf("freeListCount == %llu\n", freeListHead.count);

    PPFNdata PFNtoZero;
    PFNtoZero = dequeuePage(&freeListHead);

    if (PFNtoZero == NULL) {
        fprintf(stderr, "zero list empty - could not write out\n");
        return FALSE;
    }

    // get page number, rather than PFN metadata
    ULONG_PTR pageNumtoZero;
    pageNumtoZero = PFNtoZero - PFNarray;

    // zero page (setting PFN metadata status bits en route)
    zeroPage(pageNumtoZero);

    // enqueue to zeroList
    enqueuePage(&zeroListHead, PFNtoZero);
    printf("Moved page from standby -> free \n");

    return TRUE;
}


/*
 * FIX1
 * - check if rhere are any pages on modified list
 * - find free block in pagefile (by checking pageFile bitmap)
 * - if found
 *    - copy address of disk into PFN
 *    - take entry off modified list
 *    - map page to modifiedWriteVA
 *    - copy contents of modifiedWRite VA to pagefile location
 *    - mark pf location as busy
 * TODO: bump refcount and set read in progress bit 
 * 
 */
BOOLEAN
modifiedPageWriter()
{
    printf("modifiedListCount == %llu\n", modifiedListHead.count);
    PPFNdata PFNtoWrite;
    PFNtoWrite = dequeuePage(&modifiedListHead);

    if (PFNtoWrite == NULL) {
        fprintf(stderr, "modified list empty - could not write out\n");
        return FALSE;
    }


    BOOLEAN bResult;
    bResult = writePage(PFNtoWrite);

    if (bResult != TRUE) {
        fprintf(stderr, "error writing out page\n");
        return FALSE;
    }

    // PFN status to standby (from modified)
    PFNtoWrite->statusBits = STANDBY;

    // enqueue page to standby
    enqueuePage(&standbyListHead, PFNtoWrite);

    printf("Moved page from modified -> standby (wrote out to PageFile successfully)\n");

    return TRUE;   

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
    initLinkHead(&(headData->head));
    headData->count = 0;
}

// 
VOID 
main() 
{

    // initialize free/standby/modified lists
    for (int status = 0; status < ACTIVE; status++) {
        initListHead(&listHeads[status]);
    }

    // reserve AWE address for zeroVA
    zeroVA = VirtualAlloc(NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);

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
        fprintf(stderr, "failed to allocate PFNarray\n");
    }

    // secure privilege for the code
    BOOLEAN privResult;
    privResult = getPrivilege();
    BOOLEAN bResult;

    ULONG_PTR numPagesReturned = NUM_PAGES;
    bResult = AllocateUserPhysicalPages(GetCurrentProcess(), &numPagesReturned, aPFNs);
    if (bResult != TRUE) {
        fprintf(stderr, "could not allocate pages successfunlly \n");
        exit(-1);
    }

    if (numPagesReturned != NUM_PAGES) {
        fprintf(stderr, "allocated only %d pages out of %d pages requested\n", numPagesReturned, NUM_PAGES);
    }

    // create VAD
    leafVABlock = initVABlock(numPagesReturned, PAGE_SIZE);     // will be the starting VA of the memory I've allocated

    // create local PFN metadata array
    PFNarray = initPFNarray(aPFNs, numPagesReturned, PAGE_SIZE);

    // create local PTE array to map VAs to pages
    PTEarray = initPTEarray(numPagesReturned, PAGE_SIZE);

    // create PageFile section of memory
    pageFileVABlock = initPageFile(PAGEFILE_SIZE);


    /************** TESTING *****************/
    PPTE currPTE = PTEarray;
    void* testVA = leafVABlock;


    printf("--------------------------------\n");
    // FAULTING in testVAs

    ULONG_PTR testNum = 10;
    printf("Committing and then faulting in %d pages\n", testNum);
    for (int i = 0; i < testNum; i++) {
        faultStatus testStatus;

        // commitVA(testVA, READ_EXECUTE, 1);    // can commit (doesnt cause crash)
        commitVA(testVA, READ_WRITE_EXECUTE, 1);    // can commit

        // decommitVA(testVA, 1);
    
        if (i % 2 == 0) {
            testStatus = accessVA(testVA, READ_WRITE);
        } else {
            testStatus = accessVA(testVA, READ_ONLY);
        }

        printf("tested (VA = %d), return status = %d\n", (ULONG) testVA, testStatus);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);
    }


    printf("--------------------------------\n");

    // TRIMMING tested VAs (active -> standby/modified)

    // reset testVa
    testVA = leafVABlock;

    for (int i = 0; i < testNum; i++) {

        // trim the VAs we have just tested (to transition, from active)
        trimPage(testVA);
        testVA = (void*) ( (ULONG_PTR) testVA + PAGE_SIZE);

        // alternate calling modifiedPageWriter and zeroPageWriter
        if (i%2 == 0) {
            // modifiedPageWriter();
            modifiedPageWriter();

        } else {
            zeroPageWriter();
        }
    
    }



    printf("--------------------------------\n");

    testVA = leafVABlock;

    // toggle - can either FAULT or TEST VAs 
    for (int i = 0; i < testNum; i++) {
        // faultStatus testStatus = isVAaccessible(testVA, READ_WRITE);  // to TEST VAs
        faultStatus testStatus = pageFault(testVA, READ_ONLY);      // to FAULT VAs
    
        printf("tested (VA = %d), return status = %d\n", (ULONG) testVA, testStatus);
        testVA = (void*) ( (ULONG) testVA + PAGE_SIZE);
    }
    printf("program complete\n");

    //  free memory allocated
    VirtualFree(leafVABlock, 0, MEM_RELEASE);
    VirtualFree(PFNarray, 0, MEM_RELEASE);
    VirtualFree(PTEarray, 0, MEM_RELEASE);

}


/*
 *
 * detecting pagefileFormat: if valid = 0 and transition = 0
 * - subset of pageFile format is demand zero
 * 
 * we currently have valid, transition, "demand zero"
 */