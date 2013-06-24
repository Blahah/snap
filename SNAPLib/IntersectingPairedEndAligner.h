/*++

Module Name:

    IntersectingPairedEndAligner.h

Abstract:

    A paired-end aligner based on set intersections to narrow down possible candidate locations.

Authors:

    Bill Bolosky, February, 2013

Environment:

    User mode service.

Revision History:

--*/

#pragma once

#include "PairedEndAligner.h"
#include "BaseAligner.h"
#include "BigAlloc.h"
#include "directions.h"
#include "LandauVishkin.h"
#include "FixedSizeMap.h"

const unsigned DEFAULT_INTERSECTING_ALIGNER_MAX_HITS = 16000;

class IntersectingPairedEndAligner : public PairedEndAligner
{
public:
    IntersectingPairedEndAligner(
        GenomeIndex  *index_,
        unsigned      maxReadSize_,
        unsigned      maxHits_,
        unsigned      maxK_,
        unsigned      maxSeeds_,
        unsigned      minSpacing_,                 // Minimum distance to allow between the two ends.
        unsigned      maxSpacing_,                 // Maximum distance to allow between the two ends.
        unsigned      maxBigHits_,
        unsigned      extraSearchDepth_,
        BigAllocator  *allocator);

    void setLandauVishkin(
        LandauVishkin<1> *landauVishkin_,
        LandauVishkin<-1> *reverseLandauVishkin_) 
    {
        landauVishkin = landauVishkin_;
        reverseLandauVishkin = reverseLandauVishkin_;
    }
    
    virtual ~IntersectingPairedEndAligner();
    
    virtual void align(
        Read                  *read0,
        Read                  *read1,
        PairedAlignmentResult *result);

//    void *operator new(size_t size, BigAllocator *allocator) {_ASSERT(size == sizeof(IntersectingPairedEndAligner)); return allocator->allocate(size);}
//    void operator delete(void *ptr, BigAllocator *allocator) {/*Do nothing, the owner of the allocator is responsible for freeing memory*/}

    static size_t getBigAllocatorReservation(GenomeIndex * index, unsigned maxHitsToConsider, unsigned maxReadSize, unsigned seedLen, unsigned maxSeedsToUse);

     virtual _int64 getLocationsScored() const {
         return nLocationsScored;
     }

private:

    IntersectingPairedEndAligner() {}  // This is for the counting allocator, it doesn't build a useful object

    static const int NUM_SET_PAIRS = 2;         // A "set pair" is read0 FORWARD + read1 RC, or read0 RC + read1 FORWARD.  Again, it doesn't make sense to change this.

    void allocateDynamicMemory(BigAllocator *allocator, unsigned maxReadSize, unsigned maxHitsToConsider, unsigned maxSeedsToUse);

    GenomeIndex *   index;
    const Genome *  genome;
    unsigned        genomeSize;
    unsigned        maxReadSize;
    unsigned        maxHits;
    unsigned        maxBigHits;
    unsigned        extraSearchDepth;
    unsigned        maxK;
    unsigned        maxSeeds;
    static const unsigned MAX_MAX_SEEDS = 30;
    unsigned        minSpacing;
    unsigned        maxSpacing;
    unsigned        seedLen;
    unsigned        maxMergeDistance;
    _int64         nLocationsScored;


    struct HashTableLookup {
        unsigned        seedOffset;
        unsigned        nHits;
        const unsigned  *hits;
		bool			beginsDisjointHitSet;

        unsigned        currentHitForIntersection;
     };
    
    //
    // A set of seed hits, represented by the lookups that came out of the big hash table.
    //
    class HashTableHitSet {
    public:
        HashTableHitSet(unsigned maxSeeds_, unsigned maxMergeDistance_);

        //
        // Reset to empty state.
        //
        void init();

        //
        // Record a hash table lookup.  All recording must be done before any
        // calls to getNextHitLessThanOrEqualTo.  A disjoint hit set is a set of hits
		// that don't share any bases in the read.  This is interesting because the edit
		// distance of a read must be at least the number of seeds that didn't hit for
		// any disjoint hit set (because there must be a difference in the read within a
		// seed for it not to hit, and since the reads are disjoint there can't be a case
		// where the same difference caused two seeds to miss).
        //
        void recordLookup(unsigned seedOffset, unsigned nHits, const unsigned *hits, bool beginsDisjointHitSet);

