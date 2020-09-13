#ifndef ENQUEUE_DEQUEUE_H
#define ENQUEUE_DEQUEUE_H

// #include "userMode-AWE-pageFile.h"



/*
 * checkAvailablePages: function to check and maintain available pages
 *  - if page dequeued is on zero/free/standby, checks total # of pages on zero/free/standby lists
 *  - if total # < 10, runs modifiedPageWriter
 * 
 * No return value 
 */

VOID 
checkAvailablePages(PFNstatus dequeuedStatus);


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
 *  - checks number of available pages, running modified writer if low (TODO: add active trimming as well)
 * 
 * Returns PLIST_ENTRY
 *  - PLIST_ENTRY returnItem on success
 *  - NULL on failure (empty list)
 */
PPFNdata
dequeuePage(PlistData listHead);


/*
 * dequeuePageFromTail: function to dequeue an item from the tail of a specified list
 *  - same as dequeuePage, but just removes from the tail
 *  - checks number of available pages, running modified writer if low (TODO: add active trimming as well)
 * 
 * Returns PLIST_ENTRY
 *  - PLIST_ENTRY returnItem on success
 *  - NULL on failure (empty list)
 */
PPFNdata
dequeuePageFromTail(PlistData listHead);


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
 *  - checks number of available pages, running modified writer if low (TODO: add active trimming as well)
 * 
 * No return value
 */
VOID
dequeueSpecificPage(PPFNdata removePage);



PVANode
dequeueVA(PlistData listHead);


VOID
enqueueVA(PlistData listHead, PVANode VANode);

#endif
