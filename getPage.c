#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"


PPFNdata
getZeroPage()
{
    PPFNdata returnPFN;
    if (zeroListHead.count != 0) {

        returnPFN = dequeuePage(&zeroListHead);

        if (returnPFN == NULL) {
            PRINT_ERROR("Error in getPage(): unable to pull page off free\n");
            return NULL;
        }

        returnPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        return returnPFN;        

    } else {
        return NULL;
    }
}


PPFNdata 
getFreePage()
{
    PPFNdata returnPFN;
    if (freeListHead.count != 0) {
        returnPFN = dequeuePage(&freeListHead);

        if (returnPFN == NULL) {
            PRINT_ERROR("Error in getPage(): unable to pull page off free\n");
            return NULL;
        }

        return returnPFN;

    } else {
        return NULL;
    }
}


PPFNdata 
getStandbyPage()
{

    PPFNdata returnPFN;

    if (standbyListHead.count != 0) {

        // dequeue a page from standby list
        returnPFN = dequeuePageFromTail(&standbyListHead);

        if (returnPFN == NULL) {
            PRINT_ERROR("Error in getPage(): unable to pull page off standby\n");
            return NULL;
        }

        // get PTE
        PPTE currPTE;
        currPTE = PTEarray + returnPFN->PTEindex;       // TODO: possible multithreading compatibility issues

        // create copy of the currPTE to reference
        PTE oldPTE;
        oldPTE = *currPTE;

        //  create newPTE initialized to zero (blank slate)
        PTE newPTE;
        newPTE.u1.ulongPTE = 0;

        // if page is not already in pagefile, it MUST be a zero page (i.e. faulted into active but never written, then trimmed to standby)
        // Therefore, the PTE can be set to demand zero 
        if (returnPFN->pageFileOffset == INVALID_PAGEFILE_INDEX) {

            // copy permissions to dz format PTE
            newPTE.u1.dzPTE.permissions = oldPTE.u1.tPTE.permissions;

            // put PF index into dz format PTE
            newPTE.u1.dzPTE.pageFileIndex = INVALID_PAGEFILE_INDEX;

            returnPFN->statusBits = FREE;

            // copy newPTE back into currPTE
            * (volatile PTE *) currPTE = newPTE;
        
            return returnPFN;

        }

        // copy permissions to pf format PTE
        newPTE.u1.pfPTE.permissions = oldPTE.u1.tPTE.permissions;

        // put PF index into pf format PTE
        newPTE.u1.pfPTE.pageFileIndex = returnPFN->pageFileOffset;

        returnPFN->statusBits = FREE;

        // copy newPTE back into currPTE
        * (volatile PTE *) currPTE = newPTE;
    
        return returnPFN;

    } else {
        return NULL;
    }
} 


PPFNdata
getPage()
{

    PPFNdata returnPFN;

#ifdef CHECK_PAGEFILE
    // standby list
    returnPFN = getStandbyPage();
    if (returnPFN != NULL) {
        ULONG_PTR PFN;
        PFN = returnPFN - PFNarray;

        // TODO - fix zeroVA multithreading
        // zeroPage (does not update status bits in PFN metadata)
        zeroPage(PFN);

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        PRINT("Allocated PFN from standby list\n");

        return returnPFN;   
    }
#endif


    // Zero list
    returnPFN = getZeroPage();
    if (returnPFN != NULL) {

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        PRINT("Allocated PFN from zero list\n");

        return returnPFN;

    }

    // free list
    returnPFN = getFreePage();
    if (returnPFN != NULL) {

        ULONG_PTR PFN;
        PFN = returnPFN - PFNarray;

        // zeroPage (does not update status bits in PFN metadata)
        zeroPage(PFN);

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        PRINT("Allocated PFN from free list\n");

        return returnPFN;
    }


    // standby list
    returnPFN = getStandbyPage();
    if (returnPFN != NULL) {
        ULONG_PTR PFN;
        PFN = returnPFN - PFNarray;

        // zeroPage (does not update status bits in PFN metadata)
        zeroPage(PFN);

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        PRINT("Allocated PFN from standby list\n");

        return returnPFN;   
    }


    PRINT_ERROR("All lists empty - unable to get page\n");      // TODO - needs to be handled to avoid deadlock
    return returnPFN;                                           // should be NULL

}