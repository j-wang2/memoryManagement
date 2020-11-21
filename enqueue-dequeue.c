#include "userMode-AWE-pageFile.h"
#include "jLock.h"
#include "enqueue-dequeue.h"

//
// Defines cutoff (maximum) number of pages on the modified list 
// before waking modified page writer
//

#define MODIFIED_PAGE_COUNT_THRESHOLD 10

VOID 
checkAvailablePages(PFNstatus dequeuedStatus)
{

    //
    // conditional covers zero, free, and standby (since they are smaller in the enum)
    //
    
    if (dequeuedStatus <= STANDBY) {

        ULONG_PTR availablePageCount;

        //
        // calculate available pages
        //

        availablePageCount = 0;
        
        //
        // Acquire page listhead locks in order to calculate page counts accurately
        //

        for (int i = 0; i < STANDBY + 1; i++) {

            EnterCriticalSection(&listHeads[i].lock);

            availablePageCount += listHeads[i].count;
            
        }

    
        if (availablePageCount < MIN_AVAILABLE_PAGES) {
        
            //
            // Set event, signaling trimming thread to resume trimming active pages
            // and replenish the available pages lists
            //

            BOOL bRes;
            bRes = SetEvent(wakeTrimHandle); 

            if (bRes != TRUE) {
                PRINT_ERROR("Failed to set event successfully\n");
            }

            //
            // Note: efficacy is tied to implementation of trimValidPTEThread,
            // where this could deadlock if there was no timeout while
            // waiting on this event. This scenario could occur if all
            // trimming threads were actively trimming and this dequeue 
            // was called by a pagefault, that in turn waited infinitely
            // on new page events.
            //

            ResetEvent(wakeTrimHandle);

        }

        //
        // Release locks in reverse order of acquisition
        //

        for (int i = STANDBY; i >= 0; i--) {

            LeaveCriticalSection(&listHeads[i].lock);

        }

    }
    
}


VOID
enqueue(PLIST_ENTRY listHead, PLIST_ENTRY newItem) 
{
    PLIST_ENTRY prevFirst;
    prevFirst = listHead->Flink;
    
    listHead->Flink = newItem;
    newItem->Blink = listHead;
    newItem->Flink = prevFirst;
    prevFirst->Blink = newItem;

    return;
}


BOOLEAN
enqueuePage(PlistData listHead, PPFNdata PFN)
{

    BOOLEAN wakeModifiedWriter;
    PFNstatus listStatus;

    wakeModifiedWriter = FALSE;

    //
    // Assert that PFN lock is held upon enqueue function call
    //

    ASSERT(PFN->lockBits != 0);

    //
    // PFNs being inserted to a list cannot have a set remodified bit,
    // non-zero refcount or non-null read in prog event node
    //

    ASSERT(PFN->remodifiedBit == 0);

    ASSERT(PFN->refCount == 0 && PFN->readInProgEventNode == NULL);

    listStatus = listHead - listHeads;

    //
    // If list being enqueued to is either free or zero, the PFN
    // cannot hold pagefile space/hold a pagefile offset in it's 
    // metadata
    //

    if (PTEarray != NULL && PFN->PTEindex != 0) {
    
        if (listStatus == FREE || listStatus == ZERO) {

            ASSERT(PFN->pageFileOffset == INVALID_BITARRAY_INDEX);

            #ifdef PAGEFILE_PFN_CHECK

                PPTE currPTE;
                PPageFileDebug checkEntry;

                currPTE = PTEarray + PFN->PTEindex;

                //
                // Acquire pagefile lock
                //

                EnterCriticalSection(&pageFileLock);

                for (ULONG_PTR j = 0; j < PAGEFILE_PAGES; j++) {


                    checkEntry = pageFileDebugArray + j;

                    if (checkEntry->currPTE != NULL) {

                        //
                        // This case CAN occur, where a PTE is decommitted and PFN is awaiting free,
                        // but another thread recommits and decommits at a different PTE with the same 
                        // PFN, causing this assert to hit. Therefore, this assert must be toggled on carefully
                        //

                        // ASSERT(currPTE != checkEntry->currPTE);

                    }

                }

                LeaveCriticalSection(&pageFileLock);

            #endif

        }
    }

    //
    // Acquire listHead lock (since listHead values cannot be changed/accessed accurately without lock)
    //
    
    EnterCriticalSection(&(listHead->lock));

    //
    // Enqueue PFN to list and update pagecount
    //

    enqueue( &(listHead->head), &(PFN->links) );

    listHead->count++;

    #ifdef MULTITHREADING

        //
        // If multithreading is toggled on, set a new pages event to signal
        // to free/zero page threads
        //

        SetEvent(listHead->newPagesEvent);

    #endif

    //
    // If list being enqueued to is modified list, check the 
    // pageCount - if pagecount > 10, set return value to true
    // signaling caller to wake modified page writer
    //

    if (listStatus == MODIFIED) {

        if (listHead->count > MODIFIED_PAGE_COUNT_THRESHOLD) {

            wakeModifiedWriter = TRUE;

        }
        
    }

    // unlock listHead
    LeaveCriticalSection(&(listHead->lock));

    // set statusBits to the list we've just enqueued the page on
    PFN->statusBits = listStatus;

    return wakeModifiedWriter;

}


