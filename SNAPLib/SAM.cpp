/*++

Module Name:

    SAM.cpp

Abstract:

    Sequence Alignment Map (SAM) file writer and reader.

Environment:

    User mode service.

    SamWriter and SamReader (and their subclasses) aren't thread safe.

--*/

#include "stdafx.h"
#include "BigAlloc.h"
#include "Compat.h"
#include "Read.h"
#include "SAM.h"
#include "Tables.h"

using std::max;
using std::min;


//
// You'd think this would be in the C library.
// Like strchr, but with a max length so it doesn't
// run over the end of the buffer.  Basically,
// strings suck in C.
//

    char *
strnchr(char *str, char charToFind, size_t maxLen)
{
    for (size_t i = 0; i < maxLen; i++) {
        if (str[i] == charToFind) {
            return str + i;
        }
        if (str[i] == 0) {
            return NULL;
        }
    }
    return NULL;
}

    const char *
strnchr(const char *str, char charToFind, size_t maxLen)
{
    for (size_t i = 0; i < maxLen; i++) {
        if (str[i] == charToFind) {
            return str + i;
        }
        if (str[i] == 0) {
            return NULL;
        }
    }
    return NULL;
}

SAMWriter:: ~SAMWriter()
{
}


SAMWriter* SAMWriter::create(const char *fileName, const Genome *genome)
{
    SimpleSAMWriter *writer = new SimpleSAMWriter();
    if (!writer->open(fileName, genome)) {
        delete writer;
        return NULL;
    } else {
        return writer;
    }
}

    bool
SAMWriter::generateHeader(const Genome *genome, char *header, size_t headerBufferSize, size_t *headerActualSize)
{
    
    size_t bytesConsumed = snprintf(header, headerBufferSize, "@HD\tVN:1.4\tSO:unsorted\n");
    if (bytesConsumed > headerBufferSize) {
        fprintf(stderr,"SAMWriter: header buffer too small\n");
        return false;
    }

    // Write an @SQ line for each chromosome / piece in the genome
    const Genome::Piece *pieces = genome->getPieces();
    int numPieces = genome->getNumPieces();
    unsigned genomeLen = genome->getCountOfBases();
    for (int i = 0; i < numPieces; i++) {
        unsigned start = pieces[i].beginningOffset;
        unsigned end = (i + 1 < numPieces) ? pieces[i+1].beginningOffset : genomeLen;
        bytesConsumed += snprintf(header + bytesConsumed, headerBufferSize - bytesConsumed, "@SQ\tSN:%s\tLN:%u\n", pieces[i].name, end - start);

        if (bytesConsumed > headerBufferSize) {
            fprintf(stderr,"SAMWriter: header buffer too small\n");
            return false;
        }
    }

    *headerActualSize = bytesConsumed;
    return true;
}

// Compute the CIGAR edit sequence string for a read against a given genome location.
// Returns this string if possible or "*" if we fail to compute it (which would likely
// be a bug due to lack of buffer space). The pointer returned may be to cigarBuf so it
// will only be valid until computeCigarString is called again.
    const char *
SAMWriter::computeCigarString(
    const Genome *              genome,
    LandauVishkinWithCigar *    lv,
    char *                      cigarBuf,
    int                         cigarBufLen,
    char *                      cigarBufWithClipping,
    int                         cigarBufWithClippingLen,
    const char *                data,
    unsigned                    dataLength,
    unsigned                    basesClippedBefore,
    unsigned                    basesClippedAfter,
    unsigned                    genomeLocation,
    bool                        isRC
)
{
    const char *reference = genome->getSubstring(genomeLocation, dataLength);
    int r;
    if (NULL != reference) {
        r = lv->computeEditDistance(
                       genome->getSubstring(genomeLocation, dataLength),
                        dataLength,
                        data,
                        dataLength,
                        MAX_K - 1,
                        cigarBuf,
                        cigarBufLen);
    } else {
        //
        // Fell off the end of the chromosome.
        //
        return "*";
    }

    if (r == -2) {
        fprintf(stderr, "WARNING: computeEditDistance returned -2; cigarBuf may be too small\n");
        return "*";
    } else if (r == -1) {
        static bool warningPrinted = false;
        if (!warningPrinted) {
            fprintf(stderr, "WARNING: computeEditDistance returned -1; this shouldn't happen\n");
            warningPrinted = true;
        }
        return "*";
    } else {
        // Add some CIGAR instructions for soft-clipping if we've ignored some bases in the read.
        char clipBefore[16] = {'\0'};
        char clipAfter[16] = {'\0'};
        if (basesClippedBefore > 0) {
            snprintf(clipBefore, sizeof(clipBefore), "%uS", basesClippedBefore);
        }
        if (basesClippedAfter > 0) {
            snprintf(clipAfter, sizeof(clipAfter), "%uS", basesClippedAfter);
        }
        snprintf(cigarBufWithClipping, cigarBufWithClippingLen, "%s%s%s", clipBefore, cigarBuf, clipAfter);
        return cigarBufWithClipping;
    }
}


    bool
