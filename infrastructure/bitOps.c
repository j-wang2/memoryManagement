#include "../usermodeMemoryManager.h"
#include "bitOps.h"

#define BITS_PER_FRAME 64

#ifdef BITARRAY_TESTING
ULONG_PTR testArray[5];
#endif

ULONG_PTR
reserveBitRange(ULONG_PTR bits, PULONG_PTR bitArray, ULONG_PTR bitArraySize)
{

    //
    // Note: Bitarray MUST be locked to use this function
    //

    ULONG_PTR bitIndex;
    ULONG_PTR bitsFound;
    BOOLEAN success;
    ULONG_PTR i;
    ULONG_PTR j;

    //
    // Initialize bit index return val to invalid value
    //

    bitIndex = INVALID_BITARRAY_INDEX;

    //
    // Initialize i and j index variables to zero
    //

    i = 0;

    j = 0;

    //
    // Initialize bitsFound and success variables to zero and false,
    // respectively
    //

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

                if (bitsFound == 0) {

                    //
                    // If bitsFound is zero, this marks the prospective start
                    // of an open space - therefore, calculate the bitindex
                    //

                    bitIndex = i * BITS_PER_FRAME + j;

                }

                bitsFound++;

                //
                // If a stretch of bits is found, set success flag to true and
                // break out of the loop
                //

                if (bitsFound == bits) {

                    success = TRUE;

                    //
                    // Increment j one more time (to account for zero-indexing)
                    //

                    j++;

                    break;

                }

            } else {

                //
                // Reset bitsfound and set success flag to false
                //

                bitsFound = 0;

                success = FALSE;

            }

            //
            // Shift current frame by a single bit
            //

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

        //
        // If success flag is set, bitindex cannot be invalid (since it 
        // would have been set en route)
        //

        ASSERT(bitIndex != INVALID_BITARRAY_INDEX);

        //
        // Now that a clear range has been found, call set bit range with
        // first param set in order to reflect this change in the bitarray
        //

        setBitRange(TRUE, bitIndex, bits, bitArray);

        return bitIndex;

    } else {

        PRINT("[reserveBitRange] unable to find free bit range\n");
        return INVALID_BITARRAY_INDEX;

    }



}


VOID
setBitRange(BOOLEAN isSet, ULONG_PTR startBitIndex, ULONG_PTR numPages, PULONG_PTR bitArray)
{

    ULONG_PTR i;
    ULONG_PTR j;
    ULONG_PTR currFrame;

    //
    // i variable keeps track of byte increments (i.e. "bytes place")
    //

    i = startBitIndex / BITS_PER_FRAME;

    //
    // j variable keeps track of bit increments (i.e. "bits place")
    // 

    j = startBitIndex % BITS_PER_FRAME;

    if (j != 0) {

        for (ULONG_PTR l = j; numPages != 0 && l < BITS_PER_FRAME; l++) {
            
            //
            // Reset currFrame to ULONG_PTR alignment
            //

            currFrame = bitArray[i];

            if (isSet) {

                ASSERT( (currFrame & ((ULONG_PTR)1 << l) ) == 0);

                currFrame |= ((ULONG_PTR)1 << l);

            } else {

                ASSERT( currFrame & ((ULONG_PTR)1 << l) );

                currFrame &= ~((ULONG_PTR)1 << l);

            }

            //
            // set the frame in the bitarray to the newly edited frame
            //

            bitArray[i] = currFrame;

            numPages--;

        }

        i++;

    }

    //
    // Now that operations are ULONG_PTR aligned, go fast
    // AND increment i (as the index of ULONG_PTR denomination)
    //

    if (numPages >= BITS_PER_FRAME) {

        //
        // Must use a different ULONG_PTR (k) since i does not necessarily 
        // start at zero (but k should)
        //

        for (ULONG_PTR k = 0; k < numPages/BITS_PER_FRAME; k += 1, i++) {

            if (isSet) {

                ASSERT(bitArray[i] == 0);

                bitArray[i] = MAXULONG_PTR;

            } else {

                ASSERT(bitArray[i] == MAXULONG_PTR);

                bitArray[i] = 0;

            }

        }

        numPages %= BITS_PER_FRAME;

    }

    for (int n = 0; n < numPages; n++) {

        //
        // Reset currFrame to ULONG_PTR alignment
        //

        currFrame = bitArray[i];


        if (isSet) {

            ASSERT( (currFrame & ((ULONG_PTR)1 << n) ) == 0);

            currFrame |= ((ULONG_PTR)1 << n);

        } else {

            ASSERT( currFrame & ((ULONG_PTR)1 << n) );

            currFrame &= ~((ULONG_PTR)1 << n);

        }

        //
        // Set the frame in the bitarray to the newly edited frame
        //

        bitArray[i] = currFrame;

    }


}


VOID
printArray(PULONG_PTR bitArray, ULONG_PTR length)
{

    for (int i = 0; i < length; i++) {
        printf("%llx | ", bitArray[i]);

    }
    printf("\n");
}


#ifdef BITARRAY_TESTING

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