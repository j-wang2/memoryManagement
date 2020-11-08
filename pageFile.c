#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
// #include "pagefile.h"
#include "jLock.h"


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


VOID
clearPFBitIndex(ULONG_PTR pfVA) 
{

    // if pagefile address is invalid, return
    if (pfVA == INVALID_BITARRAY_INDEX) {

        return;

    }

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
    bitIndex = setPFBitIndex();


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

    ASSERT(expectedSig == * (PULONG_PTR) modifiedWriteVA || * (PULONG_PTR) modifiedWriteVA == 0 );


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

    // PFNtoWrite->dirtyBit = 0;      // TODO

    releaseJLock(&PFNtoWrite->lockBits);

    PRINT("successfully wrote page to pagefile\n");

    return TRUE;
}


BOOLEAN
readPageFromFileSystem(ULONG_PTR destPFN, ULONG_PTR pageFileIndex, ULONG_PTR expectedSig) {

    
    PVANode readPFVANode;
    readPFVANode = dequeueLockedVA(&readPFVAListHead);

    while (readPFVANode == NULL) {

        PRINT("[pageFilePageFault] Waiting for release of event node (list empty)\n");

        WaitForSingleObject(readPFVAListHead.newPagesEvent, INFINITE);

        readPFVANode = dequeueLockedVA(&readPFVAListHead);

    }


    PVOID readPFVA;
    readPFVA = readPFVANode->VA;    


    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(readPFVA, 1, &destPFN)) {

        PRINT_ERROR("[pageFilePageFault]error remapping page to copy from PF\n");
        return FALSE;

    }


    // get PFsourceVA from the pageFileIndex
    PVOID PFsourceVA;
    PFsourceVA = (PVOID) ( (ULONG_PTR) pageFileVABlock + (pageFileIndex << PAGE_SHIFT) );

    
    // copy contents from pagefile to our new page
    memcpy(readPFVA, PFsourceVA, PAGE_SIZE);

    //
    // Verify signature is coming back as expected (either VA signature or 0)
    //

    ASSERT(expectedSig == * (PULONG_PTR) readPFVA || * (PULONG_PTR) readPFVA == 0 );

    // unmap VA from page - PFN is now filled w contents from pagefile
    if (!MapUserPhysicalPages(readPFVA, 1, NULL)) {
        PRINT_ERROR("error copying page from into page\n");
        return FALSE;
    }

    enqueueVA(&readPFVAListHead, readPFVANode);

    return TRUE;
    
}