#include "userMode-AWE-pageFile.h"


#define PTE_CHANGE_LOG


#ifdef PTE_CHANGE_LOG

#define LOG_ARRAY_SIZE 0x4000

typedef struct _PTETrace{

    PPTE dest;
    PTE oldPTE;
    PTE newPTE;

    PVOID stackTrace[5];

} PTETrace, *PPTETrace;

PTETrace PTEHistoryLog[LOG_ARRAY_SIZE];

LONG currLogIndex;

#endif

VOID
writePTE(PPTE dest, PTE value)
{
    
    #ifdef PTE_CHANGE_LOG

    if (value.u1.hPTE.validBit == 1) {

        PPFNdata validPage;
        ULONG_PTR PTEindex;

        PTEindex = dest - PTEarray;

        validPage = PFNarray + value.u1.hPTE.PFN;

        ASSERT(validPage->PTEindex == PTEindex);

    }


    LONG index;
    PPTETrace currTrace;


    index = InterlockedIncrementAcquire(&currLogIndex);
    index &= (LOG_ARRAY_SIZE - 1);
    currTrace = &PTEHistoryLog[index];

    currTrace->dest = dest;
    currTrace->newPTE = value;
    currTrace->oldPTE = *dest;

    memset(&currTrace->stackTrace, 0, sizeof(currTrace->stackTrace));

    currTrace->stackTrace[0] = (PVOID) _ReturnAddress();

    #endif

    * (volatile PTE *) dest = value;
    
}


PPTE
getPTE(void* virtualAddress)
{
    // verify VA param is within the range of the VA block
    if (virtualAddress < leafVABlock || virtualAddress >= leafVABlockEnd) {
        PRINT_ERROR("access violation \n");
        return NULL;
    }

    // get VA's offset into the leafVABlock
    ULONG_PTR offset;
    offset = (ULONG_PTR) virtualAddress - (ULONG_PTR) leafVABlock;

    // convert offset to pagetable index
    ULONG_PTR pageTableIndex;

    // divide offset by PAGE_SIZE
    pageTableIndex = offset >> PAGE_SHIFT;

    // get the corresponding page table entry from the PTE array
    PPTE currPTE;
    currPTE = PTEarray + pageTableIndex;

    return currPTE;
}



PTEpermissions
getPTEpermissions(PTE curr)
{
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

    // transfer write permissions from src to dest
    if (permissionMasks[sourceP] & writeMask) {
        activeDest->u1.hPTE.writeBit = 1;
    } else {
        activeDest->u1.hPTE.writeBit = 0;
    }

    // transfer execute permissions from src to dest
    if (permissionMasks[sourceP] & executeMask) {
        activeDest->u1.hPTE.executeBit = 1;
    } else {
        activeDest->u1.hPTE.executeBit = 0;
    }

}


BOOLEAN
checkPTEpermissions(PTEpermissions currP, PTEpermissions checkP) 
{

    // get mask for permissions to check and logical and it with current mask
    if ( (permissionMasks[checkP] & permissionMasks[currP] ) != permissionMasks[checkP]) {
        // DebugBreak();
        return FALSE;
    }

    return TRUE;
}
