#include "userMode-AWE-pageFile.h"

VOID
enqueue(PLIST_ENTRY listHead, PLIST_ENTRY newItem) 
{
    PLIST_ENTRY prevFirst;
    prevFirst = listHead->Flink;
    
    listHead->Flink = newItem;
    newItem->Blink = listHead;
    newItem->Flink = prevFirst;
    prevFirst->Blink = newItem;

    return; // should I haave a return value?
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


// TODO - add another state for "no longer on list"
PPFNdata
dequeuePage(PlistData listHead) 
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

    // unlock listHead
    LeaveCriticalSection(&(listHead->lock));

    returnPFN = CONTAINING_RECORD(returnLink, PFNdata, links);

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
    dequeueSpecific( &(removePage->links) );

    // decrement count for that list
    listHeads[removePage->statusBits].count--;

}
