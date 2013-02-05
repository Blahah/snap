/*++

Module Name:

    mapq.h

Abstract:

    Support functions for mapping quality

Authors:

    Bill Bolosky, December, 2012

Environment:

    User mode service.

Revision History:

 
--*/

#pragma once

#include "SimilarityMap.h"
#include "directions.h"

void initializeMapqTables();

double mapqToProbability(int mapq); // The probability of a match for the given MAPQ

inline int computeMAPQ(
    double probabilityOfAllCandidates,
    double probabilityOfBestCandidate,
    int score,
    int firstPassSeedsNotSkipped[NUM_DIRECTIONS],
    unsigned smallestSkippedSeed[NUM_DIRECTIONS],
    unsigned location,
    int popularSeedsSkipped,
    SimilarityMap *similarityMap,
    unsigned biggestClusterScored,
    bool usedHamming)
{
    probabilityOfAllCandidates = __max(probabilityOfAllCandidates, probabilityOfBestCandidate); // You'd think this wouldn't be necessary, but floating point limited precision causes it to be.
    _ASSERT(probabilityOfBestCandidate >= 0.0);
    // Special case for MAPQ 70, which we generate only if there is no evidence of a mismatch at all.
    if (probabilityOfAllCandidates == probabilityOfBestCandidate && popularSeedsSkipped == 0 && score < 5 && !usedHamming) {
        return 70;
    }

    //
    // Skipped seeds are ones that the aligner didn't look at because they had too many hits during the first
    // pass through the read (i.e., they're disjoint).  Any genome location that was ignored because of
    // maxHits could have at least score of this number (because it would have to have a difference in each
    // of them).  Assume that there are as many reads as the smallest of the sets at this edit distance
    // away from the read (i.e., assume the worst case).  Use a probability of .001 for migrating an edit
    // distance (this is, of course, just a guess since it really depends on the base call qualities, etc.,
    // but since we didn't look at the genome locations at all, this will have to do).
    //
    /*
    double probabilityOfSkippedLocations = 0.0;
    if (0xffffffff != smallestSkippedSeed) {
        probabilityOfSkippedLocations = pow(.001, firstPassSeedsNotSkipped) * smallestSkippedSeed / 100000;
    }
    if (0xffffffff != smallestSkippedRCSeed) {
        probabilityOfSkippedLocations += pow(.001, firstPassRCSeedsNotSkipped) * smallestSkippedRCSeed / 100000;
    }
    probabilityOfAllCandidates += probabilityOfSkippedLocations;
    */

    double correctnessProbability = probabilityOfBestCandidate / probabilityOfAllCandidates;
    int baseMAPQ;
    if (correctnessProbability >= 1) {
        baseMAPQ =  69;
    } else {
        baseMAPQ = __min(69, (int)(-10 * log10(1 - correctnessProbability)));
    }

    // Completely arbitrary penalty for using Hamming distance, which can occasionally cause us to miss alignments.
    if (usedHamming) {
        if (baseMAPQ > 26) {
            baseMAPQ = 26;
        } else if (baseMAPQ > 10) {
            baseMAPQ--;
        }
    }

    if (similarityMap != NULL) {
        //int clusterSize = (int) similarityMap->getNumClusterMembers(location);
#ifdef TRACE_ALIGNER
        //printf("Cluster size at %u: %d\n", location, clusterSize);
#endif
        baseMAPQ = (int)__max(0, baseMAPQ - log10((double)biggestClusterScored) * 3);
    }

    //
    // Apply a penalty based on the number of overly popular seeds in the read
    //
    baseMAPQ = __max(0, baseMAPQ - __max(0, popularSeedsSkipped-10) / 2);

#ifdef TRACE_ALIGNER
    printf("computeMAPQ called at %u: score %d, pThis %g, pAll %g, result %d\n",
            location, score, probabilityOfBestCandidate, probabilityOfAllCandidates, baseMAPQ);
#endif

    return baseMAPQ;
}
