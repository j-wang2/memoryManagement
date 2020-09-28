#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
#include "getPage.h"
#include "pageFault.h"

BOOLEAN
tradeFreeOrZeroPage(ULONG_PTR PFNtoTrade)
{

    // get PFN metadata from PFN
    PPFNdata pageToTrade;
    pageToTrade = PFNarray + PFNtoTrade;

    acquireJLock(&(pageToTrade->lockBits));

    if (pageToTrade->statusBits != FREE && pageToTrade->statusBits != ZERO) {

        releaseJLock(&(pageToTrade->lockBits));
        PRINT ("[tradeFreeOrZeroPage] Page is no longer on free or zero list\n");
        return FALSE;

    }
    // dequeue from current list
    dequeueSpecificPage(pageToTrade);

    // sets status bits also
    enqueuePage(&quarantineListHead, pageToTrade);

    // release lock
    releaseJLock(&(pageToTrade->lockBits));

    // increment commit count, since this page is now out of circulation
    totalCommittedPages++;  // TODO- CHECK COMMIT COUNT

    return TRUE;

}


BOOLEAN
copyPage(ULONG_PTR dest, ULONG_PTR src)
{
    // map given page to the pageTradeSoureVA
    if (!MapUserPhysicalPages(pageTradeSourceVA, 1, &src)) {
        PRINT_ERROR("error remapping srcVA\n");
        return FALSE;
    }

    // map given page to the pageTradeDestVA VA
    if (!MapUserPhysicalPages(pageTradeDestVA, 1, &dest)) {
        PRINT_ERROR("error remapping destVA\n");
        return FALSE;
    }

    memcpy(pageTradeDestVA, pageTradeSourceVA, PAGE_SIZE);

    // unmap pageTradeDestVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(pageTradeDestVA, 1, NULL)) {
        PRINT_ERROR("error copying page\n");
        return FALSE;
    }

        // unmap pageTradeSoureVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(pageTradeSourceVA, 1, NULL)) {
        PRINT_ERROR("error copying page\n");
        return FALSE;
    }

    return TRUE;

}


BOOLEAN
tradeTransitionPage(ULONG_PTR PFNtoTrade)
{

    // get PFN metadata from PFN
    PPFNdata pageToTrade;
    pageToTrade = PFNarray + PFNtoTrade;

    acquireJLock(&(pageToTrade->lockBits));

    PFNstatus currStatus;
    currStatus = pageToTrade->statusBits;

    if (currStatus != STANDBY && currStatus != MODIFIED){

        releaseJLock(&(pageToTrade->lockBits));
        PRINT ("[tradeTransitionPage] Page is no longer on standby or modified list\n");

        return FALSE;
    }

    // dequeue from current list
    dequeueSpecificPage(pageToTrade);

    PPFNdata newPage;
    newPage = getPage();          // TODO - need to be able to specify preference for freed page, rather than zeroed in params to getpage

    ULONG_PTR newPFN;
    newPFN = newPage - PFNarray;


    BOOLEAN copyRes;
    copyRes = copyPage(newPFN, PFNtoTrade);

    if (copyRes == FALSE) {

        PRINT_ERROR("[tradeTransitionPage] error in page copying\n");
        return FALSE;

    }

    acquireJLock(&newPage->lockBits);

    // enqueue replacement page onto list
    enqueuePage(&listHeads[currStatus], newPage);

    // update PTE's PFN field
    PPTE currPTE;
    currPTE = PTEarray + pageToTrade->PTEindex;
    currPTE->u1.tPTE.PFN = newPFN;

    // update PFN metadata with new PTE index
    ULONG64 PTEindex;
    PTEindex = currPTE - PTEarray;
    newPage->PTEindex = PTEindex;

    newPage->pageFileOffset = pageToTrade->pageFileOffset;
    newPage->refCount = pageToTrade->refCount;         

    releaseJLock(&newPage->lockBits);

    // sets status bits
    enqueuePage(&quarantineListHead, pageToTrade);

    releaseJLock(&(pageToTrade->lockBits));

    // increment commit count, since this page is now out of circulation (in addition to the one that is just brought in)
    totalCommittedPages++;

    return TRUE;

}


BOOLEAN
tradeVA(PVOID virtualAddress)
{
    // TODO - need to lock totalCommittedPages
    if (totalCommittedPages >= totalMemoryPageLimit) {
        PRINT("[tradeVA] no remaining memory\n");
        return FALSE;
    }

    PPTE currPTE;
    currPTE = getPTE(virtualAddress);

    if (currPTE == NULL) {
        PRINT_ERROR("No PTE associated with VA %llu\n", (ULONG_PTR) virtualAddress);
        return FALSE;
    }

    PTE snapPTE;
    snapPTE = *currPTE;

    ULONG_PTR originalValidBit;
    originalValidBit = snapPTE.u1.hPTE.validBit;


    if (snapPTE.u1.hPTE.validBit == 1){

        BOOLEAN tResult;
        tResult = trimPage(virtualAddress);         // TODO: possible multithreading issues

        if (tResult == FALSE) {
            PRINT_ERROR("[pageTrade] unable to trimPage at VA %llu\n", (ULONG_PTR) virtualAddress);
            return FALSE;
        }

    }

    // resnap PTE 
    snapPTE = *currPTE;

    if (snapPTE.u1.tPTE.transitionBit == 1) {

        BOOLEAN tResult;
        tResult = tradeTransitionPage(snapPTE.u1.tPTE.PFN);

        if (tResult == FALSE) {
            PRINT_ERROR("[tradeVA] unable to trade transition page\n");
            return FALSE;
        }

        pageFault(virtualAddress, READ_ONLY);

        if (originalValidBit) {

            pageFault(virtualAddress, READ_ONLY);

        }
        return TRUE;
    }

    else {
        // no page associated
        PRINT_ERROR("[tradeVA] Address not mapped to page\n");
        return FALSE;
    }
}