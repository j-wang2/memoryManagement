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
    // enqueue onto list
    enqueue( &(listHead->head), &(PFN->links) );

    // update pagecount of that list
    listHead->count++;

    // set statusBits to the list we've just enqueued the page on
    PFN->statusBits = listHead - listHeads;
}

PPFNdata
dequeuePage(PlistData listHead) 
{

    PLIST_ENTRY headLink;
    headLink = &(listHead->head);

    // verify list has items chained to the head
    if (headLink->Flink == headLink) {
        ASSERT(listHead->count == 0);

        fprintf(stderr, "empty list\n");
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
