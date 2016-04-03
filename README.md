# SNAP

Scalable Nucleotide Alignment Program - <http://snap.cs.berkeley.edu>

## Overview

SNAP is a fast and accurate aligner for short DNA reads. It is optimized for
modern read lengths of 100 bases or higher, and takes advantage of these reads
to align data quickly through a hash-based indexing scheme.

## Documentation

A quick start guide and user manual are available in the `docs` folder, with
additional documentation at <http://snap.cs.berkeley.edu>.

## Building

Requirements:

- g++ version 4.6
- zlib 1.2.8 from http://zlib.net/


## Building for linux from OSX with holy build box

```bash
docker run -t -i --rm \
  -v `pwd`:/io \
  phusion/holy-build-box-64:latest \
  /hbb_exe/activate-exec \
  bash -x -c 'cd /io && make'
```

## Building for OSX

works best with clang - make sure to use the `osx` branch which has some osx-specific build fixes
