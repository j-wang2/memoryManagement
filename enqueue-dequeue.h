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
 * enqueuePage: SYNCHRONIZED wrapper function for enqueue PFN to head of specified list
 *  - also updates pageCount and the statusBits of the PFN that has just been enqueued
 *  - expects page lock to be held upon call, acquires and releases listhead lock
 * No return value
 */
VOID
enqueuePage(PlistData listHead, PPFNdata PFN);

/*
 * dequeuePage: function to dequeue an item from the head of a specified list
 *  - Caller must pre-acquire locks
 *  - checks number of available pages, running modified writer if low (TODO: add active trimming as well)
 * 
 * Returns PPFNdata
 *  - PPFNdata returnPFN on success
 *  - NULL on failure (empty list)
 */
PPFNdata
dequeuePage(PlistData listHead);


/*
 * dequeueLockedPage: SYNCHRONIZED function to dequeue an item from head (adapted for multithreading)
 *  - "peeks" at top item, locks it, and then checks that it has not since been pulled off
 *  - no locks should be held upon call
 *  - acquires page lock, then list lock
 *  - BOOLEAN returnLocked flag can be set to true to not release page lock
 *  - calls dequeuePage
 * 
 * Returns PPFNdata
 *  - PPFNdata returnPFN on success (can be either unlocked or locked depending on returnLocked param)
 *  - NULL on empty list
 * 
 */
PPFNdata
dequeueLockedPage(PlistData listHead, BOOLEAN returnLocked);


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
 * dequeueLockedPageFromTail: SYNCHRONIZED function to dequeue an item from tail (adapted for multithreading)
 *  - "peeks" at tail page, locks it, and then verifies it has not since changed
 *  - no locks should be held upon call
 *  - acquires page lock, then list lock
 *  - BOOLEAN returnLocked flag can be set to true/false to release/not release page lock
 *  - calls dequeuePageFromTail
 * 
 * Returns PPFNdata
 *  - PPFNdata returnPFN on success (can be either unlocked or locked depending on returnLocked param)
 *  - NULL on empty list
 */
PPFNdata
dequeueLockedPageFromTail(PlistData listHead, BOOLEAN returnLocked);


/*
 * dequeueSpecific: function to remove a specific item regardless of what list it is on
 *  - unlinks specified item from its flink and blink values
 * 
 * No return value
 */
VOID
dequeueSpecific(PLIST_ENTRY removeItem);


/*
 * dequeueSpecificPage: SYNCHRONIZED wrapper function for dequeueSpecific that takes a PPFN as the param
 *  - also decrements count for a list
 *  - does NOT change the PFN's status bits itself
 *  - checks number of available pages, running modified writer if low (TODO: add active trimming as well)
 *  - EXPECTS page lock to be held upon call, acquires and releases listhead lock
 * 
 * No return value
 */
VOID
dequeueSpecificPage(PPFNdata removePage);


/*
 * dequeueVA: wrapper function for dequeue
 *  - decrements count of VA list
 * 
 * Returns PVANode
 *  - pointer to VANode struct on success
 *  - NULL on failure
 */
PVANode
dequeueVA(PlistData listHead);


/*
 * dequeueLockedPage: SYNCHRONIZED wrapper function for dequeueVA that takes a listhead
 *  - peeks at top item
 * 
 * REturns PVANode:
 *  - item from head on success
 *  - NULL on failure
 * 
 */
PVANode
dequeueLockedVA(PlistData listHead);

/*
 * enqueueVA: wrapper function for enqueue
 *  - increments count of VA list
 * 
 * No return value
 */
VOID
enqueueVA(PlistData listHead, PVANode VANode);


PeventNode
dequeueEvent(PlistData listHead);

PeventNode
dequeueLockedEvent(PlistData listHead);

VOID
enqueueEvent(PlistData listHead, PeventNode eventNode);

#endif
