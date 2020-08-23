#ifndef PAGEFAULT_H
#define PAGEFAULT_H

#include "userMode-AWE-pageFile.h"


/*
 * pageFault: function to simulate and handle a pagefault
 * function
 *  - gets page given a VA
 *  - if PTE exists, return SUCCESS (2)?
 *  - otherweise, dequeue from freed list and return pointer to PFN entry 
 * 
 * Returns faultStatus:
 *  - SUCCESS on successful 
 *  - ACCESS_VIOLATION on access violation ( out of bounds/ invalid permissions)
 * 
 */
faultStatus
pageFault(void* virtualAddress, PTEpermissions RWEpermissions);


#endif