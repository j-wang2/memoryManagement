#include "userMode-AWE-pageFile.h"
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
                currFrame |= (1 << j);

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
    currFrame &= ~(1 << j);

    pageFileBitArray[i] = currFrame;

}



BOOLEAN
writePage(PPFNdata PFNtoWrite)
{

    ULONG_PTR PFN;
    PFN = PFNtoWrite - PFNarray;

    ULONG_PTR bitIndex;
    bitIndex = setPFBitIndex();
    if (bitIndex == INVALID_PAGEFILE_INDEX) {
        fprintf(stderr, "no remaining space in pagefile - could not write out \n");
        return FALSE;
    }

    // add/update pageFileOffset field of PFN
    PFNtoWrite->pageFileOffset = bitIndex;

    // map given page to the modifiedWriteVA
    if (!MapUserPhysicalPages(modifiedWriteVA, 1, &PFN)) {
        fprintf(stderr, "error mapping modifiedWriteVA\n");
        return FALSE;
    }

    // get location in pagefile from bitindex
    void* PFLocation;
    PFLocation = (void*) ( (ULONG_PTR)pageFileVABlock + bitIndex*PAGE_SIZE);

    // copy the contents in the currentPFN out to the pagefile
    memcpy(PFLocation, modifiedWriteVA, PAGE_SIZE);

    // unmap modifiedWriteVA from page
    if (!MapUserPhysicalPages(modifiedWriteVA, 1, NULL)) {
        fprintf(stderr, "error unmapping modifiedWriteVA\n");
        return FALSE;
    }
    printf("successfully wrote page to pagefile\n");
    return TRUE;
}