#ifdef PAGEFILE_PFN_CHECK
BOOLEAN
enqueuePageBasic(PlistData listHead, PPFNdata PFN) {

    BOOLEAN wakeModifiedWriter;
    PFNstatus listStatus;

    wakeModifiedWriter = FALSE;

    ASSERT(PFN->lockBits != 0);

    ASSERT(PFN->remodifiedBit == 0);

    listStatus = listHead - listHeads;

    //lock listHead (since listHead values are not changed/accessed until dereferenced)
    EnterCriticalSection(&(listHead->lock));

    // enqueue onto list
    enqueue( &(listHead->head), &(PFN->links) );

    // update pagecount of that list
    listHead->count++;

    #ifdef MULTITHREADING
    SetEvent(listHead->newPagesEvent);
    #endif

    if (listStatus == MODIFIED) {

        if (listHead->count > MODIFIED_PAGE_COUNT_THRESHOLD) {

            wakeModifiedWriter = TRUE;

        }
        
    }

    // unlock listHead
    LeaveCriticalSection(&(listHead->lock));

    // set statusBits to the list we've just enqueued the page on
    PFN->statusBits = listStatus;

    return wakeModifiedWriter;
}
#endif


PPFNdata
dequeuePage(PlistData listHead) 
{

    PLIST_ENTRY headLink;
    PPFNdata returnPFN;
    PLIST_ENTRY returnLink;
    PLIST_ENTRY newFirst;

    headLink = &(listHead->head);

    //
    // Assert that list must have items chained to the head (since parent function 
    // dequeueLockedPage must check)
    //

    ASSERT(headLink->Flink != headLink);

    ASSERT(listHead->count != 0);

    returnLink = headLink->Flink;

    newFirst = returnLink->Flink;

    //
    // set headLink's flink to the return item's flink
    //

    headLink->Flink = newFirst;

    newFirst->Blink = headLink;

    //
    // set returnLink's flink/blink to null before returning to caller
    //

    returnLink->Flink = NULL;

    returnLink->Blink = NULL;

    //
    // Decrement listHead data pageCount
    //

    listHead->count--;

    //
    // Derive and return containing PFN metadata from links field
    //

    returnPFN = CONTAINING_RECORD(returnLink, PFNdata, links);

    returnPFN->statusBits = NONE;

    return returnPFN;

}


