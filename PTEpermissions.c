#include "userMode-AWE-pageFile.h"
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
