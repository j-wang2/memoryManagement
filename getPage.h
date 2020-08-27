#ifndef GETPAGE_H
#define GETPAGE_H

/*
 * getZeroPage: function to get a page off zero list
 *  - called by getPage
 *  - invalidates PFN pagefile index
 * 
 * Returns PPFNdata
 *  - returnPFN (pfn metadata for the returned page) on success
 *  - NULL on failure (zero list empty)
 */
PPFNdata
getZeroPage();


/*
 * getFreePage: function to get a page off free list
 *  - also zeroes page before returning
 *  - called by getPage
 * 
 * Returns PPFNdata
 *  - returnPFN (pfn metadata for the returned page) on success
 *  - NULL on failure (free list empty)
 */
PPFNdata 
getFreePage();


/*
 * getStandbyPage: function to get a page off standby list
 *  - updates PTE to pfPTE format, and zeroes page before returning
 *  - DOES NOT write out (since pulling off standby list, is either already in PF or zero)
 *  - called by getPage
 * 
 * Returns PPFNdata
 *  - returnPFN (pfn metadata for the returned page) on success
 *  - NULL on failure (standby list empty)
 */
PPFNdata 
getStandbyPage();


/*
 * getPage: function to get a page off zero/free/standby list
 * 
 * Returns PPFNdata
 *  - returnPFN (pfn metadata for the freed page) on success
 *  - NULL on failure (all lists empty)
 */
PPFNdata
getPage();


#endif