SAMWriter::generateSAMText(
                Read *                      read, 
                AlignmentResult             result, 
                unsigned                    genomeLocation, 
                bool                        isRC, 
                bool                        hasMate, 
                bool                        firstInPair, 
                Read *                      mate, 
                AlignmentResult             mateResult, 
                unsigned                    mateLocation,
                bool                        mateIsRC, 
                const Genome *              genome, 
                LandauVishkinWithCigar *    lv, 
                char *                      buffer, 
                size_t                      bufferSpace, 
                size_t *                    spaceUsed)
{
    const int MAX_READ = 10000;
    const int cigarBufSize = MAX_READ * 2;
    char cigarBuf[cigarBufSize];

    const int cigarBufWithClippingSize = MAX_READ * 2 + 32;
    char cigarBufWithClipping[cigarBufWithClippingSize];

    int flags = 0;
    const char *pieceName = "*";
    unsigned positionInPiece = 0;
    int mapQuality = 0;
    const char *cigar = "*";
    const char *matePieceName = "*";
    unsigned matePositionInPiece = 0;
    _int64 templateLength = 0;

    //
    // If the aligner said it didn't find anything, treat it as such.  Sometimes it will emit the
    // best match that it found, even if it's not within the maximum edit distance limit (but will
    // then say NotFound).  Here, we force that to be SAM_UNMAPPED.
    //
    if (NotFound == result) {
        genomeLocation = 0xffffffff;
    }

    // Write the data and quality strings. If the read is reverse complemented, these need to
    // be backwards from the original read. Also, both need to be unclipped.
    char data[MAX_READ];
    char quality[MAX_READ];
    const char *clippedData;
    unsigned clippedLength = read->getDataLength();
    unsigned fullLength = read->getUnclippedLength();
    unsigned basesClippedBefore;
    unsigned basesClippedAfter;
    if (isRC) {
      for (unsigned i = 0; i < fullLength; i++) {
        data[fullLength - 1 - i] = COMPLEMENT[read->getUnclippedData()[i]];
        quality[fullLength - 1 - i] = read->getUnclippedQuality()[i];
      }
      clippedData = &data[fullLength - clippedLength - read->getFrontClippedLength()];
      basesClippedBefore = fullLength - clippedLength - read->getFrontClippedLength();
      basesClippedAfter = read->getFrontClippedLength();
    } else {
      memcpy(data, read->getUnclippedData(), read->getUnclippedLength());
      memcpy(quality, read->getUnclippedQuality(), read->getUnclippedLength());
      clippedData = read->getData();
      basesClippedBefore = read->getFrontClippedLength();
      basesClippedAfter = fullLength - clippedLength - basesClippedBefore;
    }

    if (genomeLocation != 0xFFFFFFFF) {
        // This could be either a single hit read or a multiple hit read where we just
        // returned one location, but either way, let's print that location. We will then
        // set the quality to 60 if it was single or 0 if it was multiple. These are the
        // values the SAM FAQ suggests for aligners that don't compute confidence scores.
        if (isRC) {
            flags |= SAM_REVERSE_COMPLEMENT;
        }
        const Genome::Piece *piece = genome->getPieceAtLocation(genomeLocation);
        pieceName = piece->name;
        positionInPiece = genomeLocation - piece->beginningOffset + 1; // SAM is 1-based
        cigar = computeCigarString(genome, lv, cigarBuf, cigarBufSize, cigarBufWithClipping, cigarBufWithClippingSize, 
                                   clippedData, clippedLength, basesClippedBefore, basesClippedAfter,
                                   genomeLocation, isRC);
        mapQuality = (result == SingleHit || result == CertainHit) ? 60 : 0;
    } else {
        flags |= SAM_UNMAPPED;
    }

    if (hasMate) {
        flags |= SAM_MULTI_SEGMENT;
        flags |= (firstInPair ? SAM_FIRST_SEGMENT : SAM_LAST_SEGMENT);
        if (isOneLocation(result) && isOneLocation(mateResult)) {
            flags |= SAM_ALL_ALIGNED;
            // Also compute the length of the whole paired-end string whose ends we saw. This is slightly
            // tricky because (a) we may have clipped some bases before/after each end and (b) we need to
            // give a signed result based on whether our read is first or second in the pair.
            _int64 myStart = genomeLocation - basesClippedBefore;
            _int64 myEnd = genomeLocation + clippedLength + basesClippedAfter;
            _int64 mateBasesClippedBefore = mate->getFrontClippedLength();
            _int64 mateBasesClippedAfter = mate->getUnclippedLength() - mate->getDataLength() - mateBasesClippedBefore;
            _int64 mateStart = mateLocation - (mateIsRC ? mateBasesClippedAfter : mateBasesClippedBefore);
            _int64 mateEnd = mateLocation + mate->getDataLength() + (!mateIsRC ? mateBasesClippedAfter : mateBasesClippedBefore);
            if (myStart < mateStart) {
                templateLength = mateEnd - myStart;
            } else {
                templateLength = -(myEnd - mateStart);
            }
        }
        if (mateIsRC) {
            flags |= SAM_NEXT_REVERSED;
        }
        if (mateLocation != 0xFFFFFFFF) {
            const Genome::Piece *piece = genome->getPieceAtLocation(mateLocation);
            matePieceName = piece->name;
            matePositionInPiece = mateLocation - piece->beginningOffset + 1;
        }
    }

    if (result == MultipleHits && genomeLocation == 0xFFFFFFFF) {
        // Read was MultipleHits but we didn't return even one potential location, which means that all
        // the seeds were too popular. Report the mapping quality as 1 to let users differentiate this
        // from a NotFound read.
        mapQuality = 1;
    }

    // Write the SAM entry, which requires the following fields:
    //
    // 1. QNAME: Query name of the read or the read pair
    // 2. FLAG: Bitwise flag (pairing, strand, mate strand, etc.)
    // 3. RNAME: Reference sequence name
    // 4. POS: 1-Based leftmost position of clipped alignment
    // 5. MAPQ: Mapping quality (Phred-scaled)
    // 6. CIGAR: Extended CIGAR string (operations: MIDNSHP)
    // 7. MRNM: Mate reference name (‘=’ if same as RNAME)
    // 8. MPOS: 1-based leftmost mate position
    // 9. ISIZE: Inferred insert size
    // 10. SEQQuery: Sequence on the same strand as the reference
    // 11. QUAL: Query quality (ASCII-33=Phred base quality)    

    //
    // Some FASTQ files have spaces in their ID strings, which is illegal in SAM.  Just truncate them at the space.
    //
    unsigned readLen;
    const char *firstSpace = strnchr(read->getId(),' ',read->getIdLength());
    if (NULL == firstSpace) {
        readLen = read->getIdLength();
    } else {
        readLen = (unsigned)(firstSpace - read->getId());
    }

    int charsInString = snprintf(buffer, bufferSpace, "%.*s\t%d\t%s\t%u\t%d\t%s\t%s\t%u\t%lld\t%.*s\t%.*s\n",
        readLen, read->getId(),
        flags,
        pieceName,
        positionInPiece,
        mapQuality,
        cigar,
        matePieceName,
        matePositionInPiece,
        templateLength,
        fullLength, data,
        fullLength, quality);

    if (charsInString > bufferSpace) {
        //
        // Out of buffer space.
        //
        return false;
    }

    if (NULL != spaceUsed) {
        *spaceUsed = charsInString;
    }
    return true;
}


SimpleSAMWriter::SimpleSAMWriter()
{
    file = NULL;
}


SimpleSAMWriter::~SimpleSAMWriter()
{
    if (file != NULL) {
        close();
    }
}


bool SimpleSAMWriter::open(const char* fileName, const Genome *genome)
{
    buffer = (char *) BigAlloc(BUFFER_SIZE);
    if (buffer == NULL) {
        fprintf(stderr, "allocating write buffer failed\n");
        return false;
    }
    file = fopen(fileName, "w");
    if (file == NULL) {
        fprintf(stderr, "fopen failed\n");
        return false;
    }
    setvbuf(file, buffer, _IOFBF, BUFFER_SIZE);
    this->genome = genome;

    // Write out SAM header
    char *headerBuffer = new char[HEADER_BUFFER_SIZE];
    size_t headerSize;
    if (!generateHeader(genome,headerBuffer,HEADER_BUFFER_SIZE,&headerSize)) {
        fprintf(stderr,"SimpleSAMWriter: unable to generate SAM header\n");
        return false;
    }
    fprintf(file, "%s", headerBuffer);
    delete[] headerBuffer;

    return true;
}


bool SimpleSAMWriter::write(Read *read, AlignmentResult result, unsigned genomeLocation, bool isRC)
{
    if (file == NULL) {
        return false;
    }

    write(read, result, genomeLocation, isRC, false, false, NULL, NotFound, 0, false);
    return true;
}


bool SimpleSAMWriter::writePair(Read *read0, Read *read1, PairedAlignmentResult *result)
{
    if (file == NULL) {
        return false;
    }

    write(read0, result->status[0], result->location[0], result->isRC[0], true, true,
          read1, result->status[1], result->location[1], result->isRC[1]);
    write(read1, result->status[1], result->location[1], result->isRC[1], true, false,
          read0, result->status[0], result->location[0], result->isRC[0]);
    return true;
}


