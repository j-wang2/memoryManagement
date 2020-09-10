#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
#include "getPage.h"

VOID
tradeFreeOrZeroPage(ULONG_PTR PFNtoTrade)
{

    // get PFN metadata from PFN
    PPFNdata pageToTrade;
    pageToTrade = PFNarray + PFNtoTrade;

    // dequeue from current list
    dequeueSpecificPage(pageToTrade);

    // sets status bits also
    enqueuePage(&quarantineListHead, pageToTrade);

    // TODO - need to get a new page to replace with


}


BOOLEAN
copyPage(ULONG_PTR dest, ULONG_PTR src)
{
    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(pageTradeSourceVA, 1, &src)) {
        PRINT_ERROR("error remapping srcVA\n");
        return FALSE;
    }

    // map given page to the "zero" VA
    if (!MapUserPhysicalPages(pageTradeDestVA, 1, &dest)) {
        PRINT_ERROR("error remapping destVA\n");
        return FALSE;
    }

    memcpy(pageTradeDestVA, pageTradeSourceVA, PAGE_SIZE);

    // unmap zeroVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(pageTradeDestVA, 1, NULL)) {
        PRINT_ERROR("error copying page\n");
        return FALSE;
    }

        // unmap zeroVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(pageTradeSourceVA, 1, NULL)) {
        PRINT_ERROR("error copying page\n");
        return FALSE;
    }

    return TRUE;

}


VOID
tradeTransitionPage(ULONG_PTR PFNtoTrade)
{

    // get PFN metadata from PFN
    PPFNdata pageToTrade;
    pageToTrade = PFNarray + PFNtoTrade;

    PFNstatus currStatus;
    currStatus = pageToTrade->statusBits;

    // dequeue from current list
    dequeueSpecificPage(pageToTrade);

    PPFNdata newPage;
    newPage = getPage();          // TODO - need to be able to specify preference for freed page, rather than zeroed in params to getpage

    ULONG_PTR newPFN;
    newPFN = newPage - PFNarray;

    copyPage(newPFN, PFNtoTrade);

    // enqueue replacement page onto list
    enqueuePage(&listHeads[currStatus], newPage);

    // update PTE's PFN field
    PPTE currPTE;
    currPTE = PTEarray + pageToTrade->PTEindex;

    currPTE->u1.tPTE.PFN = newPFN;

    // TODO: update PFN's PTE index

    ULONG64 PTEindex;
    PTEindex = currPTE - PTEarray;
    newPage->PTEindex = PTEindex;

    // sets status bits
    enqueuePage(&quarantineListHead, pageToTrade);

}


VOID
tradeActivePage(ULONG_PTR PFNtoTrade)
{
    PPFNdata pageToTrade;
    pageToTrade = PFNarray + PFNtoTrade;

    PFNstatus currStatus;
    currStatus = pageToTrade->statusBits;

    PPFNdata newPage;
    newPage = getPage();          // TODO - need to be able to specify preference for freed page, rather than zeroed in params to getpage

    ULONG_PTR newPFN;
    newPFN = newPage - PFNarray;

    copyPage(newPFN, PFNtoTrade);

    newPage->statusBits = ACTIVE;

    // update PTE's PFN field
    PPTE currPTE;
    currPTE = PTEarray + pageToTrade->PTEindex;

    currPTE->u1.tPTE.PFN = newPFN;

    // TODO: update PFN's PTE index
    ULONG64 PTEindex;
    PTEindex = currPTE - PTEarray;
    newPage->PTEindex = PTEindex;

    // sets status bits
    enqueuePage(&quarantineListHead, pageToTrade);
}

BOOLEAN
tradeVA(PVOID virtualAddress)
{
    PPTE currPTE;
    currPTE = getPTE(virtualAddress);

    if (currPTE == NULL) {
        PRINT_ERROR("No PTE associated with VA %llu\n", (ULONG_PTR) virtualAddress);
        return FALSE;
    }

    PTE snapPTE;
    snapPTE = *currPTE;

    if (snapPTE.u1.hPTE.validBit == 1){
        trimPage(virtualAddress);
        // tradeActivePage(snapPTE.u1.hPTE.PFN);
        // return TRUE;
    }
    if (snapPTE.u1.tPTE.transitionBit == 1) {
        tradeTransitionPage(snapPTE.u1.hPTE.PFN);

        if (snapPTE.u1.hPTE.validBit == 1) {
            pageFault(virtualAddress, READ_ONLY);
        }
        return TRUE;
    }
    else {
        // no page associated
        return FALSE;
    }
}