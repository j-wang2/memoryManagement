#ifndef PTEPERMISSIONS_H
#define PTEPERMISSIONS_H

#include "../usermodeMemoryManager.h"


#ifdef PTE_CHANGE_LOG

#define LOG_ARRAY_SIZE 0x8000

typedef struct _PTETrace {

    PPTE dest;
    PTE oldPTE;
    PTE newPTE;

    ULONG64 padding;
    PFNdata PFN;

    PVOID stackTrace[4];

} PTETrace, *PPTETrace;

//
// PTEHistoryLog - array of PTETraces used for debugging purposes should
// PTE_CHANGE_LOG be toggled on
//

PTETrace PTEHistoryLog[LOG_ARRAY_SIZE];

//
// Current index in the log (inclusive, meaning that this index is most 
// current)
//

LONG currLogIndex; 

/*
 * logEntry: function to log PTE/PFN/etc data to PTEHistoryLog
 *  - only called if PTE_CHANGE_LOG is defined
 * 
 * No return value
 */
VOID
logEntry(PPTE dest, PTE oldValue, PTE newValue, PPFNdata currPage);


#endif


/*
 * writePTE: function to write PTE value to destination PTE pointer
 *  - if PTE_CHANGE_LOG is defined, also logs data to PTEHistoryLog
 *    for debugging purposes
 * 
 * No return value 
 */
VOID
writePTE(PPTE dest, PTE value);


/*
 * trimPTE: function to trim a PTE from active->transition
 *  - called either directly or by trimVA
 * 
 * Returns BOOLEAN:
 *  - TRUE if success
 *  - FALSE if failure
 */
BOOLEAN
trimPTE(PPTE PTEaddress);


/*
 * getPTE: function to find corresponding PTE from a given VA
 * 
 * Returns PPTE
 *  - currPTE (corresponding PTE) on success
 *  - NULL on failure (access violation, VA outside of range)
 */
PPTE
getPTE(void* virtualAddress);


/*
 * getPTEpermissions: function to convert input VALID PTE's permissions (as separate bits) into a return PTEpermissions (3 consecutive bits)
 * 
 * Returns PTEpermissions
 *  - PTEpermissions enum on success (must be a valid PTE input)
 *  - NO_ACCESS on failure
 */
PTEpermissions
getPTEpermissions(PTE curr);


/*
 * transferPTEpermissions: function to convert input PTEpermissions into destionation (valid) PTE separate bits
 * 
 * Returns BOOLEAN
 *  - TRUE on success
 *  - FALSE on failure - also breaks into debugger (DebugBreak)
 * 
 */
VOID
transferPTEpermissions(PPTE activeDest, PTEpermissions sourceP);


/*
 * checkPTEpermissions: function to check current PTE permissions with requested permissions
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
checkPTEpermissions(PTEpermissions currP, PTEpermissions checkP);

#endif