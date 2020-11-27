#ifndef PAGETRADE_H
#define PAGETRADE_H

#include "../usermodeMemoryManager.h"

/*
 * Not currently in use !
 * 
 * tradeFreeOrZeroPage: function called on a free/zero page
 *  - gets PFN metadata, dequeues, enqueues, and increments total committed pages
 * 
 * No return value 
 */
VOID
tradeFreeOrZeroPage(ULONG_PTR PFNtoTrade);


/*
 * copyPage: function called to copy source PFN to dest PFN
 * 
 * Returns BOOLEAN:
 *  - TRUE on success
 *  - FALSE on failure
 * 
 */
BOOLEAN
copyPage(ULONG_PTR dest, ULONG_PTR src);


/*
 * tradeTransitionPage: function called by tradeVA on a transition page
 *  - gets PFNmetadata, dequeues, allocates new page, copies, enqueues
 *  - must also copy over relevent PFN metadata fields
 * 
 * returns BOOLEAN
 *  - TRUE on success
 *  - FALSE on failure (copyPage unsuccessful)
 */
BOOLEAN
tradeTransitionPage(ULONG_PTR PFNtoTrade);


/*
 * tradeVA: function to trade a page corresponding to a given VA
 *  - 
 * 
 * returns BOOLEAN
 *  - TRUE on success
 *  - FALSE if page not mapped or unable to trade trans page
 * 
 */
BOOLEAN
tradeVA(PVOID virtualAddress);

#endif