void SimpleSAMWriter::write(
        Read *read,
        AlignmentResult result,
        unsigned genomeLocation,
        bool isRC,
        bool hasMate,
        bool firstInPair,
        Read *mate,
        AlignmentResult mateResult,
        unsigned mateLocation,
        bool mateIsRC)
{
    const unsigned maxLineLength = 25000;
    char outputBuffer[maxLineLength];
    size_t outputBufferUsed;

    if (!generateSAMText(read,result,genomeLocation,isRC,hasMate,firstInPair,mate,mateResult,
            mateLocation,mateIsRC, genome, &lv, outputBuffer, maxLineLength,&outputBufferUsed)) {
        fprintf(stderr,"SimpleSAMWriter: tried to generate too long of a SAM line (> %d)\n",maxLineLength);
        exit(1);
    }
    
    if (1 != fwrite(outputBuffer,outputBufferUsed,1,file)) {
        fprintf(stderr,"Unable to write to SAM file.\n");
        exit(1);
    }
}


bool SimpleSAMWriter::close()
{
    if (file == NULL) {
        return false;
    }
    fclose(file);
    BigDealloc(buffer);
    file = NULL;
    buffer = NULL;
    return true;
}

    ParallelSAMWriter *
ParallelSAMWriter::create(const char *fileName, const Genome *genome, unsigned nThreads)
{
#ifdef  _MSC_VER
//    return SimpleParallelSAMWriter::create(fileName, genome, nThreads);
    return WindowsParallelSAMWriter::create(fileName, genome, nThreads);
#else   // _MSC_VER
    return SimpleParallelSAMWriter::create(fileName, genome, nThreads);
#endif  // _MSC_VER
}

SimpleParallelSAMWriter::~SimpleParallelSAMWriter()
{
    if (NULL != writer) {
        for (unsigned i = 0; i < nThreads; i++) {
            if (NULL != writer[i]) {
                delete writer[i];
            }
        }
        delete [] writer;
    }
}

    SimpleParallelSAMWriter *
SimpleParallelSAMWriter::create(const char *fileName, const Genome *genome, unsigned nThreads)
{
    SimpleParallelSAMWriter *parallelWriter = new SimpleParallelSAMWriter();

    parallelWriter->nThreads = nThreads;
    parallelWriter->writer = new SimpleSAMWriter *[nThreads];
    
    size_t filenameBufferSize = strlen(fileName) + 5;
    char *filenameBuffer = new char[filenameBufferSize];

    for (unsigned i = 0; i < nThreads; i++) {
        if (nThreads > 1) {
            const char *fnTemplate = fileName;
            size_t len = strlen(fnTemplate);
            // Find the last '.' in the template and add our thread number before it
            int dotPos = (int)len - 1;
            while (dotPos >= 0 && fnTemplate[dotPos] != '.') {
                dotPos--;
            }
            if (dotPos != -1) {
                sprintf(filenameBuffer, "%.*s_%02d%.*s", dotPos, fnTemplate, i,
                                              (int) (len - dotPos), fnTemplate + dotPos);
            } else {
                // The user's filename template had no extension; just append "_<threadNum>" to it
                sprintf(filenameBuffer, "%s_%02d", fnTemplate, i);
            }
        } else {
            strcpy(filenameBuffer,fileName);
        }

        parallelWriter->writer[i] = new SimpleSAMWriter;
        if (!parallelWriter->writer[i]->open(filenameBuffer,genome)) {
            fprintf(stderr,"SAM writer for file '%s' failed to open.\n",filenameBuffer);
            delete [] filenameBuffer;
            delete parallelWriter;

            return NULL;
        }
    }

    delete [] filenameBuffer;
    return parallelWriter;
}

    bool
SimpleParallelSAMWriter::close()
{
    bool worked = true;
    for (unsigned i = 0; i < nThreads; i++) {
        worked &= writer[i]->close();
    }

    return worked;
}

#ifdef  _MSC_VER
    WindowsParallelSAMWriter *
WindowsParallelSAMWriter::create(const char *fileName, const Genome *genome, unsigned nThreads)
{
    WindowsParallelSAMWriter *parallelWriter = new WindowsParallelSAMWriter();

    parallelWriter->hFile = CreateFile(fileName,GENERIC_READ | GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_FLAG_OVERLAPPED,NULL);
    if (INVALID_HANDLE_VALUE == parallelWriter->hFile) {
        fprintf(stderr,"Unable to create SAM file '%s', %d\n",fileName,GetLastError());
        delete parallelWriter;
        return NULL;
    }

    const size_t headerBufferSize = 20000;
    char headerBuffer[headerBufferSize];
    size_t headerActualSize;

    if (!SAMWriter::generateHeader(genome,headerBuffer,headerBufferSize,&headerActualSize)) {
        fprintf(stderr,"WindowsParallelSAMWriter: unable to generate SAM header.\n");
        delete parallelWriter;
        return NULL;
    }

    OVERLAPPED lap;
    lap.Offset = 0;
    lap.OffsetHigh = 0;
    lap.hEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
    if (NULL == lap.hEvent) {
        fprintf(stderr,"WindowsParallelSAMWriter: unable to allocate event, %d\n",GetLastError());
        delete parallelWriter;
        return NULL;
    }

    DWORD bytesWritten;
    if (!WriteFile(parallelWriter->hFile,headerBuffer,(DWORD)headerActualSize,&bytesWritten,&lap)) {
        if (ERROR_IO_PENDING != GetLastError()) {
            fprintf(stderr,"WindowsParallelSAMWriter: unable to write header to file, %d\n",GetLastError());
            delete parallelWriter;
            return NULL;
        }
    }

    if (!GetOverlappedResult(parallelWriter->hFile,&lap,&bytesWritten,TRUE)) {
            fprintf(stderr,"WindowsParallelSAMWriter: unable to write header to file; GetOverlappedResult failed %d\n",GetLastError());
            delete parallelWriter;
            return NULL;
    }

    if (bytesWritten != headerActualSize) {
        fprintf(stderr,"WindowsParallelSAMWriter: header didn't write completely.  %d != %lld\n",bytesWritten,headerActualSize);
            delete parallelWriter;
            return NULL;
    }

    CloseHandle(lap.hEvent);

    parallelWriter->nextWriteOffset = bytesWritten;

    parallelWriter->nThreads = nThreads;
    parallelWriter->writer = new WindowsSAMWriter *[nThreads];
    bool worked = true;

    for (unsigned i = 0; i < nThreads; i++) {
        parallelWriter->writer[i] = new WindowsSAMWriter();
        worked &= parallelWriter->writer[i]->initialize(parallelWriter->hFile,genome,&parallelWriter->nextWriteOffset);
    }

    if (!worked) {
        fprintf(stderr,"Unable to create SAM writer.\n");
        delete parallelWriter;
        return NULL;
    }


    return parallelWriter;
}

WindowsParallelSAMWriter::~WindowsParallelSAMWriter()
{
    if (NULL != writer) {
        for (unsigned i = 0; i < nThreads; i++) {
            if (NULL != writer[i]) {
                delete writer[i];
            }
        }
        delete [] writer;
    }
    CloseHandle(hFile);
}

    bool
WindowsParallelSAMWriter::close()
{
    bool worked = true;
    for (unsigned i = 0; i < nThreads; i++) {
        worked &= writer[i]->close();
    }
    return worked;
}

WindowsSAMWriter::WindowsSAMWriter() : hFile(INVALID_HANDLE_VALUE), nextWriteOffset(NULL), remainingBufferSpace(BufferSize), bufferBeingCreated(0),
        writeOutstanding(0)
{
    buffer[0] = NULL;
    buffer[1] = NULL;
    lap[0].hEvent = NULL;
    lap[1].hEvent = NULL;
}

    bool
