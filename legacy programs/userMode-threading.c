/*
 * Usermode virtual memory program intended as a rudimentary simulation
 * of Windows OS kernelmode memory management
 * 
 * Jason Wang, August 2020
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>

#define NUM_PAGES 512
#define PAGE_SIZE 4096

typedef struct _PTE{
    ULONG64 validBit: 1;
    ULONG64 standbyBit: 1;
    ULONG64 PFN: 40;    //  page frame number (PHYSICAL)
} PTE, *PPTE;

typedef struct _PFNdata {
    LIST_ENTRY links;
    ULONG64 statusBits: 5;
} PFNdata, *PPFNdata;

typedef enum {
    ACTIVE,         // 0
    FREE,            // 1
    STANDBY,        //2
} PFNstatus;

typedef enum {
    ACCESS_VIOLATION,   // 0
    NO_FREE_PAGES,      // 1
    SUCCESS,            // 2
} faultStatus;

// Global variables
void* leafVABlock;
void* leafVABlockEnd;
PPFNdata PFNarray;
PPTE PTEarray;

// listheads
LIST_ENTRY freeListHead;
LIST_ENTRY standbyListHead;

volatile BOOLEAN threadExit; // if 0, keep going, if 1, stop threads

faultStatus
getPage(void* virtualAddress);

PPTE
getPTE(void* virtualAddress);

void
enqueue(PLIST_ENTRY listHead, PLIST_ENTRY newItem);

PLIST_ENTRY
dequeue(PLIST_ENTRY listHead);

void
dequeueSpecific(PLIST_ENTRY removeItem);

void
trimThread();

void
trimVA(void* virtualAddress);

void* 
initMemBlock(int numPages, int pageSize);

PPFNdata
initPFNarray(int numPages, int pageSize);

PPTE
initPTEarray(int numPages, int pageSize);

/*
 * function
 *  - gets page given a VA
 *  - if PTE exists, return SUCCESS (2)?
 *  - otherweise, dequeue from freed list and return pointer to PFN entry 
 */
faultStatus
getPage(void* virtualAddress)
{
    PPTE currPTE;
    currPTE = getPTE(virtualAddress);

    // invalid VA (not in range)
    if (currPTE == NULL) {
        return ACCESS_VIOLATION;
    }
    PTE tempPTE;
    tempPTE = *currPTE;

    // if PTE is in array and valid, return the PFN pointer?
    if (tempPTE.validBit == 1) {
        printf("PFN is already valid");
        return SUCCESS;
    } else {

        // if standby        
        if (tempPTE.standbyBit == 1) {

            ULONG_PTR transitionPageNum;
            transitionPageNum = tempPTE.PFN;

            PPFNdata transitionPFN;
            transitionPFN = PFNarray + transitionPageNum;

            PLIST_ENTRY tLink;
            tLink = &transitionPFN->links;

            // dequeue from standbylist & set status to active
            dequeueSpecific(tLink);
            transitionPFN->statusBits = ACTIVE;

            // set PTE valid bit to 1
            tempPTE.validBit = 1;
            tempPTE.standbyBit = 0;

            // compiler writes out as indivisible store
            * (volatile PTE *) currPTE = tempPTE;
            return 2;

        // otherwise, pull new PFN
        } else{
            printf("attempting to allocate new page from freed list \n");
            
            // dequeue a page of memory from freed list, setting status bits to active
            PPFNdata freedPFN;
            freedPFN = (PPFNdata) dequeue(&freeListHead);
            if (freedPFN == NULL) {
                fprintf(stderr, "failed to successfully dequeue PFN from freed list\n");
                return NO_FREE_PAGES;
            }
            freedPFN->statusBits = ACTIVE;
            
            // assign currPFN as calculated, and change PTE to validBit;
            ULONG currPFN = freedPFN - PFNarray;
            tempPTE.PFN = currPFN;
            tempPTE.validBit = 1;

            // compiler writes out as indivisible store
            * (volatile PTE *) currPTE = tempPTE;
            return SUCCESS;         // return value of 2; 
        }
    }
}

PPTE
getPTE(void* virtualAddress)
{
    // verify VA param is within the range of the VA block
    if (virtualAddress < leafVABlock || virtualAddress >= leafVABlockEnd) {
        fprintf(stderr, "access violation \n");
        return NULL;
    }

    // get VA's offset into the leafVABlock
    ULONG_PTR offset;
    offset = (ULONG_PTR) virtualAddress - (ULONG_PTR) leafVABlock;

    // convert offset to pagetable index
    ULONG_PTR pageTableIndex;
    pageTableIndex = offset/PAGE_SIZE;     // can i just shift by 12 bytes here instead?

    // get the corresponding page table entry from the PTE array
    PPTE currPTE;
    currPTE = PTEarray + pageTableIndex;

    return currPTE;
}

void
enqueue(PLIST_ENTRY listHead, PLIST_ENTRY newItem) 
{
    PLIST_ENTRY prevFirst;
    prevFirst = listHead->Flink;
    
    listHead->Flink = newItem;
    newItem->Blink = listHead;
    newItem->Flink = prevFirst;
    prevFirst->Blink = newItem;

    return; // should I haave a return value?
}

