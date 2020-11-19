#include "userMode-AWE-pageFile.h"

/*
 * reserveBitRange: function to find and set a clear range of bits in a bitarray
 *  - calls setBitRange once a clear range is found
 *  - must hold bitarray lock to call if caller is multithreaded
 * 
 * Returns ULONG_PTR:
 *  - starting bitIndex on success
 *  - INVALID_BITARRAY_INDEX on failure
 */
ULONG_PTR
reserveBitRange(ULONG_PTR bits, PULONG_PTR bitArray, ULONG_PTR bitArraySize);


/*
 * setBitRange: function to either set or clear a range of bits in a bitarray
 *  - if isSet param is true, sets bits. If isSet param is false, clears bits.
 * 
 * No return value
 * 
 */
VOID
setBitRange(BOOLEAN isSet, ULONG_PTR startBitIndex, ULONG_PTR numPages, PULONG_PTR bitArray);


/*
 * printArray: function to print bitArray, with each ULONG_PTR separated by "|"
 * 
 * No return value
 * 
 */
VOID
printArray(PULONG_PTR bitArray, ULONG_PTR length);

