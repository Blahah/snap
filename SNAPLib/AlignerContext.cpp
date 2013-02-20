/*++


Module Name:

    AlignerContext.cpp

Abstract:

    Common parameters for running single & paired alignment.

Authors:

    Ravi Pandya, May, 2012

Environment:
`
    User mode service.

Revision History:

    Integrated from SingleAligner.cpp & PairedAligner.cpp

--*/

#include "stdafx.h"
#include "Compat.h"
#include "options.h"
#include "AlignerOptions.h"
#include "AlignerContext.h"
#include "AlignerStats.h"
#include "FileFormat.h"

using std::max;
using std::min;

AlignerContext::AlignerContext(int i_argc, const char **i_argv, const char *i_version, AlignerExtension* i_extension)
    :
    index(NULL),
    writerSupplier(NULL),
    options(NULL),
    stats(NULL),
    extension(i_extension != NULL ? i_extension : new AlignerExtension()),
    readWriter(NULL),
    argc(i_argc),
    argv(i_argv),
    version(i_version)
{
}

AlignerContext::~AlignerContext()
{
    delete extension;
}

void AlignerContext::runAlignment(int argc, const char **argv, const char *version)
{
    options = parseOptions(argc, argv, version);
    initialize();
    extension->initialize();
    
    if (! extension->skipAlignment()) {
        printStatsHeader();
        do {

            beginIteration();

            runTask();
            
            finishIteration();
            
            printStats();

        } while (nextIteration());
    }

    extension->finishAlignment();
extern _int64 nanosInSingleAligner;
extern _int64 callsToSingleAligner;
extern _int64 nanosInMateAligner;
extern _int64 callsToMateAligner;
extern _int64 nanosInAlignTogether;
extern _int64 callsToAlignTogether;
printf("%lld s in %lld calls to single aligner, %lld ns/call.  %lld s in %lld calls to mate aligner, %lld ns/call.  %lld s in %lld s calls to align together, %lld ns/call\n",
       nanosInSingleAligner/1000000000,callsToSingleAligner,nanosInSingleAligner/max((_int64)1,callsToSingleAligner),nanosInMateAligner/1000000000,callsToMateAligner,nanosInMateAligner/max((_int64)1,callsToMateAligner),
       nanosInAlignTogether/1000000000,callsToAlignTogether,nanosInAlignTogether/max((_int64)1,callsToAlignTogether));
}

    void
AlignerContext::initializeThread()
{
    stats = newStats(); // separate copy per thread
    stats->extra = extension->extraStats();
    readWriter = writerSupplier != NULL ? writerSupplier->getWriter() : NULL;
    extension = extension->copy();
}

    void
AlignerContext::runThread()
{
    extension->beginThread();
    runIterationThread();
    if (readWriter != NULL) {
        readWriter->close();
        delete readWriter;
    }
    extension->finishThread();
}
    
    void
AlignerContext::finishThread(AlignerContext* common)
{
    common->stats->add(stats);
    delete stats;
    stats = NULL;
    delete extension;
    extension = NULL;
}

    void
AlignerContext::initialize()
{
    printf("Loading index from directory... ");
    fflush(stdout);
    _int64 loadStart = timeInMillis();
    index = GenomeIndex::loadFromDirectory((char*) options->indexDir);
    if (index == NULL) {
        fprintf(stderr, "Index load failed, aborting.\n");
        exit(1);
    }
    _int64 loadTime = timeInMillis() - loadStart;
    printf("%llds.  %u bases, seed size %d\n",
        loadTime / 1000, index->getGenome()->getCountOfBases(), index->getSeedLength());

    if (options->outputFileTemplate != NULL && (options->maxHits.size() > 1 || options->maxDist.size() > 1 || options->numSeeds.size() > 1
                || options->confDiff.size() > 1 || options->adaptiveConfDiff.size() > 1)) {
        fprintf(stderr, "WARNING: You gave ranges for some parameters, so SAM files will be overwritten!\n");
    }

    confDiff_ = options->confDiff.start;
    maxHits_ = options->maxHits.start;
    maxDist_ = options->maxDist.start;
    numSeeds_ = options->numSeeds.start;
    adaptiveConfDiff_ = options->adaptiveConfDiff.start;
}

    void
