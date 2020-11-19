#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
// #include "pagefile.h"
#include "jLock.h"
#include "PTEpermissions.h"

#ifndef PAGEFILE_PFN_CHECK

ULONG_PTR
setPFBitIndex()
{

    ULONG_PTR bitIndex;

    EnterCriticalSection(&pageFileLock);

    for (ULONG_PTR i = 0; i < ARRAYSIZE (pageFileBitArray); i++) {

        ULONG_PTR currFrame;
        currFrame = pageFileBitArray[i];

        for (ULONG_PTR j = 0; j < (sizeof(currFrame) * 8); j++) {

            //
            // If the current bit is clear, set the bit and return the
            // bitIndex
            //

            if ((currFrame & 1) == 0) {

                // reset currFrame
                currFrame = pageFileBitArray[i];

                //
                // Assert the current bit is clear
                //
                
                ASSERT( ( currFrame & ((ULONG_PTR)1 << j) ) == 0);


                // set the bit
                currFrame |= ((ULONG_PTR)1 << j);

                // set the frame in the bitarray to the edited frame
                pageFileBitArray[i] = currFrame;

                LeaveCriticalSection(&pageFileLock);

                // calculate bitIndex
                bitIndex = (i * (sizeof(currFrame)) * 8) + j;

                return bitIndex;

            }

            currFrame >>= 1;   

        }

    }

    LeaveCriticalSection(&pageFileLock);

    return INVALID_BITARRAY_INDEX;
    
}

#else

ULONG_PTR
setPFDebugIndex(PPFNdata currPFN)
{

    pageFileDebug currEntry;
    PPageFileDebug checkEntry;

    acquireJLock(&currPFN->lockBits);

    currEntry.currPFN = currPFN;
    currEntry.PFNdata = *currPFN;

    ULONG_PTR PTEindex;
    PTEindex = currPFN->PTEindex;

    PPTE currPTE;
    currPTE = PTEarray + PTEindex;

    currEntry.currPTE = currPTE;
    currEntry.PTEdata = *currPTE;
    
    EnterCriticalSection(&pageFileLock);

    for (ULONG_PTR j = 0; j < PAGEFILE_PAGES; j++) {

        checkEntry = pageFileDebugArray + j;
        ULONG_PTR checkPTEindex = checkEntry->currPTE - PTEarray;

        //
        // Verify PTE corresponding to the PFN being written to pagefile 
        // does not already occupy a pagefile space.
        //

        ASSERT(PTEindex != checkPTEindex);

    }


    for (ULONG_PTR i = 0; i < PAGEFILE_PAGES; i++) {

        if (pageFileDebugArray[i].currPFN == NULL && pageFileDebugArray[i].currPTE == NULL) {

            pageFileDebugArray[i] = currEntry;
            LeaveCriticalSection(&pageFileLock);
            releaseJLock(&currPFN->lockBits);

            return i;

        }

    }
    LeaveCriticalSection(&pageFileLock);

    releaseJLock(&currPFN->lockBits);
    return INVALID_BITARRAY_INDEX;
    
}

#endif


VOID
clearPFBitIndex(ULONG_PTR pfVA) 
{

    // if pagefile address is invalid, return
    if (pfVA == INVALID_BITARRAY_INDEX) {

        return;

    }

    #ifdef PAGEFILE_PFN_CHECK

        pageFileDebug temp = { 0 };

        EnterCriticalSection(&pageFileLock);

        ASSERT(memcmp(&pageFileDebugArray[pfVA], &temp, sizeof(temp)) != 0 );

        pageFileDebug currEntry;

        currEntry = pageFileDebugArray[pfVA];

        PTE logPTE;
        logPTE.u1.ulongPTE = MAXULONG_PTR;

        #ifdef PTE_CHANGE_LOG

            logEntry( currEntry.currPTE, *currEntry.currPTE, logPTE, &currEntry.PFNdata);

        #endif

        memset(&pageFileDebugArray[pfVA], 0, sizeof(pageFileDebug) );

        LeaveCriticalSection(&pageFileLock);

        return;

    #else

    // keeps track of byte increments (i.e. "bytes place")
    ULONG_PTR i;

    // keeps track of bit increments (i.e. "bits place")
    ULONG_PTR j;

    // keeps track of current frame
    ULONG_PTR currFrame;

    i = pfVA / (sizeof(ULONG_PTR) * 8);
    j = pfVA % (sizeof(ULONG_PTR) * 8);

    EnterCriticalSection(&pageFileLock);


    currFrame = pageFileBitArray[i];

    //
    // Assert bit is previously set
    //

    ASSERT ( currFrame & (ULONG_PTR)1 << j );

    currFrame &= ~((ULONG_PTR)1 << j);

    pageFileBitArray[i] = currFrame;

    LeaveCriticalSection(&pageFileLock);

    #endif

}