WindowsSAMWriter::initialize(HANDLE i_hFile, const Genome *i_genome, volatile _int64 *i_nextWriteOffset)
{
    hFile = i_hFile;
    genome = i_genome;
    nextWriteOffset = i_nextWriteOffset;
    buffer[0] = (char *)BigAlloc(BufferSize);
    buffer[1] = (char *)BigAlloc(BufferSize);
    lap[0].hEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
    lap[1].hEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
    
    if (NULL == buffer[0] || NULL == buffer[1] || NULL == lap[0].hEvent || NULL == lap[1].hEvent) {
        fprintf(stderr,"WindowsSAMWriter: failed to initialize\n");
        return false;
    }

    return true;
}

WindowsSAMWriter::~WindowsSAMWriter()
{
    CloseHandle(lap[0].hEvent);
    CloseHandle(lap[1].hEvent);
    BigDealloc(buffer[0]);
    BigDealloc(buffer[1]);
}

    bool
WindowsSAMWriter::close()
{
    if (remainingBufferSpace != BufferSize) {
        if (!startIo()) {
            fprintf(stderr,"WindowsSAMWriter::close(): startIo failed\n");
            return false;
        }

        if (!waitForIoCompletion()) {
            fprintf(stderr,"WindowsSAMWriter::close(): waitForIoCompletion failed\n");
            return false;
        }
    }

    return true;
}

    bool
WindowsSAMWriter::write(Read *read, AlignmentResult result, unsigned genomeLocation, bool isRC)
{
    size_t sizeUsed;
    if (!generateSAMText(read,result,genomeLocation,isRC,false,true,NULL,UnknownAlignment,0,false,genome,&lv,
            buffer[bufferBeingCreated] + BufferSize - remainingBufferSpace,remainingBufferSpace, &sizeUsed)) {

        if (!startIo()) {
            return false;
        }

        if (!generateSAMText(read,result,genomeLocation,isRC,false,true,NULL,UnknownAlignment,0,false,genome,&lv,
                buffer[bufferBeingCreated] + BufferSize - remainingBufferSpace,remainingBufferSpace, &sizeUsed)) {

            fprintf(stderr,"WindowsSAMWriter: create SAM string into fresh buffer failed\n");
            return false;
        }
    }

    remainingBufferSpace -= sizeUsed;
    return true;
}

    bool
WindowsSAMWriter::writePair(Read *read0, Read *read1, PairedAlignmentResult *result)
{
    //
    // We need to write both halves of the pair into the same buffer, so that a write from
    // some other thread doesn't separate them.  So, try the writes and if either doesn't
    // work start IO and try again.
    //
    size_t sizeUsed[2];
    bool writesFit = generateSAMText(read0,result->status[0],result->location[0],result->isRC[0],true,true,
                                     read1,result->status[1],result->location[1],result->isRC[1],
                        genome, &lv, buffer[bufferBeingCreated] + BufferSize - remainingBufferSpace,remainingBufferSpace,&sizeUsed[0]);
    if (writesFit) {
        writesFit = generateSAMText(read1,result->status[1],result->location[1],result->isRC[1],true,false,
                                    read0,result->status[0],result->location[0],result->isRC[0],
                        genome, &lv, buffer[bufferBeingCreated] + BufferSize - remainingBufferSpace + sizeUsed[0],remainingBufferSpace-sizeUsed[0],&sizeUsed[1]);
    }

    if (!writesFit) {
        if (!startIo()) {
            return false;
        }

    if (!generateSAMText(read0,result->status[0],result->location[0],result->isRC[0],true,true,
                         read1,result->status[1],result->location[1],result->isRC[1],
            genome, &lv, buffer[bufferBeingCreated] + BufferSize - remainingBufferSpace,remainingBufferSpace,&sizeUsed[0]) ||
        !generateSAMText(read1,result->status[1],result->location[1],result->isRC[1],true,false,
                         read0,result->status[0],result->location[0],result->isRC[0],
            genome, &lv, buffer[bufferBeingCreated] + BufferSize - remainingBufferSpace + sizeUsed[0],remainingBufferSpace-sizeUsed[0],&sizeUsed[1])) {

            fprintf(stderr,"WindowsSAMWriter: create SAM string into fresh buffer failed\n");
            return false;
        }
    }

    remainingBufferSpace -= (sizeUsed[0] + sizeUsed[1]);
    return true;
}

    bool
WindowsSAMWriter::startIo()
{
    //
    // It didn't fit in the buffer.  Start writing it.
    //
    LARGE_INTEGER writeOffset;
    writeOffset.QuadPart = InterlockedAdd64AndReturnNewValue(nextWriteOffset,BufferSize - remainingBufferSpace) - 
                                (BufferSize - remainingBufferSpace);
    lap[bufferBeingCreated].OffsetHigh = writeOffset.HighPart;
    lap[bufferBeingCreated].Offset = writeOffset.LowPart;
    DWORD bytesWritten;
    if (!WriteFile(hFile,buffer[bufferBeingCreated],(DWORD)(BufferSize - remainingBufferSpace),&bytesWritten,&lap[bufferBeingCreated])) {
        if (ERROR_IO_PENDING != GetLastError()) {
            fprintf(stderr,"WindowsSAMWriter: WriteFile failed, %d\n",GetLastError());
            return false;
        }
    }

    //
    // If necessary, wait for the other buffer to finish writing.
    //
    if (writeOutstanding) {
        if (!waitForIoCompletion()) {
            fprintf(stderr,"WindowsSAMWriter: GetOverlappedResult failed, %d\n",GetLastError());
            return false;
        }
    }
    writeOutstanding = true;
    bufferBeingCreated = 1 - bufferBeingCreated;
    remainingBufferSpace = BufferSize;

    return true;
}

    bool
WindowsSAMWriter::waitForIoCompletion()
{
    _ASSERT(writeOutstanding);
    
    DWORD nBytesTransferred;
    if (!GetOverlappedResult(hFile,&lap[1-bufferBeingCreated],&nBytesTransferred,TRUE)) {
        return false;
    }
    writeOutstanding = false;
    return true;
}

#endif  // _MSC_VER

    char *
strnchrs(char *str, char charToFind, char charToFind2, size_t maxLen) // Hokey version that looks for either of two chars
{
    for (size_t i = 0; i < maxLen; i++) {
        if (str[i] == charToFind || str[i] == charToFind2) {
            return str + i;
        }
        if (str[i] == 0) {
            return NULL;
        }
    }
    return NULL;
}

    char *
skipToBeyondNextRunOfSpacesAndTabs(char *str, const char *endOfBuffer, size_t *charsUntilFirstSpaceOrTab = NULL)
{
    if (NULL == str) return NULL;

    char *nextChar = str;
    while (nextChar < endOfBuffer && *nextChar != ' ' && *nextChar != '\n' && *nextChar != '\t' && *nextChar != '\r' /* for Windows CRLF text */) {
        nextChar++;
    }

    if (NULL != charsUntilFirstSpaceOrTab) {
        *charsUntilFirstSpaceOrTab = nextChar - str;
    }

    if (nextChar >= endOfBuffer || *nextChar == '\n') {
        return NULL;
    }

    while (nextChar < endOfBuffer && (' ' == *nextChar || '\t' == *nextChar || '\r' == *nextChar)) {
        nextChar++;
    }

    if (nextChar >= endOfBuffer) {
        return NULL;
    }

    return nextChar;
}