// PPFNdata
PLIST_ENTRY
dequeue(PLIST_ENTRY listHead) 
{

    // verify list has items chained to the head
    if (listHead->Flink == listHead) {
        fprintf(stderr, "empty list\n");
        return NULL;
    }

    PLIST_ENTRY returnItem;
    PLIST_ENTRY newFirst;
    returnItem = listHead->Flink;
    newFirst = returnItem->Flink;

    // set listhead's flink to the return item's flink
    listHead->Flink = newFirst;
    newFirst->Blink = listHead;

    // set returnItem's flink/blink to null before returning it
    returnItem->Flink = NULL;
    returnItem->Blink = NULL;

    return returnItem;
}

void
dequeueSpecific(PLIST_ENTRY removeItem)
{
    PLIST_ENTRY prev;
    prev = removeItem->Blink;

    PLIST_ENTRY next;
    next = removeItem->Flink;

    prev->Flink = next;
    next->Blink = prev;

    removeItem->Flink = NULL;
    removeItem->Blink = NULL;
    return;
}

/*
 * trimThread() - passed to CreateThread
 * 
 * only want to trim when standby/free are low
 */
// void
// trimThread()
// {

// }


/*
 * trimVA(void* VA)
 * Trims entire page corresponding to VA, called by trimming thread
 */
void
trimVA(void* virtualAddress)
{
    PPTE PTEtoTrim;
    PTEtoTrim = getPTE(virtualAddress);     //JW fix - error check

    // invalidate PTE
    PTEtoTrim->validBit = 0;

    // add given VA's page to standby list
    PPFNdata PFNtoTrim;
    PFNtoTrim = PFNarray + PTEtoTrim->PFN;

    PLIST_ENTRY linkToTrim;
    linkToTrim = &PFNtoTrim->links;
    enqueue(&standbyListHead, linkToTrim);

    PFNtoTrim->statusBits = STANDBY;
    // set standbyBit to 1
    PTEtoTrim->standbyBit = 1;
}


void* 
initMemBlock(int numPages, int pageSize)
{
    // initialize block of "memory" (unsure of type)
    void* leafVABlock;

    leafVABlock = VirtualAlloc(NULL, NUM_PAGES*PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (leafVABlock == NULL) {
        exit(-1);
    }

    leafVABlockEnd = (PVOID) ( (ULONG_PTR)leafVABlock + NUM_PAGES*PAGE_SIZE );
    return leafVABlock;
}

PPFNdata
initPFNarray(int numPages, int pageSize)
{
    // initialize PFN metadata array
    PPFNdata PFNarray;
    PFNarray = VirtualAlloc(NULL, NUM_PAGES*(sizeof(PFNdata)), MEM_COMMIT, PAGE_READWRITE);
    if (PFNarray == NULL) {
        exit(-1);
    }
    return PFNarray;
}

PPTE
initPTEarray(int numPages, int pageSize)
{    
    // initialize PTE array
    PPTE PTEarray;
    PTEarray = VirtualAlloc(NULL, NUM_PAGES*(sizeof(PTE)), MEM_COMMIT, PAGE_READWRITE);
    if (PTEarray == NULL) {
        exit(-1);
    }
    return PTEarray;
}

// 
void 
main() 
{

    // initialize memory
    leafVABlock = initMemBlock(NUM_PAGES, PAGE_SIZE);     // will be the starting VA of the memory I've allocated
    PFNarray = initPFNarray(NUM_PAGES, PAGE_SIZE);
    PTEarray = initPTEarray(NUM_PAGES, PAGE_SIZE);

    // initalize threadExit to FALSE
    threadExit = FALSE;

    // initialize startListHead, pointing to itself
    freeListHead.Flink = &freeListHead;
    freeListHead.Blink = &freeListHead;

    standbyListHead.Flink = &freeListHead;
    standbyListHead.Blink = &freeListHead;

    // add pages to freed list
    PPFNdata currPFN = PFNarray;
    for (int i = 0; i < NUM_PAGES; i++) {
        LIST_ENTRY currLink;
        currLink = currPFN->links;
        enqueue(&freeListHead, &currLink);
        currPFN->statusBits = FREE;
        currPFN++;
    }

    // Create multiple threads (since structures are initialized)

    // trimming/aging thread
    HANDLE trimHandle;
    trimHandle = CreateThread(NULL, 0, trimThread, NULL, 0, NULL);

    if (trimHandle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "could not create trimThread\n");
        return;
    }

    // accessing/faulting thread
    HANDLE faultHandle;
    faultHandle = CreateThread(NULL, 0, a, NULL, 0, NULL); // fix replace a/NULL with function name and param

    if (faultHandle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "could not create faultHandle\n");
        return;
    }


    // void* testVA = (void*) (ULONG_PTR) leafVABlock + 8;
    void* testVA = leafVABlock;

    trimVA(testVA);
    printf("trimmed page\n");
    faultStatus testStatus = getPage(testVA);
    printf("tested (VA), return status = %d\n", testStatus);

    threadExit = TRUE;
    WaitForSingleObject(trimHandle, INFINITE);
    WaitForSingleObject(faultHandle, INFINITE);

    CloseHandle(trimHandle);
    CloseHandle(faultHandle);

    //  free memory allocated
    free(leafVABlock);
    free(PFNarray);
    free(PTEarray);
}