BOOLEAN
writePageToFileSystem(PPFNdata PFNtoWrite, ULONG_PTR expectedSig)
{

    PVANode writeVANode;
    writeVANode = dequeueLockedVA(&writeVAListHead);

    while (writeVANode == NULL) {

        PRINT("[modifiedPageWriter] waiting for release\n");

        WaitForSingleObject(writeVAListHead.newPagesEvent, INFINITE);

        writeVANode = dequeueLockedVA(&writeVAListHead);

    }

    PVOID modifiedWriteVA;
    modifiedWriteVA = writeVANode->VA;

    ULONG_PTR PFN;
    PFN = PFNtoWrite - PFNarray;

    ULONG_PTR bitIndex;

    #ifdef PAGEFILE_PFN_CHECK
        bitIndex = setPFDebugIndex(PFNtoWrite);
    #else
        bitIndex = setPFBitIndex();
    #endif


    if (bitIndex == INVALID_BITARRAY_INDEX) {

        enqueueVA(&writeVAListHead, writeVANode);
        PRINT("no remaining space in pagefile - could not write out\n");
        return FALSE;

    }


    // map given page to the modifiedWriteVA
    if (!MapUserPhysicalPages(modifiedWriteVA, 1, &PFN)) {

        clearPFBitIndex(bitIndex);

        enqueueVA(&writeVAListHead, writeVANode);

        PRINT_ERROR("error mapping modifiedWriteVA\n");

        return FALSE;

    }

    //
    // Verify signature written to filesystem is expected (either VA signature or 0)
    //

    #ifdef VERIFY_ADDRESS_SIGNATURES

        ASSERT(expectedSig == * (PULONG_PTR) modifiedWriteVA || * (PULONG_PTR) modifiedWriteVA == 0 );

    #endif

    // get location in pagefile from bitindex
    PVOID PFLocation;
    PFLocation = (void*) ( (ULONG_PTR)pageFileVABlock + ( bitIndex << PAGE_SHIFT ) );        // equiv to bitIndex*page_size


    // copy the contents in the currentPFN out to the pagefile
    memcpy(PFLocation, modifiedWriteVA, PAGE_SIZE);


    // unmap modifiedWriteVA from page
    if (!MapUserPhysicalPages(modifiedWriteVA, 1, NULL)) {

        clearPFBitIndex(bitIndex);

        enqueueVA(&writeVAListHead, writeVANode);

        PRINT_ERROR("error unmapping modifiedWriteVA\n");

        return FALSE;

    }


    enqueueVA(&writeVAListHead, writeVANode);


    // add/update pageFileOffset field of PFN
    // ONLY updates if the mapuserphysical pages calls are also successful

    acquireJLock(&PFNtoWrite->lockBits);

    PFNtoWrite->pageFileOffset = bitIndex;

    releaseJLock(&PFNtoWrite->lockBits);

    PRINT("successfully wrote page to pagefile\n");

    return TRUE;

}


BOOLEAN
readPageFromFileSystem(ULONG_PTR destPFN, ULONG_PTR pageFileIndex, ULONG_PTR expectedSig) {

    
    PVANode readPFVANode;
    PVOID readPFVA;
    PVOID PFsourceVA;

    //
    // Acquire a spare VA from readPFVAList to temporarily
    // map to page for memcpy operation
    //

    readPFVANode = dequeueLockedVA(&readPFVAListHead);

    while (readPFVANode == NULL) {

        PRINT("[pageFilePageFault] Waiting for release of event node (list empty)\n");

        WaitForSingleObject(readPFVAListHead.newPagesEvent, INFINITE);

        readPFVANode = dequeueLockedVA(&readPFVAListHead);

    }


    readPFVA = readPFVANode->VA;    

    //
    // Map given page to the temporary reading-in VA
    //

    if (!MapUserPhysicalPages(readPFVA, 1, &destPFN)) {

        PRINT_ERROR("[pageFilePageFault]error remapping page to copy from PF\n");

        return FALSE;

    }

    //
    // Derive PFsourceVA from the pageFileIndex
    //

    PFsourceVA = (PVOID) ( (ULONG_PTR) pageFileVABlock + (pageFileIndex << PAGE_SHIFT) );

    //
    // Copy contents from pagefile to our new 
    //

    memcpy(readPFVA, PFsourceVA, PAGE_SIZE);

    //
    // Verify signature is coming back as expected (either VA signature or 0)
    //

    #ifdef VERIFY_ADDRESS_SIGNATUREs

        ASSERT(expectedSig == * (PULONG_PTR) readPFVA || * (PULONG_PTR) readPFVA == 0 );

    #endif

    //
    // Unmap VA from page - PFN is now filled w contents from pagefile
    //

    if (!MapUserPhysicalPages(readPFVA, 1, NULL) ) {

        PRINT_ERROR("error copying page from into page\n");
        return FALSE;
        
    }

    enqueueVA(&readPFVAListHead, readPFVANode);

    return TRUE;
    
}