SAMReader::~SAMReader()
{
}

    SAMReader *
SAMReader::create(const char *fileName, const Genome *genome, _int64 startingOffset, 
                    _int64 amountOfFileToProcess, ReadClippingType clipping)
{
#ifdef  _MSC_VER
    WindowsOverlappedSAMReader *reader = new WindowsOverlappedSAMReader(clipping);
#else
    MemMapSAMReader *reader = new MemMapSAMReader(clipping);
#endif
    if (!reader->init(fileName, genome, startingOffset, amountOfFileToProcess)) {
        //
        // Probably couldn't open the file.
        //
        delete reader;
        return NULL;
    }
    return reader;
}

//
// Implement the ReadReader form of getNextRead, which doesn't include the
// alignment results by simply throwing them away.
//
    bool
SAMReader::getNextRead(Read *readToUpdate)
{
    return getNextRead(readToUpdate, NULL, NULL, NULL, NULL, NULL, NULL);
}


    bool
SAMReader::getNextReadPair(Read *read1, Read *read2, PairedAlignmentResult *alignmentResult, 
            unsigned *mapQ, const char **cigar)
{
    unsigned flag[2];
    if (!getNextRead(read1, &alignmentResult->status[0],&alignmentResult->location[0],
            &alignmentResult->isRC[0],mapQ ? &mapQ[0] : NULL,&flag[0],false,cigar ? &cigar[0] : NULL)) {
        return false;
    }

    if (!getNextRead(read2, &alignmentResult->status[1],&alignmentResult->location[1],
            &alignmentResult->isRC[1],mapQ ? &mapQ[1] : NULL,&flag[1],true, cigar? &cigar[1] : NULL)) {
        return false;
    }

    if (!(flag[0] & SAM_MULTI_SEGMENT) || !(flag[1] & SAM_MULTI_SEGMENT) || !(flag[0] & SAM_FIRST_SEGMENT) || !(flag[1] & SAM_LAST_SEGMENT)) {
        return false;
    }

    return true;
}

    bool
SAMReader::parseHeader(const char *fileName, char *firstLine, char *endOfBuffer, const Genome *genome, size_t *headerSize)
{
    char *nextLineToProcess = firstLine;

    while (NULL != nextLineToProcess && nextLineToProcess < endOfBuffer && '@' == *nextLineToProcess) {
        if (!strncmp("@SQ",nextLineToProcess,3)) {
            //
            // These lines represent sequences in the reference genome, what are
            // called "pieces" in the Genome class.  (Roughly, chromosomes or major
            // variants like some versions of the MHC genes on chr6; or more
            // particularly the things that come in different FASTA files from the
            // reference assembly).
            //
            // Verify that they actually match what's in our reference genome.
            //

            if (nextLineToProcess + 3 >= endOfBuffer || ' ' != nextLineToProcess[3] && '\t' != nextLineToProcess[3]) {
                fprintf(stderr,"Malformed SAM file '%s' has @SQ without a following space or tab.\n",fileName);
                return false;
            }

            char *snStart = nextLineToProcess + 4;
            while (snStart < endOfBuffer && strncmp(snStart,"SN:",__min(3,endOfBuffer-snStart)) && *snStart != '\n') {
                snStart++;
            }

            if (snStart >= endOfBuffer || *snStart == '\n') {
                fprintf(stderr,"Malformed @SQ line doesn't have 'SN:' in file '%s'\n",fileName);
                return false;
            }

            const size_t pieceNameBufferSize = 512;
            char pieceName[pieceNameBufferSize];
            for (unsigned i = 0; i < pieceNameBufferSize && snStart+3+i < endOfBuffer; i++) {
                if (snStart[3+i] == ' ' || snStart[3+i] == '\t' || snStart[3+i] == '\n') {
                    pieceName[i] = '\0';
                } else {
                    pieceName[i] = snStart[3+i];
                }
            }
            pieceName[pieceNameBufferSize - 1] = '\0';

            if (!genome->getOffsetOfPiece(pieceName,NULL)) {
                fprintf(stderr,"SAM file '%s' contains sequence name '%s' that isn't in the reference genome.\n",
                            fileName,pieceName);
                return false;
            }
        } else if (!strncmp("@HD",nextLineToProcess,3) || !strncmp("@RG",nextLineToProcess,3) || !strncmp("@PG",nextLineToProcess,3) ||
            !strncmp("@CO",nextLineToProcess,3)) {
            //
            // Ignore these lines.
            //
        } else {
            fprintf(stderr,"Unrecognized header line in SAM file.\n");
            return false;
        }
        nextLineToProcess = strnchr(nextLineToProcess,'\n',endOfBuffer-nextLineToProcess) + 1;
    }

    *headerSize = nextLineToProcess - firstLine;
    return true;
}

    bool
SAMReader::parseLine(char *line, char *endOfBuffer, char *result[], size_t *linelength, size_t fieldLengths[])
{
    *linelength = 0;

    char *next = line;
    char *endOfLine = strnchr(line,'\n',endOfBuffer-line);
    if (NULL == endOfLine) {
        return false;
    }

    //
    // Skip over any leading spaces and tabs
    //
    while (next < endOfLine && (*next == ' ' || *next == '\t')) {
        next++;
    }

    for (unsigned i = 0; i < nSAMFields; i++) {
        if (NULL == next || next >= endOfLine) {
            //
            // Too few fields.
            //
            return false;
        }

        result[i] = next;

        next = skipToBeyondNextRunOfSpacesAndTabs(next,endOfLine,&fieldLengths[i]);
    }

    *linelength =  endOfLine - line + 1;    // +1 skips over the \n
    return true;
}

    void