        //
        // This efficiently works through the set looking for the next hit at or below this address.
        // A HashTableHitSet only allows a single iteration through its address space per call to
        // init().
        //
        bool    getNextHitLessThanOrEqualTo(unsigned maxGenomeOffsetToFind, unsigned *actualGenomeOffsetFound, unsigned *seedOffsetFound, unsigned *bestPossibleScore);

        //
        // Walk down just one step, don't binary search.
        //
        bool getNextLowerHit(unsigned *genomeLocation, unsigned *seedOffsetFound, unsigned *bestPossibleScore);


        //
        // Find the highest genome address.
        //
        bool    getFirstHit(unsigned *genomeLocation, unsigned *seedOffsetFound, unsigned *bestPossibleScore);

    private:

        HashTableLookup *lookups;
        unsigned        maxSeeds;
        unsigned        nLookupsUsed;
        unsigned        mostRecentLocationReturned;
		unsigned		maxMergeDistance;

		static inline bool isWithin(unsigned a, unsigned b, unsigned distance)
		{
			return a <= b && a+distance >= b || a >= b && a <= b + distance;
		}

		unsigned computeBestPossibleScoreForCurrentHit();
    };

    HashTableHitSet *hashTableHitSets[NUM_READS_PER_PAIR][NUM_DIRECTIONS];
    unsigned        countOfHashTableLookups[NUM_READS_PER_PAIR];
    unsigned        totalHashTableHits[NUM_READS_PER_PAIR][NUM_DIRECTIONS];
    unsigned        largestHashTableHit[NUM_READS_PER_PAIR][NUM_DIRECTIONS];
    unsigned        readWithMoreHits;
    unsigned        readWithFewerHits;

    //
    // A location that's been scored (or waiting to be scored).  This is needed in order to do merging
    // of close-together hits and to track potential mate pairs.
    //
    struct HitLocation {
        unsigned        genomeLocation;
        int             genomeLocationOffset;   // This is needed because we might get an offset back from scoring (because it's really scoring a range).
        unsigned        seedOffset;
        bool            isScored;           // Mate pairs are sometimes not scored when they're inserted, because they
        unsigned        score;
        unsigned        maxK;               // The maxK that this was scored with (we may need to rescore if we need a higher maxK and score is -1)
        double          matchProbability;
		unsigned		bestPossibleScore;

        //
        // We have to be careful in the case where lots of offsets in a row match well against the read (think
        // about repetitive short sequences, i.e., ATTATTATTATT...).  We want to merge the close ones together,
        // but if the repetitive sequence extends longer than maxMerge, we don't want to just slide the window
        // over the whole range and declare it all to be one.  There is really no good definition for the right
        // thing to do here, so instead all we do is that when we declare two candidates to be matched we
        // pick one of them to be the match primary and then coalesce all matches that are within maxMatchDistance
        // of the match primary.  No one can match with any of the locations in the set that's beyond maxMatchDistance
        // from the set primary.  This means that in the case of repetitve sequences that we'll declare locations
        // right next to one another not to be matches.  There's really no way around this while avoiding
        // matching things that are possibly much more than maxMatchDistance apart.
        //
        unsigned        genomeLocationOfNearestMatchedCandidate;
    };

    class HitLocationRingBuffer {
    public:
        HitLocationRingBuffer(unsigned bufferSize_) : bufferSize(bufferSize_), head(0), tail(0)
        {
            buffer = new HitLocation[bufferSize];
        }

        ~HitLocationRingBuffer()
        {
            delete [] buffer;
        }

        bool isEmpty() {return head == tail;}

        void insertHead(unsigned genomeLocation, unsigned seedOffset, unsigned bestPossibleScore) 
        {
            _ASSERT((head + 1 ) % bufferSize != tail);  // Not overflowing
            _ASSERT(head == tail || genomeLocation < buffer[(head + bufferSize - 1)%bufferSize].genomeLocation);  // Inserting in strictly descending order

            buffer[head].genomeLocation = genomeLocation;
            buffer[head].seedOffset = seedOffset;
            buffer[head].isScored = false;
            buffer[head].genomeLocationOfNearestMatchedCandidate = 0xffffffff;
			buffer[head].bestPossibleScore = bestPossibleScore;
           head = (head + 1) % bufferSize;
        }

        void insertHead(unsigned genomeLocation, unsigned seedOffset, unsigned score, double matchProbability)
        {
            insertHead(genomeLocation, seedOffset, score);
            HitLocation *insertedLocation = &buffer[(head + bufferSize - 1)%bufferSize];
            insertedLocation->isScored = true;
            insertedLocation->score = score;
            insertedLocation->matchProbability = matchProbability;
        }

