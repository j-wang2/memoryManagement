

/*
 * enqueue: function to enqueue an item at head of specified list
 *   - enqueue newItem param at head of list with listHead
 * 
 * No return value
 */

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