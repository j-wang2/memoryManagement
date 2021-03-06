#ifndef VAPERMISSIONS_H
#define VAPERMISSION_H

#include "../usermodeMemoryManager.h"
#include "VADNodes.h"

/*
 * accessVA: function to access a VA, given either read or write permissions
 *  - pagefaults if not already valid
 *  - checks read/write permissions, but not execute
 * 
 * Returns faultStatus value
 *  - SUCCESS on success
 *  - ACCESS_VIOLATION on failure
 */
faultStatus
accessVA (PVOID virtualAddress, PTEpermissions RWEpermissions);


/*
 * writeVA: function to write to a VA, given PVOID data type
 *  - calls accessVA and if successful, write to VA
 * 
 * Returns faultSTatus value
 *  - SUCCESS on successful acces & write
 *  - ACCESS_VIOLATION/NO_FREE_PAGES on failure
 * 
 */
faultStatus
writeVA(PVOID virtualAddress, PVOID str);


/*
 * isVAaccessible: function to check access to a VA - DOES NOT pagefault on failure
 * 
 * Returns faultStatus value
 *  - SUCCESS on success
 *  - ACCESS_VIOLATION on failure (1)
 */
faultStatus 
isVAaccessible (PVOID virtualAddress, PTEpermissions RWEpermissions);


/*
 * commitVA: function to commit a VA
 *  - changes PTE to demand zero
 *  - checks whether there is still memory left
 * 
 * Returns boolean
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
commitVA (PVOID startVA, PTEpermissions RWEpermissions, ULONG_PTR commitSize);


/*
 * protectVA: function to update permissions for a PTE associated with a VA
 *  - updates to permissions passed in parameter newRWEpermissions
 * 
 * Returns BOOLEAN
 *  - TRUE on success
 *  - FALSE on failure
 */
BOOLEAN
protectVA(PVOID startVA, PTEpermissions newRWEpermissions, ULONG_PTR commitSize);


/*
 * trimVA(void* VA): function to trim the entire containing page corresponding to VA param
 *  - Converts from VALID format PTE to TRANSITION format PTE
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure (i.e. PTE valid bit not set)
 */
BOOLEAN
trimVA(void* virtualAddress);


/*
 * decommitVA: function to decommit a given virtual address
 * 
 * Returns BOOLEAN
 *  - TRUE on success
 *  - FALSE on failure (VA out of bounds, PTE already zero)
 */
BOOLEAN
decommitVA (PVOID startVA, ULONG_PTR commitSize);


/*
 * commitPages: function to increment the global commit count
 * by param numPages pages
 * 
 * Returns BOOLEAN
 *  - TRUE if could be successfully allocated
 *  - FALSE if wrap or no remaining pages
 * 
 */
BOOLEAN
commitPages (ULONG_PTR numPages);


/*
 * decommitPages: function to decrement global commit count by param
 * numPages pages
 * 
 * Returns BOOLEAN
 *  - TRUE if successful
 *  - FALSE if wrapping error
 * 
 */
BOOLEAN
decommitPages (ULONG_PTR numPages);


/*
 * checkDecommitted: function to determine how many PTEs in a given 
 * range have been committed
 *  - VAD "read" lock must be held to prevent changes to the VAD during call
 *  - takes param currVAD, which determines which fields to check
 * 
 * Returns ULONG_PTR
 *  - Number of PTEs/Pages within range that are decommitted
 */
ULONG_PTR
checkDecommitted(PVADNode currVAD, PPTE startPTE, PPTE endPTE);


#endif