PPFNdata
dequeueLockedPage(PlistData listHead, BOOLEAN returnLocked)
{

    PLIST_ENTRY headLink;
    PPFNdata headPFN;
    PPFNdata returnPFN;
    PFNstatus dequeueStatus;
    
    while (TRUE) {

        //
        // "Peek" at the listhead's flink
        //

        headLink = (listHead->head).Flink;

        if (headLink == &listHead->head ) {

            PRINT("[dequeueLockedPage] List is empty - page cannot be dequeued\n");
            return NULL;

        }

        headPFN = CONTAINING_RECORD(headLink, PFNdata, links);

        //
        // Lock page initially from at head - however it could have been pulled off list in meantime
        //

        acquireJLock(&headPFN->lockBits);

        //
        // Lock list itself - both list and page locks have now been acquired
        //

        EnterCriticalSection(&(listHead->lock));

        //
        // Verify page remains at head of list - if so, break immediately
        //

        if (headLink == (listHead->head).Flink) {

            break;

        }

        //
        // If page does not remain at head of list, release held locks 
        // (in inverse order of acquisition) and try again
        //

        LeaveCriticalSection(&listHead->lock);

        releaseJLock(&headPFN->lockBits);

    }
    
    //
    // Dequeue page from listHead (since locked page remains at head of list)
    //

    returnPFN = dequeuePage(listHead);

    ASSERT(returnPFN == headPFN);

    LeaveCriticalSection(&listHead->lock);

    //
    // If list being dequeued from is free, zero, or standby, check
    // available pages function will check to see whether there is a
    // sufficient number of available pages. If the number of pages is 
    // insufficient, the trim event is set.
    //

    dequeueStatus = listHead - listHeads;

    checkAvailablePages(dequeueStatus);

    //
    // If returnLocked param flag is TRUE, return PFN without
    // unlocking. Otherwise, release PFN lock and return
    //

    if (returnLocked == TRUE) {

        return returnPFN;

    }

    releaseJLock(&(headPFN->lockBits));

    return returnPFN;
}


PPFNdata
dequeuePageFromTail(PlistData listHead)
{

    PLIST_ENTRY headLink;
    PPFNdata returnPFN;
    PLIST_ENTRY returnLink;
    PLIST_ENTRY newLast;

    headLink = &(listHead->head);

    //
    // Acquire listHead lock in order to check if there are pages on list
    //
     
    EnterCriticalSection(&(listHead->lock));

    //
    // Verify that list has items chained to the head. If no 
    // items are on list, assert that count reflects that, release
    // page lock and return NULL
    //

    if (headLink->Flink == headLink) {

        ASSERT(listHead->count == 0);

        //
        // Release listHead lock
        //

        LeaveCriticalSection(&listHead->lock);

        return NULL;
    }

    ASSERT(listHead->count != 0);

    returnLink = headLink->Blink;

    newLast = returnLink->Blink;

    //
    // Set headLink's flink to the return item's flink
    //

    headLink->Blink = newLast;

    newLast->Flink = headLink;

    //
    // Set returnLink's flink/blink to null before returning it
    //

    returnLink->Flink = NULL;

    returnLink->Blink = NULL;

    //
    // Decrement listhead's page count
    //

    listHead->count--;

    //
    // Release listHead lock before returning
    //

    LeaveCriticalSection(&(listHead->lock));
    
    returnPFN = CONTAINING_RECORD(returnLink, PFNdata, links);

    returnPFN->statusBits = NONE;

    return returnPFN;

}


