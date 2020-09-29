#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
// #include "pagefile.h"


ULONG_PTR
setPFBitIndex()
{

    ULONG_PTR bitIndex;

    for (ULONG_PTR i = 0; i < ARRAYSIZE (pageFileBitArray); i++){

        ULONG_PTR currFrame;
        currFrame = pageFileBitArray[i];

        for (ULONG_PTR j = 0; j < (sizeof(currFrame) * 8); j++){

            // free
            if ((currFrame & 1) == 0){

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
    return INVALID_PAGEFILE_INDEX;
}


VOID
clearPFBitIndex(ULONG_PTR pfVA) 
{
    if (pfVA == INVALID_PAGEFILE_INDEX) {
        return;
    }
    ULONG_PTR i;
    ULONG_PTR j;
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
    writeVANode = dequeueVA(&writeVAListHead);

    if (writeVANode == NULL) {
        PRINT("[modifiedPageWriter] TODO: waiting for release\n");
    }

    PVOID modifiedWriteVA;
    modifiedWriteVA = writeVANode->VA;

    ULONG_PTR PFN;
    PFN = PFNtoWrite - PFNarray;

    ULONG_PTR bitIndex;
    bitIndex = setPFBitIndex();
    if (bitIndex == INVALID_PAGEFILE_INDEX) {
        PRINT_ERROR("no remaining space in pagefile - could not write out\n");
        return FALSE;
    }

    // map given page to the modifiedWriteVA
    if (!MapUserPhysicalPages(modifiedWriteVA, 1, &PFN)) {
        PRINT_ERROR("error mapping modifiedWriteVA\n");
        clearPFBitIndex(bitIndex);
        enqueueVA(&writeVAListHead, writeVANode);
        return FALSE;
    }

    // get location in pagefile from bitindex
    void* PFLocation;
    PFLocation = (void*) ( (ULONG_PTR)pageFileVABlock + ( bitIndex << PAGE_SHIFT ) );        // equiv to bitIndex*page_size

    // copy the contents in the currentPFN out to the pagefile
    memcpy(PFLocation, modifiedWriteVA, PAGE_SIZE);

    // unmap modifiedWriteVA from page
    if (!MapUserPhysicalPages(modifiedWriteVA, 1, NULL)) {
        PRINT_ERROR("error unmapping modifiedWriteVA\n");
        clearPFBitIndex(bitIndex);                      // TODO - does order matter here?
        enqueueVA(&writeVAListHead, writeVANode);
        return FALSE;
    }

    enqueueVA(&writeVAListHead, writeVANode);


    // add/update pageFileOffset field of PFN
    // ONLY updates if the mapuserphysical pages calls are also successful
    PFNtoWrite->pageFileOffset = bitIndex;

    PRINT("successfully wrote page to pagefile\n");
    return TRUE;
}