AlignerContext::printStatsHeader()
{
    printf("ConfDif\tMaxHits\tMaxDist\tMaxSeed\tConfAd\t%%Used\t%%Unique\t%%Multi\t%%!Found\t%%Error\tReads/s\n");
}

    void
AlignerContext::beginIteration()
{
    writerSupplier = NULL;
    // total mem in Gb if given; default 1 Gb/thread for human genome, scale down for smaller genomes
    size_t totalMemory = options->sortMemory > 0
        ? options->sortMemory * ((size_t) 1 << 30)
        : options->numThreads * max(2ULL * 16 * 1024 * 1024,
                                    (size_t) index->getGenome()->getCountOfBases() / 3);
    if (NULL != options->outputFileTemplate) {
        const FileFormat* format = 
            FileFormat::SAM[0]->isFormatOf(options->outputFileTemplate) ? FileFormat::SAM[options->useM] :
            FileFormat::BAM[0]->isFormatOf(options->outputFileTemplate) ? FileFormat::BAM[options->useM] :
            NULL;
        if (format != NULL) {
            writerSupplier = format->getWriterSupplier(options, index->getGenome());
            ReadWriter* headerWriter = writerSupplier->getWriter();
            headerWriter->writeHeader(options->sortOutput, argc, argv, version, options->rgLineContents);
            headerWriter->close();
            delete headerWriter;
        } else {
            fprintf(stderr, "warning: no output, unable to determine format of output file %s\n", options->outputFileTemplate);
        }
    }

    alignStart = timeInMillis();

    clipping = options->clipping;
    totalThreads = options->numThreads;
    computeError = options->computeError;
    bindToProcessors = options->bindToProcessors;
    maxDist = maxDist_;
    maxHits = maxHits_;
    numSeeds = numSeeds_;
    confDiff = confDiff_;
    adaptiveConfDiff = adaptiveConfDiff_;
    selectivity = options->selectivity;

    if (stats != NULL) {
        delete stats;
    }
    stats = newStats();
    stats->extra = extension->extraStats();
    extension->beginIteration();
    typeSpecificBeginIteration();
}

    void
AlignerContext::finishIteration()
{
    extension->finishIteration();

    if (NULL != writerSupplier) {
        writerSupplier->close();
        delete writerSupplier;
        writerSupplier = NULL;
    }

    alignTime = timeInMillis() - alignStart;
}

    bool
AlignerContext::nextIteration()
{
     typeSpecificNextIteration();
     if ((adaptiveConfDiff_ += options->adaptiveConfDiff.step) > options->adaptiveConfDiff.end) {
        adaptiveConfDiff_ = options->adaptiveConfDiff.start;
        if ((numSeeds_ += options->numSeeds.step) > options->numSeeds.end) {
            numSeeds_ = options->numSeeds.start;
            if ((maxDist_ += options->maxDist.step) > options->maxDist.end) {
                maxDist_ = options->maxDist.start;
                if ((maxHits_ += options->maxHits.step) > options->maxHits.end) {
                    maxHits_ = options->maxHits.start;
                    if ((confDiff_ += options->confDiff.step) > options->confDiff.end) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

    void
AlignerContext::printStats()
{
    double usefulReads = max((double) stats->usefulReads, 1.0);
    char errorRate[16];
    if (options->computeError) {
        _int64 numSingle = max(stats->singleHits, (_int64) 1);
        snprintf(errorRate, sizeof(errorRate), "%0.3f%%", (100.0 * stats->errors) / numSingle);
    } else {
        snprintf(errorRate, sizeof(errorRate), "-");
    }
    printf("%d\t%d\t%d\t%d\t%d\t%0.2f%%\t%0.2f%%\t%0.2f%%\t%0.2f%%\t%s\t%.0f\n",
            confDiff_, maxHits_, maxDist_, numSeeds_, adaptiveConfDiff_,
            100.0 * usefulReads / max(stats->totalReads, (_int64) 1),
            100.0 * stats->singleHits / usefulReads,
            100.0 * stats->multiHits / usefulReads,
            100.0 * stats->notFound / usefulReads,
            errorRate,
            (1000.0 * usefulReads) / max(alignTime, (_int64) 1));

    extension->printStats();
}

