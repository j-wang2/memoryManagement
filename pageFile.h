#ifndef PAGEFILE_H
#define PAGEFILE_H

// #include "userMode-AWE-pageFile.h"

/*
 * setPFBitIndex: function to set (change from 0 to 1) bit index of pagefile bit array
 *  - denotes that that PF offset is now in use
 * 
 * Returns ULONG_PTR:
 *  - bitIndex on success
 *  - INVALID_BITARRAY_INDEX on failure ( no available space)
 */
ULONG_PTR
setPFBitIndex();


/* 
 * clearPFBitIndex: function to clear (change from 1 to 0) bit index of pageFile bit array
 *  - denotes that PF offset is clear and can be used
 *  - if called with INVALID_BITARRAY_INDEX, simply returns
 * 
 * No return value 
 */
VOID
clearPFBitIndex(ULONG_PTR pfVA);


/*
 * writePageToFileSystem: function to write out a given page (associated w PFN metadata) to pagefile
 *  - calls setPFBitIndex to find and set free block in pageFile
 *  - if found
 *    - copy contents of given page to pagefile location
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
writePageToFileSystem(PPFNdata PFNtoWrite, ULONG_PTR expectedSig);


/*
 * readPageFromFileSystem: function to read in a given page from file system
 * 
 * returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 * 
 */
BOOLEAN
readPageFromFileSystem(ULONG_PTR destPFN, ULONG_PTR pageFileIndex, ULONG_PTR expectedSig);

#endif