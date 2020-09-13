#ifndef PAGETRADE_H
#define PAGETRADE_H

#include "userMode-AWE-pageFile.h"


VOID
tradeFreeOrZeroPage(ULONG_PTR PFNtoTrade);

BOOLEAN
copyPage(ULONG_PTR dest, ULONG_PTR src);

VOID
tradeTransitionPage(ULONG_PTR PFNtoTrade);

BOOLEAN
tradeVA(PVOID virtualAddress);

#endif