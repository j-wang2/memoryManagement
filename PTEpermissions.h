#ifndef PTEPERMISSIONS_H
#define PTEPERMISSIONS_H

// #include "userMode-AWE-pageFile.h"


/*
 * getPTEpermissions: function to convert input PTE's permissions (as separate bits) into a return PTEpermissions (3 consecutive bits)
 * 
 * Returns PTEpermissions
 *  - PTEpermissions enum on success
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