SAMReader::getReadFromLine(
    const Genome        *genome,
    char                *line, 
    char                *endOfBuffer, 
    Read                *read, 
    AlignmentResult     *alignmentResult,
    unsigned            *genomeLocation, 
    bool                *isRC,
    unsigned            *mapQ,
    size_t              *lineLength,
    unsigned *           flag,
    unsigned **          newReferenceCounts,
    const char **        cigar,
    ReadClippingType     clipping
    )
{
    char *field[nSAMFields];
    size_t fieldLength[nSAMFields];

    if (!parseLine(line,endOfBuffer,field,lineLength,fieldLength)) {
        fprintf(stderr,"Failed to parse SAM line.\n");
        exit(1);
    }

    //
    // We have to copy the piece name (RNAME) into its own buffer because the code in Genome expects
    // it to be a null-terminated string, while all we've got is one that's space delimited.
    //
    const size_t pieceNameBufferSize = 512;
    char pieceName[pieceNameBufferSize];

    if (fieldLength[RNAME] >= pieceNameBufferSize) {  // >= because we need a byte for the \0
        fprintf(stderr,"SAMReader: too long an RNAME.  Can't parse.\n");
        exit(1);
    }
    
    memcpy(pieceName,field[RNAME],fieldLength[RNAME]);
    pieceName[fieldLength[RNAME]] = '\0';

    unsigned offsetOfPiece;
    if ('*' != pieceName[0] && !genome->getOffsetOfPiece(pieceName,&offsetOfPiece)) {
        fprintf(stderr,"Unable to find piece '%s' in genome.  SAM file malformed.\n",pieceName);
        exit(1);
    }

    if (NULL != genomeLocation) {
        unsigned oneBasedOffsetWithinPiece = 0;
        if ('*' != pieceName[0]) {
            //
            // We can't call sscanf directly into the mapped file, becuase it reads to the end of the
            // string even when it's satisfied all of its fields.  Since this can be gigabytes, it's not
            // really good for perf.  Instead, copy the POS field into a local buffer and null terminate it.
            //

            const unsigned posBufferSize = 20;
            char posBuffer[posBufferSize];
            if (fieldLength[POS] >= posBufferSize) {
                fprintf(stderr,"SAMReader: POS field too long.\n");
                exit(1);
            }
            memcpy(posBuffer,field[POS],fieldLength[POS]);
            posBuffer[fieldLength[POS]] = '\0';
            if (0 == sscanf(posBuffer,"%d",&oneBasedOffsetWithinPiece)) {
                fprintf(stderr,"SAMReader: Unable to parse position when it was expected.\n");
                exit(1);
            }
            if (0 == oneBasedOffsetWithinPiece) {
                fprintf(stderr,"SAMReader: Position parsed as 0 when it was expected.\n");
                exit(1);
            }
            *genomeLocation = offsetOfPiece + oneBasedOffsetWithinPiece - 1; // -1 is because our offset is 0 based, while SAM is 1 based.
        } else {
            *genomeLocation = 0xffffffff;
        }
    }

    if (fieldLength[SEQ] != fieldLength[QUAL]) {
        fprintf(stderr,"SAMReader: QUAL string unequal in length to SEQ string.\n");
        exit(1);
    }

    unsigned _flag;
    const size_t flagBufferSize = 20;   // More than enough
    char flagBuffer[flagBufferSize];
    if (fieldLength[FLAG] >= flagBufferSize) {
        fprintf(stderr,"SAMReader: flag field is too long.\n");
        exit(1);
    }
    memcpy(flagBuffer,field[FLAG],fieldLength[FLAG]);
    flagBuffer[fieldLength[FLAG]] = '\0';
    if (1 != sscanf(flagBuffer,"%d",&_flag)) {
        fprintf(stderr,"SAMReader: couldn't parse FLAG field.\n");
        exit(1);
    }

    if (NULL != read) {
        //
        // Clip reads where the quality strings end in '#'
        //
        read->init(field[QNAME],(unsigned)fieldLength[QNAME],field[SEQ],field[QUAL],(unsigned)fieldLength[SEQ],newReferenceCounts);
        //
        // If this read is RC in the SAM file, we need to reverse it here, since Reads are always the sense that they were as they came
        // out of the base caller.
        //

        if (_flag & SAM_REVERSE_COMPLEMENT) {
            read->becomeRC();
        }
        read->clip(clipping);
    }

    if (NULL != alignmentResult) {
        if (_flag & SAM_UNMAPPED) {
            *alignmentResult = NotFound;
        } else {
            if ('*' == pieceName[0]) {
                fprintf(stderr,"SAMReader: mapped read didn't have RNAME filled in.\n");
                exit(1);
            }
            *alignmentResult = SingleHit;   // NB: This isn't quite right, we should look at MAPQ.
        }
    }

    if (NULL != isRC) {
        *isRC = (_flag & SAM_REVERSE_COMPLEMENT) ? true : false;
    }

    if (NULL != mapQ) {
        *mapQ = atoi(field[MAPQ]);
        if (*mapQ > 255) {
            fprintf(stderr,"SAMReader: MAPQ field has bogus value\n");
            exit(1);
        }
    }

    if (NULL != flag) {
        *flag = _flag;
    }

    if (NULL != cigar) {
        *cigar = field[CIGAR];
    }
}


#ifdef  _MSC_VER

WindowsOverlappedSAMReader::WindowsOverlappedSAMReader(ReadClippingType i_clipping)
{
    clipping = i_clipping;

    //
    // Initilize the buffer info struct.
    //
    for (unsigned i = 0 ; i < nBuffers; i++) {
        bufferInfo[i].buffer = (char *)BigAlloc(bufferSize + 1);    // +1 gives us a place to put a terminating null
        if (NULL == bufferInfo[i].buffer) {
            fprintf(stderr,"FASTQ Reader: unable to allocate IO buffer\n");
            exit(1);
        }

        bufferInfo[i].buffer[bufferSize] = 0;       // The terminating null.
        
        bufferInfo[i].lap.hEvent = CreateEvent(NULL,TRUE,FALSE,NULL);
        if (NULL == bufferInfo[i].lap.hEvent) {
            fprintf(stderr,"Unable to create event for FASTQ reader\n");
            exit(1);
        }

        bufferInfo[i].state = Empty;
        bufferInfo[i].isEOF = false;
        bufferInfo[i].offset = 0;
        bufferInfo[i].referenceCount = 0;
    }

    hFile = INVALID_HANDLE_VALUE;
    genome = NULL;
}

    bool
WindowsOverlappedSAMReader::init(const char *fileName, const Genome *i_genome, _int64 startingOffset, _int64 amountOfFileToProcess)
{
    genome = i_genome;
    
    hFile = CreateFile(fileName,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
    if (INVALID_HANDLE_VALUE == hFile) {
        return false;
    }

    if (!GetFileSizeEx(hFile,&fileSize)) {
        fprintf(stderr,"WindowsSAM reader: unable to get file size of '%s', %d\n",fileName,GetLastError());
        return false;
    }

    //
    // Read and parse the header specially.
    //
    BufferInfo *info = &bufferInfo[0];
    info->lap.Offset = 0;
    info->lap.OffsetHigh = 0;

    if (!ReadFile(hFile,info->buffer,1024 * 1024,&info->validBytes,&info->lap)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            fprintf(stderr,"WindowsOverlappedSAMReader::init: unable to read header of '%s', %d\n",fileName,GetLastError());
            return false;
        }
    }

    if (!GetOverlappedResult(hFile,&info->lap,&info->validBytes,TRUE)) {
        fprintf(stderr,"WindowsOverlappedSAMReader::init: error reading header of '%s', %d\n",fileName,GetLastError());
        return false;
    }

    if (!parseHeader(fileName,info->buffer,info->buffer + info->validBytes,genome,&headerSize)) {
        fprintf(stderr,"SAMReader: failed to parse header on '%s'\n",fileName);
        return false;
    }

    reinit(startingOffset,amountOfFileToProcess);

    return true;
}

    void
