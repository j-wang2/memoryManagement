#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
// #include "pagefile.h"
#include "jLock.h"


ULONG_PTR
setPFBitIndex()
{

    ULONG_PTR bitIndex;

    for (ULONG_PTR i = 0; i < ARRAYSIZE (pageFileBitArray); i++) {

        ULONG_PTR currFrame;
        currFrame = pageFileBitArray[i];

        for (ULONG_PTR j = 0; j < (sizeof(currFrame) * 8); j++) {

            // free
            if ((currFrame & 1) == 0) {

                // reset currFrame
                currFrame = pageFileBitArray[i];

                // set the bit
                currFrame |= ((ULONG_PTR)1 << j);

                // set the frame in the bitarray to the edited frame
                pageFileBitArray[i] = currFrame;

                // calculate bitIndex
                bitIndex = (i * (sizeof(currFrame)) * 8) + j;
                return bitIndex;
            }
            currFrame >>= 1;     
        }
    }

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

    currFrame = pageFileBitArray[i];
    currFrame &= ~((ULONG_PTR)1 << j);

    pageFileBitArray[i] = currFrame;

}


BOOLEAN
writePageToFileSystem(PPFNdata PFNtoWrite)
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

    // get location in pagefile from bitindex
    void* PFLocation;
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
readPageFromFileSystem(ULONG_PTR destPFN, ULONG_PTR pageFileIndex) {

    
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


    // unmap VA from page - PFN is now filled w contents from pagefile
    if (!MapUserPhysicalPages(readPFVA, 1, NULL)) {
        PRINT_ERROR("error copying page from into page\n");
        return FALSE;
    }

    enqueueVA(&readPFVAListHead, readPFVANode);

    return TRUE;
    
}