PPFNdata
dequeueLockedPageFromTail(PlistData listHead, BOOLEAN returnLocked)
{
    PLIST_ENTRY tailLink;
    PPFNdata tailPFN;
    PPFNdata returnPFN;
    PFNstatus dequeueStatus;

    while (TRUE) {

        //
        // "Peek" at the listhead's Blink
        //

        tailLink = (listHead->head).Blink;

        if (tailLink == &(listHead->head) ) {

            PRINT("[dequeueLockedPageFromTail] List is empty - page cannot be dequeued\n");
            return NULL;
            
        }

        tailPFN = CONTAINING_RECORD(tailLink, PFNdata, links);

        //
        // Lock page initially at tail (it may have been pulled off list in meantime)
        //

        acquireJLock(&(tailPFN->lockBits));

        //
        // Lock list itself - both page and list locks have now been acquired
        //

        EnterCriticalSection(&listHead->lock);

        //
        // Verify page remains at tail of list - if so, break immediately
        //

        if (tailLink == (listHead->head).Blink) {

            break;

        }

        //
        // If page does not remain at tail of list, release held locks 
        // (in inverse order of acquisition) and try again
        //

        LeaveCriticalSection(&listHead->lock);

        releaseJLock(&tailPFN->lockBits);

    }

    //
    // Dequeue page from listHead (since locked page remains at tail of list)
    //

    returnPFN = dequeuePageFromTail(listHead);

    ASSERT(returnPFN == tailPFN);

    LeaveCriticalSection(&listHead->lock);

    //
    // If list being dequeued from is free, zero, or standby, check
    // available pages function will check to see whether there is a
    // sufficient number of available pages. If the number of pages is 
    // insufficient, the trim event is set.
    //

    dequeueStatus = listHead - listHeads;

    checkAvailablePages(dequeueStatus);

    //
    // If returnLocked param flag is TRUE, return PFN without
    // unlocking. Otherwise, release PFN lock and return
    //

    if (returnLocked == TRUE) {

        return returnPFN;

    }

    releaseJLock(&tailPFN->lockBits);

    return returnPFN;

}


VOID
dequeueSpecific(PLIST_ENTRY removeItem)
{
    PLIST_ENTRY prev;
    prev = removeItem->Blink;

    PLIST_ENTRY next;
    next = removeItem->Flink;

    prev->Flink = next;
    next->Blink = prev;

    removeItem->Flink = NULL;
    removeItem->Blink = NULL;
    return;
}


VOID
dequeueSpecificPage(PPFNdata removePage)
{

    //
    // Assert page lock is held by caller
    //

    ASSERT(removePage->lockBits != 0);

    //
    // Acquire list lock
    //

    EnterCriticalSection(&listHeads[removePage->statusBits].lock);

    dequeueSpecific(&removePage->links);

    //
    // Decrement pageCount for that list
    //

    listHeads[removePage->statusBits].count--;

    LeaveCriticalSection(&(listHeads[removePage->statusBits].lock));

    checkAvailablePages(removePage->statusBits);

    //
    // Page lock remains held on return
    //

    removePage->statusBits = NONE;

}


PVANode
dequeueVA(PlistData listHead)
{

    PLIST_ENTRY headLink;
    PVANode returnVANode;
    PLIST_ENTRY returnLink;
    PLIST_ENTRY newFirst;

    headLink = &(listHead->head);

    //
    // List must have items chained to the head (since dequeueLockedVA checks)
    //

    ASSERT(headLink->Flink != headLink);

    ASSERT(listHead->count != 0);

    returnLink = headLink->Flink;

    newFirst = returnLink->Flink;

    //
    // Set headLink's flink to the return item's flink
    //

    headLink->Flink = newFirst;

    newFirst->Blink = headLink;

    //
    // Set returnLink's flink/blink to null before returning it
    //

    returnLink->Flink = NULL;

    returnLink->Blink = NULL;

    //
    // Decrement listhead pagecount
    //

    listHead->count--;

    returnVANode = CONTAINING_RECORD(returnLink, VANode, links);

    return returnVANode;

}


PVANode
dequeueLockedVA(PlistData listHead)
{
    
    PLIST_ENTRY headLink;
    PVANode headNode;
    PVANode returnVANode;

    while (TRUE) {

        //
        // "Peek" at the listhead's flink
        //

        headLink = (listHead->head).Flink;

        if (headLink == &listHead->head) {

            PRINT("[dequeueVA] List is empty - VA node cannot be dequeued\n");
            return NULL;

        }

        headNode = CONTAINING_RECORD(headLink, VANode, links);

        //
        // Lock list to check that node remains at head of list - if so, break immediately
        //

        EnterCriticalSection(&listHead->lock);
        
        if (headLink == (listHead->head).Flink) {

            break;

        }
        
        //
        // Otherwise, release listhead lock and try again
        //

        LeaveCriticalSection(&listHead->lock);

    }

    //
    // Dequeue node from listHead (since node remains at head of list and 
    // list has remained locked)
    //

    returnVANode = dequeueVA(listHead);

    ASSERT(returnVANode == headNode);

    LeaveCriticalSection(&listHead->lock);

    return returnVANode;

}