WindowsOverlappedSAMReader::reinit(_int64 startingOffset, _int64 amountOfFileToProcess)
{
    _ASSERT(INVALID_HANDLE_VALUE != hFile && 0 != headerSize);  // Must call init() before reinit()

    //
    // First let any pending IO complete.
    //
    for (unsigned i = 0; i < nBuffers; i++) {
        if (bufferInfo[i].state == Reading) {
            waitForBuffer(i);
        }
        bufferInfo[i].state = Empty;
        bufferInfo[i].isEOF= false;
        bufferInfo[i].offset = 0;
        bufferInfo[i].referenceCount = 0;
    }

    nextBufferForReader = 0;
    nextBufferForConsumer = 0;    

    readOffset.QuadPart = max(headerSize,(size_t)startingOffset) - 1; // -1 is to point at the previous newline so we don't skip the first line.
    if (0 == amountOfFileToProcess) {
        //
        // This means just read the whole file.
        //
        endingOffset = fileSize.QuadPart;
    } else {
        endingOffset = min(fileSize.QuadPart,startingOffset + amountOfFileToProcess);
    }

    //
    // Kick off IO, wait for the first buffer to be read and then skip until hitting the first newline.
    //
    startIo();
    waitForBuffer(nextBufferForConsumer);

    BufferInfo *info = &bufferInfo[nextBufferForConsumer];
    char *firstNewline = strnchr(info->buffer,'\n',info->validBytes);
    if (NULL == firstNewline) {
        return;
    }

    //
    // Parse the new first line.  If it's got SAM_MULTI_SEGMENT set and not SAM_FIRST_SEGMENT, then skip it and go with the next one.
    //
    Read read(this);
    AlignmentResult alignmentResult;
    unsigned genomeLocation;
    bool isRC;
    unsigned mapQ;
    size_t lineLength;
    unsigned flag;
    unsigned *referenceCounts[2];

    referenceCounts[0] = &info->referenceCount;
    referenceCounts[1] = NULL;

    getReadFromLine(genome,firstNewline+1,info->buffer + info->validBytes,
                    &read,&alignmentResult,&genomeLocation,&isRC,&mapQ,&lineLength,
                    &flag,referenceCounts,NULL,clipping);

    if ((flag & SAM_MULTI_SEGMENT) && !(flag & SAM_FIRST_SEGMENT)) {
        //
        // Skip this line.
        //
        info->offset = (unsigned)(firstNewline + lineLength - info->buffer + 1); // +1 skips over the newline.
    } else {
        info->offset = (unsigned)(firstNewline - info->buffer + 1); // +1 skips over the newline.
    }
}

WindowsOverlappedSAMReader::~WindowsOverlappedSAMReader()
{
    for (unsigned i = 0; i < nBuffers; i++) {
        BigDealloc(bufferInfo[i].buffer);
        bufferInfo[i].buffer = NULL;
        CloseHandle(bufferInfo[i].lap.hEvent);
    }
    CloseHandle(hFile);
}


    bool
WindowsOverlappedSAMReader::getNextRead(
            Read *read, AlignmentResult *alignmentResult, unsigned *genomeLocation, bool *isRC, unsigned *mapQ, 
            unsigned *flag, bool ignoreEndOfRange, const char **cigar)
{
    BufferInfo *info = &bufferInfo[nextBufferForConsumer];
    if (info->isEOF && info->offset >= info->validBytes) {
        //
        // EOF.
        //
        return false;
    }

    if (info->offset > info->nBytesThatMayBeginARead && !ignoreEndOfRange) {
        //
        // Past the end of our section.
        //
        return false;
    }

    if (info->state != Full) {
        waitForBuffer(nextBufferForConsumer);
    }

    unsigned *referenceCounts[2];
    referenceCounts[0] = &info->referenceCount;
    referenceCounts[1] = NULL;

    char *nextLine;
    char *endOfBuffer;

    char *newLine = strchr(info->buffer + info->offset, '\n'); // The buffer is null terminated
    if (NULL == newLine) {
        //
        // There is no newline, so the line crosses the end of the buffer.  Use the overflow buffer
        //
        if (info->isEOF) {
            fprintf(stderr,"SAM file doesn't end with a newline!  Failing.  fileOffset = %lld, offset = %d, validBytes = %d, nBytesThatMayBeginARead %d\n",
                info->fileOffset,info->offset,info->validBytes,info->nBytesThatMayBeginARead);
            exit(1);
        }

        if (bufferInfo[(nextBufferForConsumer + 1) % nBuffers].state != Full) {
            waitForBuffer((nextBufferForConsumer + 1) % nBuffers);
        }

        _ASSERT(bufferInfo[nextBufferForConsumer].fileOffset + bufferInfo[nextBufferForConsumer].validBytes == 
                    bufferInfo[(nextBufferForConsumer + 1) % nBuffers].fileOffset);
        
        unsigned amountFromOldBuffer = info->validBytes - info->offset;

        nextLine = info->overflowBuffer;
        memcpy(nextLine,info->buffer + info->offset, info->validBytes - info->offset);
        info->state = UsedButReferenced;        // The consumer is no longer using this buffer, but it's still referecned by Read(s)
        info->referenceCount++;
        info->offset = info->validBytes;

        nextBufferForConsumer = (nextBufferForConsumer + 1) % nBuffers;
        info = &bufferInfo[nextBufferForConsumer];
        referenceCounts[1] = &info->referenceCount;

        newLine = strchr(info->buffer,'\n');
        _ASSERT(NULL != newLine);
        memcpy(nextLine + amountFromOldBuffer, info->buffer, newLine - info->buffer + 1);
        endOfBuffer = nextLine + maxLineLen + 1;

        info->offset = (unsigned)(newLine - info->buffer + 1);
    } else {
        nextLine = info->buffer + info->offset;
        info->offset = (unsigned)((newLine + 1) - info->buffer);
        endOfBuffer = info->buffer + info->validBytes;
    }

    info->referenceCount++;

    size_t lineLength;
    getReadFromLine(genome,nextLine,endOfBuffer,read,alignmentResult,genomeLocation,isRC,mapQ,&lineLength,flag,referenceCounts,cigar,clipping);

    return true;
}

    void
WindowsOverlappedSAMReader::startIo()
{
    //
    // Launch reads on whatever buffers are ready.
    //
    while (bufferInfo[nextBufferForReader].state == Empty) {
        BufferInfo *info = &bufferInfo[nextBufferForReader];

        if (readOffset.QuadPart >= fileSize.QuadPart || readOffset.QuadPart >= endingOffset + maxReadSizeInBytes) {
            info->validBytes = 0;
            info->nBytesThatMayBeginARead = 0;
            info->isEOF = readOffset.QuadPart >= fileSize.QuadPart;
            info->state = Full;
            SetEvent(info->lap.hEvent);
            return;
        }

        unsigned amountToRead;
        if (fileSize.QuadPart - readOffset.QuadPart > bufferSize && endingOffset + maxReadSizeInBytes - readOffset.QuadPart > bufferSize) {
            amountToRead = bufferSize;

            if (readOffset.QuadPart + amountToRead > endingOffset) {
                info->nBytesThatMayBeginARead = (unsigned)(endingOffset - readOffset.QuadPart);
            } else {
                info->nBytesThatMayBeginARead = amountToRead;
            }
            info->isEOF = false;
        } else {
            amountToRead = (unsigned)__min(fileSize.QuadPart - readOffset.QuadPart,endingOffset+maxReadSizeInBytes - readOffset.QuadPart);
            if (endingOffset <= readOffset.QuadPart) {
                //
                // We're only reading this for overflow buffer.
                //
                info->nBytesThatMayBeginARead = 0;
            } else {
                info->nBytesThatMayBeginARead = __min(amountToRead,(unsigned)(endingOffset - readOffset.QuadPart));    // Don't begin a read past endingOffset
            }
            info->isEOF = readOffset.QuadPart + amountToRead >= fileSize.QuadPart;
        }

        _ASSERT(amountToRead >= info->nBytesThatMayBeginARead || !info->isEOF || fileSize.QuadPart == readOffset.QuadPart + amountToRead);
        ResetEvent(info->lap.hEvent);
        info->lap.Offset = readOffset.LowPart;
        info->lap.OffsetHigh = readOffset.HighPart;
        info->fileOffset = readOffset.QuadPart;
         
        if (!ReadFile(
                hFile,
                info->buffer,
                amountToRead,
                &info->validBytes,
                &info->lap)) {

            if (GetLastError() != ERROR_IO_PENDING) {
                fprintf(stderr,"FASTQReader::startIo(): readFile failed, %d\n",GetLastError());
                exit(1);
            }
        }

        readOffset.QuadPart += amountToRead;
        info->state = Reading;
        info->offset = 0;

        nextBufferForReader = (nextBufferForReader + 1) % nBuffers;
    }
}

    void
