#include "userMode-AWE-pageFile.h"
#include "bitOps.h"

#define BITS_PER_FRAME 64


ULONG_PTR testArray[5];


ULONG_PTR
reserveBitRange(ULONG_PTR bits, PULONG_PTR bitArray, ULONG_PTR bitArraySize)
{

    //
    // TODO: lock BITARRAY
    //

    ULONG_PTR bitIndex;
    ULONG_PTR bitsFound;
    BOOLEAN success;
    ULONG_PTR i;
    ULONG_PTR j;

    i = 0;
    j = 0;


    bitsFound = 0;
    

    success = FALSE;

    //
    // ULONG_PTR (8-byte/64-bit) level for-loop
    //

    for (i = 0; i < bitArraySize / BITS_PER_FRAME; i++) {

        ULONG_PTR currFrame;

        currFrame = bitArray[i];

        //
        // No free bits in the current frame - continue to next
        //

        if (currFrame == MAXULONG_PTR) {

            bitsFound = 0;
            continue;

        }

        //
        // Bit-level for-loop
        // j variable keeps track of starting bit index
        //

        for (j = 0; j < BITS_PER_FRAME; j++) {


            // 
            // Check if location is cleared (cleared bit)
            //

            if ((currFrame & 1) == 0) {

                //
                // Increment bitsFound, checking to see if the requested amount 
                // has been reached - if so, break
                //

                bitsFound++;

                if (bitsFound == bits) {

                    success = TRUE;
                    j++;
                    break;

                }

            } else {

                bitsFound = 0;
                success = FALSE;

            }

            currFrame >>= 1;

        }

        //
        // If all requested bits are found successfully,
        // break, set bit range, and return the starting
        // index
        //

        if (success == TRUE) {

            break;

        }

    }

    if (success) {

        bitIndex = i * BITS_PER_FRAME + j - bits;

        printf("bitIndex %llu\n", bitIndex);
        setBitRange(TRUE, bitIndex, bits, bitArray);
        return bitIndex;

    } else {

        printf("[reserveBitRange] unable to find free bit range\n");
        return INVALID_BITARRAY_INDEX;

    }



}


VOID
setBitRange(BOOLEAN isSet, ULONG_PTR startBitIndex, ULONG_PTR numPages, PULONG_PTR bitArray)
{

    // keeps track of byte increments (i.e. "bytes place")
    ULONG_PTR i;

    // keeps track of bit increments (i.e. "bits place")
    ULONG_PTR j;

    // keeps track of current frame
    ULONG_PTR currFrame;

    i = startBitIndex / (sizeof(ULONG_PTR) * 8);
    j = startBitIndex % (sizeof(ULONG_PTR) * 8);

    printf("i: %llu \n", i);

    if (j != 0) {

        for (ULONG_PTR l = j; numPages != 0 && l < BITS_PER_FRAME; l++) {

            // Reset currFrame to ULONG_PTR alignment
            currFrame = bitArray[i];

            if (isSet) {
                currFrame |= ((ULONG_PTR)1 << l);

            } else {
                currFrame &= ~((ULONG_PTR)1 << l);
            }


            // set the frame in the bitarray to the newly edited frame
            bitArray[i] = currFrame;

            numPages--;

        }

        i++;

    }

    printf("numPages: %llu\n", numPages);
    printf("i: %llu \n", i);

    //
    // Now that operations are ULONG_PTR aligned, go fast
    // AND increment i (as the index of ULONG_PTR denomination)
    //

    if (numPages >= 64) {

        for ( i = i; i < numPages/64; i++) {

            if (isSet) {
                bitArray[i] = MAXULONG_PTR;
            } else {
                bitArray[i] = 0;

            }

        }

        numPages %= 64;

    }

    printf("numPages: %llu\n", numPages);
    printf("i: %llu \n", i);


    for (int n = 0; n < numPages; n++) {

        // Reset currFrame to ULONG_PTR alignment
        currFrame = bitArray[i];


        if (isSet) {
            currFrame |= ((ULONG_PTR)1 << n);

        } else {
            currFrame &= ~((ULONG_PTR)1 << n);
        }

        // set the frame in the bitarray to the newly edited frame
        bitArray[i] = currFrame;

    }


}


VOID
printArray(PULONG_PTR bitArray, ULONG_PTR length)
{

    for (int i = 0; i < length; i++) {
        printf("%llx | ", bitArray[i]);       // should be 1f

    }
    printf("\n");
}


#if 0 
VOID
main()
{

    ULONG_PTR returnVal;

    returnVal = reserveBitRange(129, testArray, sizeof(testArray) * 8);
    printf("%llu\n", returnVal);        // should be 0
    printArray(testArray, 5);

    setBitRange( FALSE, 0, 128, testArray);
    printArray(testArray, 5);



    returnVal = reserveBitRange(65, testArray, sizeof(testArray) * 8);
    // returnVal = reserveBitRange(66, testArray, sizeof(testArray));


    printf("%llu\n", returnVal);        // should be 0
    printArray(testArray, 5);


    returnVal = reserveBitRange(5, testArray, sizeof(testArray) * 8);
    printf("%llu\n", returnVal);        // should be 0
    printArray(testArray, 5);



    returnVal = reserveBitRange(5, testArray, sizeof(testArray) * 8);
    printf("%llu\n", returnVal);        // should be 5
    printArray(testArray, 5);

    printf("_----\n");


    returnVal = reserveBitRange(126, testArray, sizeof(testArray) * 8);
    printf("%llu\n", returnVal);        // should be 10
    printArray(testArray, 5);
    

}

#endif