        void clear() 
        {
            head = tail = 0;
        }

        void trimAboveLocation(unsigned highestGenomeLocatonToKeep)
        {
            for (; tail != head && buffer[tail].genomeLocation > highestGenomeLocatonToKeep; tail = (tail + 1) % bufferSize) {
                // This loop body intentionally left blank.
            }
        }

        HitLocation *getTail(unsigned *index) {
            if (head == tail) return NULL;
            *index = tail;
            return &buffer[tail];
        }

        HitLocation *getTail() {
            if (head == tail) return NULL;
            return &buffer[tail];
        }

        HitLocation *getHead() {
            if (head == tail) return NULL;
            //
            // Recall that head is the next place to write, so it's not filled in yet.
            // Use the previous element.
            //
            return &buffer[(head + bufferSize - 1)% bufferSize];
        }

        HitLocation *getNext(unsigned *index)  // Working from tail to head
        {
            if ((*index + 1) % bufferSize == head) return NULL;
            *index = (*index + 1) % bufferSize;
             return &buffer[*index];
        }

    private:

        unsigned        bufferSize;
        unsigned        head;
        unsigned        tail;
        HitLocation     *buffer;
    };

    //
    // Ring buffers to keep track of the recent hits in the smaller and mate reads.  The buffer for the smaller read is just used to
    // check for hits to merge (i.e., places close enough together that we assume that they're just offsets caused by indels in the read
    // and don't treat them as separate for purposes of computing MAPQ).  The mate buffer is much larger and is used to find potential
    // mate candidates as well as for merging.
    //

    HitLocationRingBuffer    *hitLocations[NUM_SET_PAIRS];
    HitLocationRingBuffer    *mateHitLocations[NUM_SET_PAIRS];

 
    char *rcReadData[NUM_READS_PER_PAIR];                   // the reverse complement of the data for each read
    char *rcReadQuality[NUM_READS_PER_PAIR];                // the reversed quality strings for each read
    unsigned readLen[NUM_READS_PER_PAIR];

    Read *reads[NUM_READS_PER_PAIR][NUM_DIRECTIONS];        // These are the reads that are provided in the align call, together with their reverse complements, which are computed.
    Read rcReads[NUM_READS_PER_PAIR][NUM_DIRECTIONS];

    char *reversedRead[NUM_READS_PER_PAIR][NUM_DIRECTIONS]; // The reversed data for each read for forward and RC.  This is used in the backwards LV

    LandauVishkin<> *landauVishkin;
    LandauVishkin<-1> *reverseLandauVishkin;

    char rcTranslationTable[256];
    unsigned nTable[256];

    BaseAligner *baseAligner;

    BYTE *seedUsed;

    inline bool IsSeedUsed(unsigned indexInRead) const {
        return (seedUsed[indexInRead / 8] & (1 << (indexInRead % 8))) != 0;
    }

    inline void SetSeedUsed(unsigned indexInRead) {
        seedUsed[indexInRead / 8] |= (1 << (indexInRead % 8));
    }

    void alignWithBaseAligner(Read *read0, Read *read1, PairedAlignmentResult *result, int maxMapq);

    //
    // "Local probability" means the probability that each end is correct given that the pair itself is correct.
    // Consider the example where there's exactly one decent match for one read, but the other one has several
    // that are all within the correct range for the first one.  Then the local probability for the second read
    // is lower than the first.  The overall probability of an alignment then is 
    // pairProbability * localProbability/ allPairProbability.
    //
    double localBestPairProbability[NUM_READS_PER_PAIR];

    void scoreLocation(
            unsigned             whichRead,
            Direction            direction,
            unsigned             genomeLocation,
            unsigned             seedOffset,
            unsigned             scoreLimit,
            unsigned            *score,
            double              *matchProbability,
            int                 *genomeLocationOffset   // The computed offset for genomeLocation (which is needed because we scan several different possible starting locations)
    );

    struct MergeAnchor {
        unsigned    locationForReadWithMoreHits;
        unsigned    locationForReadWithFewerHits;
        double      matchProbability;
        int         pairScore;

        MergeAnchor() : locationForReadWithMoreHits(InvalidGenomeLocation), locationForReadWithFewerHits(InvalidGenomeLocation), 
            matchProbability(0), pairScore(0) {}

        //
        // Returns true and sets oldMatchProbability if this should be eliminated due to a match.
        //
        bool checkMerge(unsigned newMoreHitLocation, unsigned newFewerHitLocation, double newMatchProbability, int newPairScore, 
                        double *oldMatchProbability); 
    };

};
