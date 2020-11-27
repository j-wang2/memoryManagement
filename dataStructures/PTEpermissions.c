#include "../usermodeMemoryManager.h"
#include "../coreFunctions/pageFile.h"
#include "../infrastructure/jLock.h"
#include "../infrastructure/enqueue-dequeue.h"
#include "PTEpermissions.h"


//
// Logging functions enabled only if PTE_CHANGE_LOG
// is defined
//

#ifdef PTE_CHANGE_LOG

VOID
logEntry(PPTE dest, PTE oldValue, PTE newValue, PPFNdata currPage)
{

    LONG index;
    PPTETrace currTrace;
    DWORD hash;

    //
    // Increment and "logical and" current index in order
    // to index into PTEHistoryLog array
    //

    index = InterlockedIncrementAcquire(&currLogIndex);

    index &= (LOG_ARRAY_SIZE - 1);

    //
    // Get current log (to edit) from the PTEHistorylog array
    //

    currTrace = &PTEHistoryLog[index];

    //
    // Assign fields within currTrace to parameters
    //

    currTrace->dest = dest;

    currTrace->newPTE = newValue;

    currTrace->oldPTE = oldValue;

    //
    // Zero out current stacktrace before copying in
    //

    memset(&currTrace->stackTrace, 0, sizeof(currTrace->stackTrace));

    if (currPage != NULL) {

        memcpy(&currTrace->PFN, currPage, sizeof(PFNdata));

    } else {

        memset(&currTrace->PFN, 0, sizeof(PFNdata));

    }

    hash = 0;

    //
    // Capture backtrace in stackTrace field of struct
    //

    if (RtlCaptureStackBackTrace (0, ARRAYSIZE(currTrace->stackTrace), currTrace->stackTrace, &hash ) == 0) {

        currTrace->stackTrace[0] = (PVOID) _ReturnAddress();

    }

}

#endif

VOID
writePTE(PPTE dest, PTE value)
{
    
    #ifdef PTE_CHANGE_LOG

        PPFNdata currPage;

        ULONG_PTR PTEindex;

        PTEindex = dest - PTEarray;

        if (value.u1.hPTE.validBit == 1) {

            currPage = PFNarray + value.u1.hPTE.PFN;

            ASSERT(currPage->statusBits != AWAITING_FREE);

        }
        else if (value.u1.tPTE.transitionBit == 1) {

            currPage = PFNarray + value.u1.tPTE.PFN;

            ASSERT(currPage->statusBits != AWAITING_FREE);

        }
        else if (dest->u1.hPTE.validBit == 1) {

            currPage = PFNarray + dest->u1.hPTE.PFN;

        }
        else if (dest->u1.tPTE.transitionBit == 1) {

            currPage = PFNarray + dest->u1.tPTE.PFN;

        }
        else {

            currPage = NULL;

        }

        //
        // Either destination and new value are not valid/transition,
        // OR the current page's PTE index is consistent with the dest
        // PTE index

        // THis debug is due to dest->valid
        //->PFn index. The PFN index does not line up
        //
        //

        ASSERT(currPage == NULL || currPage->PTEindex == PTEindex);


        logEntry(dest, *dest, value, currPage);

    #endif

    * (volatile PTE *) dest = value;
    
}


