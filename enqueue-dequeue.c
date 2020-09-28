#include "userMode-AWE-pageFile.h"
#include "jLock.h"

VOID 
checkAvailablePages(PFNstatus dequeuedStatus)
{

    // conditional covers zero, free, and standby (since they are smaller in the enum)
    if (dequeuedStatus <= STANDBY) {

        // calculate available pages
        ULONG_PTR availablePageCount;
        availablePageCount = zeroListHead.count + freeListHead.count + standbyListHead.count;

    
        if (availablePageCount < 10) {
            modifiedPageWriter();               // TODO: need to check ret val?
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


VOID
enqueuePage(PlistData listHead, PPFNdata PFN)
{

    //lock listHead (since listHead values are not changed/accessed until dereferenced)
    EnterCriticalSection(&(listHead->lock));

    // enqueue onto list
    enqueue( &(listHead->head), &(PFN->links) );

    // update pagecount of that list
    listHead->count++;

    #ifdef MULTITHREADING
    SetEvent(listHead->newPagesEvent);
    #endif

    // unlock listHead
    LeaveCriticalSection(&(listHead->lock));

    // set statusBits to the list we've just enqueued the page on
    PFN->statusBits = listHead - listHeads;
}


PPFNdata
dequeuePage(PlistData listHead) 
{

    PLIST_ENTRY headLink;
    headLink = &(listHead->head);

    // TODO - any list now requires synchronization

    // TODO - can be assertted rather than checked once all converted to multithreading
    // verify list has items chained to the head
    if (headLink->Flink == headLink) {

        ASSERT(listHead->count == 0);
        return NULL;

    }

    ASSERT(listHead->count != 0);

    PPFNdata returnPFN;
    PLIST_ENTRY returnLink;
    PLIST_ENTRY newFirst;
    returnLink = headLink->Flink;
    newFirst = returnLink->Flink;

    // set headLink's flink to the return item's flink
    headLink->Flink = newFirst;
    newFirst->Blink = headLink;

    // set returnLink's flink/blink to null before returning it
    returnLink->Flink = NULL;
    returnLink->Blink = NULL;

    // decrement count
    listHead->count--;

    returnPFN = CONTAINING_RECORD(returnLink, PFNdata, links);

    checkAvailablePages(returnPFN->statusBits);

    returnPFN->statusBits = NONE;

    return returnPFN;
}


PPFNdata
dequeueLockedPage(PlistData listHead, BOOLEAN returnLocked)
{

    PLIST_ENTRY headLink;
    PPFNdata headPFN;
    
    while (TRUE) {

        // check the listhead flink
        headLink = (listHead->head).Flink;

        if (headLink == &(listHead->head) ) {
            PRINT("List is empty - page cannot be dequeued\n");
            return NULL;
        }

        headPFN = CONTAINING_RECORD(headLink, PFNdata, links);

        // lock page initially from at head - however it could have been pulled off list in meantime
        acquireJLock(&(headPFN->lockBits));

        // lock list - both locks are now acquired
        EnterCriticalSection(&(listHead->lock));

        // check page remains at head of list
        if (headLink == (listHead->head).Flink) {
            break;
        }
        // - if not, release lock and try again

        // release held locks (in diverging order in terms of "hotness")
        LeaveCriticalSection(&(listHead->lock));

        releaseJLock(&(headPFN->lockBits));

    }

    PPFNdata returnPFN;
    returnPFN = dequeuePage(listHead);

    ASSERT(returnPFN == headPFN);

    LeaveCriticalSection(&(listHead->lock));

    // check "returnLocked" boolean flag
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
    headLink = &(listHead->head);

    // lock listHead
    EnterCriticalSection(&(listHead->lock));

    // verify list has items chained to the head
    if (headLink->Flink == headLink) {

        ASSERT(listHead->count == 0);

        // unlock listHead
        LeaveCriticalSection(&(listHead->lock));

        return NULL;
    }

    ASSERT(listHead->count != 0);

    PPFNdata returnPFN;
    PLIST_ENTRY returnLink;
    PLIST_ENTRY newLast;
    returnLink = headLink->Blink;
    newLast = returnLink->Blink;

    // set headLink's flink to the return item's flink
    headLink->Blink = newLast;
    newLast->Flink = headLink;

    // set returnLink's flink/blink to null before returning it
    returnLink->Flink = NULL;
    returnLink->Blink = NULL;

    // decrement count
    listHead->count--;

    // unlock listHead
    LeaveCriticalSection(&(listHead->lock));
    
    returnPFN = CONTAINING_RECORD(returnLink, PFNdata, links);

    checkAvailablePages(returnPFN->statusBits);

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

    EnterCriticalSection(&(listHeads[removePage->statusBits].lock));

    dequeueSpecific( &(removePage->links) );

    // decrement count for that list
    listHeads[removePage->statusBits].count--;

    LeaveCriticalSection(&(listHeads[removePage->statusBits].lock));

    checkAvailablePages(removePage->statusBits);

    // still holding page lock
    removePage->statusBits = NONE;

}



PVANode
dequeueVA(PlistData listHead)
{
    
    PLIST_ENTRY headLink;
    headLink = &(listHead->head);

    //lock listHead (since listHead values are not changed/accessed until dereferenced)
    EnterCriticalSection(&(listHead->lock));

    // verify list has items chained to the head
    if (headLink->Flink == headLink) {
        ASSERT(listHead->count == 0);

        // unlock listHead
        LeaveCriticalSection(&(listHead->lock));

        return NULL;
    }

    ASSERT(listHead->count != 0);

    PVANode returnVANode;
    PLIST_ENTRY returnLink;
    PLIST_ENTRY newFirst;
    returnLink = headLink->Flink;
    newFirst = returnLink->Flink;

    // set headLink's flink to the return item's flink
    headLink->Flink = newFirst;
    newFirst->Blink = headLink;

    // set returnLink's flink/blink to null before returning it
    returnLink->Flink = NULL;
    returnLink->Blink = NULL;

    // decrement count
    listHead->count--;

    // unlock listHead
    LeaveCriticalSection(&(listHead->lock));

    returnVANode = CONTAINING_RECORD(returnLink, VANode, links);

    return returnVANode;

}


VOID
enqueueVA(PlistData listHead, PVANode VANode)
{

    //lock listHead (since listHead values are not changed/accessed until dereferenced)
    EnterCriticalSection(&(listHead->lock));

    // enqueue onto list
    enqueue( &(listHead->head), &(VANode->links) );

    // update pagecount of that list
    listHead->count++;

    #ifdef MULTITHREADING
    SetEvent(listHead->newPagesEvent);
    #endif

    // unlock listHead
    LeaveCriticalSection(&(listHead->lock));

}