VOID
enqueueVA(PlistData listHead, PVANode VANode)
{

    //
    // Lock listHead (since listHead values are not changed/accessed until dereferenced)
    //

    EnterCriticalSection(&listHead->lock);

    //
    // Enqueue onto list
    //

    enqueue(&listHead->head, &VANode->links);

    //
    // Update Listhead pagecount
    //

    listHead->count++;

    #ifdef MULTITHREADING

        SetEvent(listHead->newPagesEvent);

    #endif

    //
    // Release listHead lock
    //

    LeaveCriticalSection(&(listHead->lock));

}


PeventNode
dequeueEvent(PlistData listHead)
{

    PLIST_ENTRY headLink;
    PeventNode returnNode;
    PLIST_ENTRY returnLink;
    PLIST_ENTRY newFirst;

    headLink = &(listHead->head);

    //
    // List must have items chained to the head (since dequeueLockedVA checks)
    //

    ASSERT(headLink->Flink != headLink);

    ASSERT(listHead->count != 0);

    returnLink = headLink->Flink;

    newFirst = returnLink->Flink;

    //
    // Set headLink's flink to the return item's flink
    //

    headLink->Flink = newFirst;

    newFirst->Blink = headLink;

    //
    // Set returnLink's flink/blink to null before returning it
    //

    returnLink->Flink = NULL;

    returnLink->Blink = NULL;

    //
    // Decrement event node count
    //

    listHead->count--;

    returnNode = CONTAINING_RECORD(returnLink, eventNode, links);

    return returnNode;

}


PeventNode
dequeueLockedEvent(PlistData listHead)
{

    PLIST_ENTRY headLink;
    PeventNode headNode;
    PeventNode returnNode;
    BOOL bResult;

    while (TRUE) {

        //
        // "Peek" at the listhead's flink
        //

        headLink = (listHead->head).Flink;

        if (headLink == &listHead->head) {

            PRINT("[dequeueVA] List is empty - VA node cannot be dequeued\n");
            return NULL;

        }

        headNode = CONTAINING_RECORD(headLink, eventNode, links);

        //
        // Lock listHead (since listHead values are not changed/accessed until dereferenced)
        //

        EnterCriticalSection(&(listHead->lock));

        //
        // Verify node remains at head of list
        //

        if (headLink == (listHead->head).Flink) {

            break;

        }

        LeaveCriticalSection(&listHead->lock);

    }

    //
    // Dequeue event from listHead (since locked event remains at head of list)
    //

    returnNode = dequeueEvent(listHead);

    ASSERT(returnNode == headNode);

    //
    // Reset event before returning (so event will not be set initially)
    //

    bResult = ResetEvent(returnNode->event);
    
    if (bResult != TRUE) {

        PRINT_ERROR("[dequeueLockedEvent] unable to reset event\n");

    }

    LeaveCriticalSection(&listHead->lock);

    return returnNode;

}


VOID
enqueueEvent(PlistData listHead, PeventNode eventNode)
{

    //
    // Lock listHead (since listHead values are not changed/accessed until dereferenced)
    //

    EnterCriticalSection(&listHead->lock);

    //
    // enqueue onto list
    //

    enqueue(&listHead->head, &eventNode->links);

    //
    // Update listhead event count
    //

    listHead->count++;

    #ifdef MULTITHREADING

        SetEvent(listHead->newPagesEvent);

    #endif

    //
    // Release listHead lock
    //
    
    LeaveCriticalSection(&(listHead->lock));

}