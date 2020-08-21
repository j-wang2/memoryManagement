#include "userMode-AWE-pageFile.h"



/*
 * enqueue: function to enqueue an item at head of specified list
 *   - enqueue newItem param at head of list with listHead
 * 
 * No return value
 */
VOID
enqueue(PLIST_ENTRY listHead, PLIST_ENTRY newItem);

/*
 * enqueuePage: wrapper function for enqueue PFN to head of specified list
 *  - also updates pageCount and the statusBits of the PFN that has just been enqueued
 * 
 * No return value
 */
VOID
enqueuePage(PlistData listHead, PPFNdata PFN);

/*
 * dequeuePage: function to dequeue an item from the head of a specified list
 * 
 * Returns PLIST_ENTRY
 *  - PLIST_ENTRY returnItem on success
 *  - NULL on failure (empty list)
 */
PPFNdata
dequeuePage(PlistData listHead);

/*
 * dequeueSpecific: function to remove a specific item regardless of what list it is on
 *  - unlinks specified item from its flink and blink values
 * 
 * No return value
 */
VOID
dequeueSpecific(PLIST_ENTRY removeItem);

/*
 * dequeueSpecificPage: wrapper function for dequeueSpecific that takes a PPFN as the param
 *  - also decrements count for a list
 *  - does NOT change the PFN's status bits itself
 * 
 * No return value
 */
VOID
dequeueSpecificPage(PPFNdata removePage);