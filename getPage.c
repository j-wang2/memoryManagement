#include "userMode-AWE-pageFile.h"
#include "enqueue-dequeue.h"


PPFNdata
getZeroPage()
{
    PPFNdata returnPFN;
    if (zeroListHead.count != 0) {

        returnPFN = dequeuePage(&zeroListHead);

        if (returnPFN == NULL) {
            fprintf(stderr, "Error in getPage(): unable to pull page off free\n");
            return NULL;
        }

        returnPFN->statusBits = ZERO;                           // TODO - not sure if this or line below is necessary
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
            fprintf(stderr, "Error in getPage(): unable to pull page off free\n");
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

        returnPFN = dequeuePage(&standbyListHead);
        if (returnPFN == NULL) {
            fprintf(stderr, "Error in getPage(): unable to pull page off standby\n");
            return NULL;
        }

        // get PTE
        PPTE currPTE;
        currPTE = PTEarray + returnPFN->PTEindex;       // TODO: possible compatibility issues with multiple processes (ALSO NEED TO IMPLEMENT ON TRIMPAGE)

        // create copy of the currPTE to reference
        PTE oldPTE;
        oldPTE = *currPTE;

        //  create newPTE that is zeroed
        PTE newPTE;
        newPTE.u1.ulongPTE = 0;

        // if page is not already in pagefile, it MUST be a zero page (i.e. faulted into active but never written, then trimmed to standby)
        // Therefore, the PTE can be set to demand zero TODO
        if (returnPFN->pageFileOffset == INVALID_PAGEFILE_INDEX) {
            BOOLEAN bResult;
            bResult = writePage(returnPFN);

            if (bResult != TRUE) {
                fprintf(stderr, "error writing out page\n");
                return NULL;
            }
        }

        // copy permissions to pf format PTE
        newPTE.u1.pfPTE.permissions = oldPTE.u1.tPTE.permissions;

        // put PF index into pf format PTE
        newPTE.u1.pfPTE.pageFileIndex = returnPFN->pageFileOffset;

        // set pagefile bit
        newPTE.u1.pfPTE.pageFileBit = 1;

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

    // standby list
    returnPFN = getStandbyPage();
    if (returnPFN != NULL) {
        ULONG_PTR PFN;
        PFN = returnPFN - PFNarray;

        // zeroPage (does not update status bits in PFN metadata)
        zeroPage(PFN);

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        printf("Allocated PFN from standby list\n");

        return returnPFN;   
    }


    // Zero list
    returnPFN = getZeroPage();
    if (returnPFN != NULL) {

        // set PF offset to our "null" value in the PFN metadata
        returnPFN->pageFileOffset = INVALID_PAGEFILE_INDEX;

        printf("Allocated PFN from zero list\n");

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

        printf("Allocated PFN from free list\n");

        return returnPFN;
    }


    fprintf(stderr, "All lists empty - unable to get page\n");
    return returnPFN;                                           // should be NULL

}