WindowsOverlappedSAMReader::waitForBuffer(unsigned bufferNumber)
{
    BufferInfo *info = &bufferInfo[bufferNumber];

    if (info->state == Full) {
        return;
    }

    if (info->state == UsedButReferenced) {
        fprintf(stderr,"Overlapped buffer manager: waiting for buffer that's in UsedButReferenced.  Almost certainly a bug.\n");
        exit(1);
    }

    if (info->state != Reading) {
        startIo();
    }

    if (!GetOverlappedResult(hFile,&info->lap,&info->validBytes,TRUE)) {
        fprintf(stderr,"Error reading FASTQ file, %d\n",GetLastError());
        exit(1);
    }

    info->state = Full;
    info->buffer[info->validBytes] = 0;
    ResetEvent(info->lap.hEvent);
}


    void
WindowsOverlappedSAMReader::readDoneWithBuffer(unsigned *referenceCount)
{
    if (0 != *referenceCount) {
        return;
    }

    BufferInfo *info = NULL;
    for (unsigned i = 0; i < nBuffers; i++) {
        if (&bufferInfo[i].referenceCount == referenceCount) {
            info = &bufferInfo[i];
            break;
        }
    }

    _ASSERT(NULL != info);

    if (info->state == UsedButReferenced) {
        info->state = Empty;

        startIo();
    }
}


#else   // _MSC_VER


MemMapSAMReader::MemMapSAMReader(ReadClippingType i_clipping)
    : clipping(i_clipping), fileData(NULL), fd(-1)
{
}


MemMapSAMReader::~MemMapSAMReader()
{
    unmapCurrentRange();
    if (fd != -1) {
        close(fd);
    }
}


void MemMapSAMReader::unmapCurrentRange()
{
    if (fileData != NULL) {
        munmap(fileData, amountMapped);
        fileData = NULL;
    }
}


bool MemMapSAMReader::init(
        const char *fileName,
        const Genome *i_genome,
        _int64 startingOffset,
        _int64 amountOfFileToProcess)
{
    genome = i_genome;

    fd = open(fileName, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Failed to open %s\n", fileName);
        return false;
    }
    
    struct stat sb;
    int r = fstat(fd, &sb);
    if (r == -1) {
        fprintf(stderr, "Failed to stat %s\n", fileName);
        return false;
    }
    fileSize = sb.st_size;

    // Let's first mmap() the whole file to figure out where the header ends.
    char *allData = (char *) mmap(NULL, fileSize, PROT_READ, MAP_SHARED, fd, 0);
    if (allData == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap SAM file\n");
        return false;
    }

    if (!parseHeader(fileName, allData, allData + fileSize, genome, &headerSize)) {
        fprintf(stderr, "Failed to parse SAM header from %s\n", fileName);
        munmap(allData, fileSize);
        return false;
    }
    //printf("headerSize: %lu\n", headerSize);

    munmap(allData, fileSize);

    reinit(startingOffset, amountOfFileToProcess);
    return true;
}


void MemMapSAMReader::reinit(_int64 startingOffset, _int64 amountToProcess)
{
    unmapCurrentRange();

    if (amountToProcess == 0) {
        // This means to process the whole file.
        amountToProcess = fileSize - startingOffset;
    }

    _int64 misalignment = (startingOffset % getpagesize());
    _int64 alignedOffset = startingOffset - misalignment;

    size_t amountToMap = min((_uint64) amountToProcess + misalignment + 2 * maxReadSizeInBytes,
                             (_uint64) fileSize - alignedOffset);
    //printf("Going to map %llu bytes starting at %lld (amount=%lld)\n", amountToMap, alignedOffset, amountToProcess);

    fileData = (char *) mmap(NULL, amountToMap, PROT_READ, MAP_SHARED, fd, alignedOffset);
    if (fileData == MAP_FAILED) {
        fprintf(stderr, "mmap failed on SAM file\n");
        exit(1);
    }

    pos = max(misalignment, (_int64) (headerSize - 1 - startingOffset));
    endPos = misalignment + amountToProcess;
    offsetMapped = alignedOffset;
    amountMapped = amountToMap;

    // Read to the first newline after our initial position
    while (pos < endPos && fileData[pos] != '\n') {
        pos++;
    }
    //printf("First newline is at %llu\n", pos);
    pos++;

    // If the first read has SAM_MULTI_SEGMENT and not SAM_FIRST_SEGMENT set, then it's part
    // of a pair that the reader for the range before us will process, so skip it.
    
    Read read;
    AlignmentResult alignmentResult;
    unsigned genomeLocation;
    bool isRC;
    unsigned mapQ;
    size_t lineLength;
    unsigned flag;

    getReadFromLine(genome, fileData + pos, fileData + amountMapped, 
                    &read, &alignmentResult, &genomeLocation, &isRC, &mapQ, &lineLength, 
                    &flag, NULL, NULL, clipping);

    if ((flag & SAM_MULTI_SEGMENT) && !(flag & SAM_FIRST_SEGMENT)) {
        pos += lineLength;
        //printf("Increasing pos by lineLength = %llu\n", lineLength);
    }

    // Do our first madvise()
    _uint64 amountToMadvise = min((_uint64) madviseSize, amountToMap - pos);
    int r = madvise(fileData + pos, amountToMadvise, MADV_WILLNEED);
    _ASSERT(r == 0);
    lastPosMadvised = pos;
}


bool MemMapSAMReader::getNextRead(
        Read *read,
        AlignmentResult *alignmentResult,
        unsigned *genomeLocation,
        bool *isRC,
        unsigned *mapQ,
        unsigned *flag,
        bool ignoreEndOfRange,
        const char **cigar)
{
    if (pos >= endPos) {
        return false;
    }
    //printf("getting next read at %llu\n", pos);

    size_t lineLength;

    getReadFromLine(genome, fileData + pos, fileData + amountMapped, read, alignmentResult,
                    genomeLocation, isRC, mapQ, &lineLength, flag, NULL, cigar, clipping);
    pos += lineLength;

    // Call madvise() to (a) start reading more bytes if we're past half our current
    // range and (b) tell the OS we won't need any stuff we've read in the past
    if (pos > lastPosMadvised + madviseSize / 2) {
        _uint64 offset = lastPosMadvised + madviseSize;
        _uint64 len = (offset > amountMapped ? 0 : min(amountMapped - offset, (_uint64) madviseSize));
        if (len > 0) {
            // Start reading new range
            int r = madvise(fileData + offset, len, MADV_WILLNEED);
            _ASSERT(r == 0);
        }
        if (lastPosMadvised >= madviseSize) {
          // Unload the range we had before our current one
          int r = madvise(fileData + lastPosMadvised - madviseSize, madviseSize, MADV_DONTNEED);
          _ASSERT(r == 0);
        }
        lastPosMadvised = offset;
    }

    return true;
}


void MemMapSAMReader::readDoneWithBuffer(unsigned *referenceCount)
{
    // Ignored because we only unmap the region when the whole reader is closed.
}


#endif  // _MSC_VER
