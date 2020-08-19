#ifndef PAGEFILE_H
#define PAGEFILE_H

// #include "userMode-AWE-pageFile.h"

/*
 * writePage: function to write out a given page (associated w PFN metadata) to pagefile
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
writePage(PPFNdata PFNtoWrite);

/*
 * setPFBitIndex: function to set (change from 0 to 1) bit index of pagefile bit array
 *  - denotes that that PF offset is now in use
 * 
 * Returns ULONG_PTR:
 *  - bitIndex on success
 *  - MAXULONG_PTR on failure ( no available space)
 */
ULONG_PTR
setPFBitIndex();


/* 
 * clearPFBitIndex: function to clear (change from 1 to 0) bit index of pageFile bit array
 *  - denotes that PF offset is clear and can be used
 * 
 * No return value 
 */
VOID
clearPFBitIndex(ULONG_PTR pfVA);

#endif