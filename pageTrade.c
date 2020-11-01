#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"
#include "getPage.h"
#include "pageFault.h"
#include "jLock.h"
#include "VApermissions.h"
#include "PTEpermissions.h"

BOOLEAN
tradeFreeOrZeroPage(ULONG_PTR PFNtoTrade)
{

    // get PFN metadata from PFN
    PPFNdata pageToTrade;
    pageToTrade = PFNarray + PFNtoTrade;

    acquireJLock(&pageToTrade->lockBits);

    if (pageToTrade->statusBits != FREE && pageToTrade->statusBits != ZERO) {

        releaseJLock(&(pageToTrade->lockBits));
        PRINT ("[tradeFreeOrZeroPage] Page is no longer free or zero \n");
        return FALSE;

    }


    if (pageToTrade->writeInProgressBit == 1) {

        // signal to zeroPageWriter to enqueue to quarantine when done
        pageToTrade->statusBits = AWAITING_QUARANTINE;

    } else {

        // dequeue from current list
        dequeueSpecificPage(pageToTrade);

        // sets status bits also
        enqueuePage(&quarantineListHead, pageToTrade);

    }

    // release lock
    releaseJLock(&(pageToTrade->lockBits));

    // increment commit count, since this page is now out of circulation
    InterlockedIncrement64(&totalCommittedPages);

    return TRUE;

}


BOOLEAN
copyPage(ULONG_PTR dest, ULONG_PTR src)
{

    PVOID sourceVA;
    PVOID destVA;

    PVANode sourceVANode;
    PVANode destVANode;

    sourceVANode = dequeueLockedVA(&pageTradeVAListHead);
    destVANode = dequeueLockedVA(&pageTradeVAListHead);

    while (sourceVANode == NULL || destVANode == NULL) {
        
        PRINT("[zeroPage] Waiting for release of event node (list empty)\n");

        WaitForSingleObject(pageTradeVAListHead.newPagesEvent, INFINITE);

        if (sourceVANode == NULL) {
            sourceVANode = dequeueLockedVA(&pageTradeVAListHead);

        } else {
            ASSERT (destVANode == NULL);
            destVANode = dequeueLockedVA(&pageTradeVAListHead);
        }

    }

    sourceVA = sourceVANode->VA;
    destVA = destVANode->VA;

    // map given page to the pageTradeSoureVA
    if (!MapUserPhysicalPages(sourceVA, 1, &src)) {
        enqueueVA(&pageTradeVAListHead, sourceVANode);
        enqueueVA(&pageTradeVAListHead, destVANode);
        PRINT_ERROR("error remapping srcVA\n");
        return FALSE;
    }

    // map given page to the pageTradeDestVA VA
    if (!MapUserPhysicalPages(destVA, 1, &dest)) {
        enqueueVA(&pageTradeVAListHead, sourceVANode);
        enqueueVA(&pageTradeVAListHead, destVANode);
        PRINT_ERROR("error remapping destVA\n");
        return FALSE;
    }

    memcpy(destVA, sourceVA, PAGE_SIZE);

    // unmap pageTradeDestVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(destVA, 1, NULL)) {
        enqueueVA(&pageTradeVAListHead, sourceVANode);
        enqueueVA(&pageTradeVAListHead, destVANode);
        PRINT_ERROR("error copying page\n");
        return FALSE;
    }

        // unmap pageTradeSoureVA from page - PFN is now ready to be alloc'd
    if (!MapUserPhysicalPages(sourceVA, 1, NULL)) {

        enqueueVA(&pageTradeVAListHead, sourceVANode);
        enqueueVA(&pageTradeVAListHead, destVANode);
        PRINT_ERROR("error copying page\n");
        return FALSE;
    }

    enqueueVA(&pageTradeVAListHead, sourceVANode);
    enqueueVA(&pageTradeVAListHead, destVANode);

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

    if (currStatus != STANDBY && currStatus != MODIFIED) {

        releaseJLock(&(pageToTrade->lockBits));
        PRINT ("[tradeTransitionPage] Page is no longer on standby or modified list\n");

        return FALSE;
    }

    // dequeue from current list
    dequeueSpecificPage(pageToTrade);

    PPFNdata newPage;
    newPage = getFreePage(FALSE);

    if (newPage == NULL) {
        newPage = getPage(FALSE);
    }

    if (newPage == NULL) {
        DebugBreak();
    }

    //
    // Todo- check if newpage isnull again (as in pagefault)
    //

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
    InterlockedIncrement64(&totalCommittedPages);

    return TRUE;

}


BOOLEAN
tradeVA(PVOID virtualAddress)
{

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


    if (snapPTE.u1.hPTE.validBit == 1) {

        BOOLEAN tResult;
        tResult = trimVA(virtualAddress);

        if (tResult == FALSE) {
            PRINT_ERROR("[pageTrade] unable to trimVA at VA %llu\n", (ULONG_PTR) virtualAddress);
            return FALSE;
        }

    }

    // resnap PTE 
    acquirePTELock(currPTE);

    snapPTE = *currPTE;

    if (snapPTE.u1.tPTE.transitionBit == 1) {

        BOOLEAN tResult;
        tResult = tradeTransitionPage(snapPTE.u1.tPTE.PFN);

        if (tResult == FALSE) {
            releasePTELock(currPTE);
            PRINT_ERROR("[tradeVA] unable to trade transition page\n");
            return FALSE;
        }


        if (originalValidBit) {

            pageFault(virtualAddress, READ_ONLY);

        }

        releasePTELock(currPTE);
        
        return TRUE;
    }

    else {

        //
        // Current VA/PTE is not mapped to a page
        //
        
        releasePTELock(currPTE);
        PRINT("[tradeVA] Address not mapped to page\n");
        return FALSE;
    }
}