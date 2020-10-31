#include "userMode-AWE-pageFile.h"


VOID
setBitRange(BOOLEAN isSet, ULONG_PTR startBitIndex, ULONG_PTR numPages, PULONG_PTR bitArray);


ULONG_PTR
reserveBitRange(ULONG_PTR bits, PULONG_PTR bitArray, ULONG_PTR bitArraySize);


VOID
printArray(PULONG_PTR bitArray, ULONG_PTR length);

