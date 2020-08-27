#include "userMode-AWE-pageFile.h"


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
        fprintf(stderr, "getPTEpermissions called on an invalid PTE\n");
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
