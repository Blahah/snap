/*++

Module Name:

    AlignerStats.cpp

Abstract:

    Common statistics for running single & paired alignment.

Authors:

    Ravi Pandya, May, 2012

Environment:
`
    User mode service.

Revision History:

    Integrated from SingleAligner.cpp & PairedAligner.cpp

--*/

#include "stdafx.h"
#include "options.h"
#include "AlignerStats.h"

AbstractStats::~AbstractStats()
{}

AlignerStats::AlignerStats(AbstractStats* i_extra)
:
    totalReads(0),
    usefulReads(0),
    singleHits(0), 
    multiHits(0),
    notFound(0),
    errors(0),
    extra(i_extra)
{
    for (int i = 0; i <= AlignerStats::maxMapq; i++) {
        mapqHistogram[i] = 0;
        mapqErrors[i] = 0;
    }

    for (int i = 0; i < maxMaxHits; i++) {
        countOfBestHitsByWeightDepth[i] = 0;
        countOfAllHitsByWeightDepth[i] = 0;
        probabilityMassByWeightDepth[i] = 0;
    }

#ifdef  TIME_STRING_DISTANCE
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            nanosTimeInBSD[i][j] = 0;
            BSDCounts[i][j] = 0;
        }
    }
    hammingCount = 0;
    hammingNanos = 0;
#endif  // TIME_STRING_DISSTANCE
}

AlignerStats::~AlignerStats()
{
    if (extra != NULL) {
        delete extra;
    }
}

    void
AlignerStats::printHistograms(
    FILE* out)
{
    // nothing
    if (extra != NULL) {
        extra->printHistograms(out);
    }
}

    void
AlignerStats::add(
    const AbstractStats* i_other)
{
    AlignerStats* other = (AlignerStats*) i_other;
    totalReads += other->totalReads;
    usefulReads += other->usefulReads;
    singleHits += other->singleHits;
    multiHits += other->multiHits;
    notFound += other->notFound;
    errors += other->errors;
    if (extra != NULL && other->extra != NULL) {
        extra->add(other->extra);
    }
    for (int i = 0; i <= AlignerStats::maxMapq; i++) {
        mapqHistogram[i] += other->mapqHistogram[i];
        mapqErrors[i] += other->mapqErrors[i];
    }
    for (int i = 0; i < maxMaxHits; i++) {
        countOfBestHitsByWeightDepth[i] += other->countOfBestHitsByWeightDepth[i];
        countOfAllHitsByWeightDepth[i] += other->countOfAllHitsByWeightDepth[i];
        probabilityMassByWeightDepth[i] = other->probabilityMassByWeightDepth[i];
    }

#ifdef  TIME_STRING_DISTANCE
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            nanosTimeInBSD[i][j] += other->nanosTimeInBSD[i][j];
            BSDCounts[i][j] += other->BSDCounts[i][j];
        }
    }

    hammingCount += other->hammingCount;
    hammingNanos += other->hammingNanos;
#endif  // TIME_STRING_DISTANCE
}