BOOLEAN
trimPTE(PPTE PTEaddress) 
{

    BOOLEAN wakeModifiedWriter;
    PTE oldPTE;
    ULONG_PTR pageNum;
    PPFNdata PFNtoTrim;
    PVOID currVA;
    PTE newPTE;

    if (PTEaddress == NULL) {

        PRINT_ERROR("could not trim - invalid PTE\n");
        return FALSE;

    }
    

    wakeModifiedWriter = FALSE;

    //
    // Acquire PTE lock - although it may also be acquired in trimming function in 
    // main, the recursive nature of underlying CRITICAL_SECTION locking functionality
    // permits the re-acquisition of a critical section by the owning thread
    //

    acquirePTELock(PTEaddress);

    oldPTE = *PTEaddress;

    //
    // Check if PTE's valid bit is set - if not, can't be trimmed and return failure
    //
    
    if (oldPTE.u1.hPTE.validBit == 0) {

        releasePTELock(PTEaddress);

        PRINT("could not trim - PTE is not valid\n");
        
        return FALSE;

    }

    pageNum = oldPTE.u1.hPTE.PFN;

    PFNtoTrim = PFNarray + pageNum;

    ASSERT(PFNtoTrim->statusBits == ACTIVE);

    //
    // Initialize new PTE to zero
    //

    newPTE.u1.ulongPTE = 0;

    currVA = (PVOID) ( (ULONG_PTR) leafVABlock + (PTEaddress - PTEarray) *PAGE_SIZE );
    
    //
    // Unmap page from VA (invalidates hardwarePTE)
    //

    MapUserPhysicalPages(currVA, 1, NULL);

    //
    // Acquire page lock (prior to viewing/editing PFN fields)
    //

    acquireJLock(&PFNtoTrim->lockBits);

    //
    // If write in progress bit is set, set the page status bits to 
    // signify the modified writer to re-enqueue page. If dirty bit is 
    // clear, PFN can be re-enqueued to standby. If it is set, PFN has been
    // remodified and thus remodified bit must be set in addition to 
    // setting status bits to modified.
    //
    // PFN refCount (as a proxy for any finishing reads), must also be checked
    // similarly
    //

    if (PFNtoTrim->writeInProgressBit == 1 || PFNtoTrim->refCount != 0) {

        if (oldPTE.u1.hPTE.dirtyBit == 0) {

            PFNtoTrim->statusBits = STANDBY;

        }
        else {
            
            ASSERT(oldPTE.u1.hPTE.dirtyBit == 1);

            //
            // Notify modified writer that page has been re-modified since initial write began
            //

            PFNtoTrim->remodifiedBit = 1;

            PFNtoTrim->statusBits = MODIFIED;

        }

    }
    else {

        //
        // If refCount is zero, readInProgress bit must also be zero
        //

        ASSERT(PFNtoTrim->readInProgressBit == 0);

        //
        // Check dirtyBit to see if page has been modified
        //

        if (oldPTE.u1.hPTE.dirtyBit == 0 && PFNtoTrim->remodifiedBit == 0) {

            //
            // Add given VA's page to standby list
            //

            enqueuePage(&standbyListHead, PFNtoTrim);

        } 
        else {

            if (PFNtoTrim->pageFileOffset != INVALID_BITARRAY_INDEX) {

                ASSERT(PFNtoTrim->remodifiedBit == 1);

                clearPFBitIndex(PFNtoTrim->pageFileOffset);

                PFNtoTrim->pageFileOffset = INVALID_BITARRAY_INDEX;
                
            }

            //
            // Since PTE dirty bit is set, we can also clear PFN remodified bit and 
            // enqueue to modified list
            //

            PFNtoTrim->remodifiedBit = 0;

            //
            // Add given VA's page to modified list
            //

            wakeModifiedWriter = enqueuePage(&modifiedListHead, PFNtoTrim);

        }
    }

    //
    // Set PTE transitionBit to 1, assign PFN and permissions,
    // and write out
    //

    newPTE.u1.tPTE.transitionBit = 1;  

    newPTE.u1.tPTE.PFN = pageNum;

    newPTE.u1.tPTE.permissions = getPTEpermissions(oldPTE);

    writePTE(PTEaddress, newPTE);

    //
    // Release PFN and PTE lock in order of acquisition
    //

    releaseJLock(&PFNtoTrim->lockBits);

    releasePTELock(PTEaddress);

    if (wakeModifiedWriter == TRUE) {

        BOOL bRes;

        bRes = SetEvent(wakeModifiedWriterHandle);

        if (bRes != TRUE) {

            PRINT_ERROR("[trimPTE] failed to set event\n");

        }

        ResetEvent(wakeModifiedWriterHandle);

    }

    return TRUE;

}


PPTE
getPTE(void* virtualAddress)
{

    ULONG_PTR offset;
    ULONG_PTR pageTableIndex;
    PPTE currPTE;

    //
    // Verify VA param is within the range of the VA block
    //

    if (virtualAddress < leafVABlock || virtualAddress >= leafVABlockEnd) {

        PRINT("[getPTE] Not within allocated VA block \n");
        return NULL;

    }

    //
    // Get VA's offset into the leafVABlock
    //

    offset = (ULONG_PTR) virtualAddress - (ULONG_PTR) leafVABlock;

    //
    // Convert offset to pagetable index, dividing offset by PAGE_SIZE
    //

    pageTableIndex = offset >> PAGE_SHIFT;

    //
    // Get the corresponding page table entry from the PTE array
    //

    currPTE = PTEarray + pageTableIndex;

    return currPTE;

}


PTEpermissions
getPTEpermissions(PTE curr)
{

    //
    // If PTE is valid format, interpret bits individually in order
    // to return accurate permissions
    //

    if (curr.u1.hPTE.validBit == 1) {

        if (curr.u1.hPTE.writeBit && curr.u1.hPTE.executeBit) {

            return READ_WRITE_EXECUTE;

        } else if (curr.u1.hPTE.writeBit) {

            return READ_WRITE;

        } else if (curr.u1.hPTE.executeBit) {

            return READ_EXECUTE;

        } else {

            return READ_ONLY;

        }

    } else {

        PRINT_ERROR("getPTEpermissions called on an invalid PTE\n");
        return NO_ACCESS;

    }
}


VOID
transferPTEpermissions(PPTE activeDest, PTEpermissions sourceP)
{

    //
    // Transfer write permissions from source to dest (active) PTE
    //

    if (permissionMasks[sourceP] & writeMask) {

        activeDest->u1.hPTE.writeBit = 1;

    } else {

        activeDest->u1.hPTE.writeBit = 0;

    }

    //
    // Transfer execute permissions from src to dest (active) PTE
    //

    if (permissionMasks[sourceP] & executeMask) {

        activeDest->u1.hPTE.executeBit = 1;

    } else {

        activeDest->u1.hPTE.executeBit = 0;

    }

}


BOOLEAN
checkPTEpermissions(PTEpermissions currP, PTEpermissions checkP) 
{

    //
    // Get mask for permissions to check and perform "logical and" operation
    // between it and current mask
    //

    if ( (permissionMasks[checkP] & permissionMasks[currP] ) != permissionMasks[checkP]) {

        return FALSE;

    }

    return TRUE;

}
