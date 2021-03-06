/**
 * This is code is released under the
 * Apache License Version 2.0 http://www.apache.org/licenses/.
 *
 * (c) Daniel Lemire, http://lemire.me/en/
 */

#ifndef PFOR2008_H_
#define PFOR2008_H_

#include "common.h"
#include "codecs.h"
#include "bitpacking.h"
#include "util.h"

using namespace std;

/**
 * Follows:
 *
 *  Zhang J, Long X, Suel T. Performance of compressed inverted list caching in search engines.
 *  Proceeedings of 17th Conference on the World Wide Web, Beijing, China, Huai J, Chen R,
 *  Hon H-W, Liu Y, Ma W-Y, Tomkins A, Zhang X (eds.). ACM Press: New York, April 2008; 387�396.
 *
 * It is very similar to PFor except that exceptions can be stored using 8, 16 or 32 bits.
 * So I just copied PFor and made the required changes.
 *
 */
class PFor2008: public IntegerCODEC {
public:
    enum {
        BlockSizeInUnitsOfPackSize = 4,
        PACKSIZE = 32,
        BlockSize = BlockSizeInUnitsOfPackSize * PACKSIZE,
        blocksizeinbits = gccbits(BlockSize)
    };
    // these are reusable buffers
    vector<uint32_t> codedcopy;
    vector<size_t> miss;
    typedef uint32_t DATATYPE;// this is so that our code looks more like the original paper

    PFor2008() :
        codedcopy(BlockSize), miss(BlockSize) {
    }
    // for delta coding, we don't use a base.
    static uint32_t determineBestBase(const DATATYPE * in, size_t size,
            const uint32_t maxb) {
        if (size == 0)
            return 0;
        const size_t defaultsamplesize = 64 * 1024;
        // the original paper describes sorting
        // a sample, but this only makes sense if you
        // are coding a frame of reference.
        size_t samplesize = size > defaultsamplesize ? defaultsamplesize : size;
        vector<uint32_t> freqs(33);
        // we choose the sample to be consecutive
        uint32_t rstart = size > samplesize ? (rand() % (size - samplesize))
                : 0;
        for (uint32_t k = rstart; k < rstart + samplesize; ++k) {
            freqs[asmbits(in[k])]++;
        }
        uint32_t bestb = maxb;
        uint32_t numberofexceptions = 0;
        double Erate = 0;
        double bestcost = maxb;
        for (uint32_t b = bestb - 1; b < maxb; --b) {
            numberofexceptions += freqs[b + 1];
            Erate = numberofexceptions * 1.0 / samplesize;
            /**
             * though this is not explicit in the original paper, you
             * need to somehow compensate for compulsory exceptions
             * when the chosen number of bits is small.
             *
             * We use their formula (3.1.5) to estimate actual number
             * of total exceptions, including compulsory exceptions.
             */
            if (numberofexceptions > 0) {
                double altErate = (Erate * 128 - 1) / (Erate * (1U << b));
                if (altErate > Erate)
                    Erate = altErate;
            }
            const double thiscost = b + Erate * maxb;
            if (thiscost <= bestcost) {
                bestcost = thiscost;
                bestb = b;
            }
        }
        return bestb;
    }

    // returns location of first exception or BlockSize if there is none
    size_t compressblockPFOR(const DATATYPE * __restrict__ in,
            uint32_t * __restrict__ outputbegin, const uint32_t b,
            DATATYPE * __restrict__ & exceptions) {
        if (b == 32) {
            for (size_t k = 0; k < BlockSize; ++k)
                *(outputbegin++) = *(in++);
            return BlockSize;
        }
        size_t exceptcounter = 0;
        const uint32_t maxgap = 1U << b;
        {
            vector<uint32_t>::iterator cci = codedcopy.begin();
            for (size_t k = 0; k < BlockSize; ++k, ++cci) {
                miss[exceptcounter] = k;
                exceptcounter += (in[k] >= maxgap);
            }

        }
        if (exceptcounter == 0) {
            packblock(in, outputbegin, b);
            return BlockSize;
        }
        codedcopy.assign(in, in + BlockSize);
        size_t firstexcept = miss[0];
        size_t prev = 0;
        *(exceptions++) = codedcopy[firstexcept];
        prev = firstexcept;
        if (maxgap < BlockSize) {
            for (size_t i = 1; i < exceptcounter; ++i) {
                size_t cur = miss[i];
                // they don't include this part, but it is required:
                while (cur > maxgap + prev) {
                    // compulsory exception
                    size_t compulcur = prev + maxgap;
                    *(exceptions++) = codedcopy[compulcur];
                    codedcopy[prev] = maxgap - 1;
                    prev = compulcur;
                }
                *(exceptions++) = codedcopy[cur];
                codedcopy[prev] = cur - prev - 1;
                prev = cur;
            }
        } else {
            for (size_t i = 1; i < exceptcounter; ++i) {
                size_t cur = miss[i];
                *(exceptions++) = codedcopy[cur];
                codedcopy[prev] = cur - prev - 1;
                prev = cur;
            }
        }
        packblock(&codedcopy[0], outputbegin, b);
        return firstexcept;
    }

    void packblock(const uint32_t * source, uint32_t * out, const uint32_t bit) {
        for (uint32_t j = 0; j != BlockSize; j += PACKSIZE) {
            fastpack(source + j, out, bit);
            out += bit;
        }
    }

    void unpackblock(const uint32_t * source, uint32_t * out,
            const uint32_t bit) {
        for (uint32_t j = 0; j != BlockSize; j += PACKSIZE) {
            fastunpack(source, out + j, bit);
            source += bit;
        }
    }

    void encodeArray(const uint32_t *in, const size_t len, uint32_t *out,
            size_t &nvalue) {
        *out++ = len;
        const uint32_t * const finalin(in + len);
        const uint32_t maxsize = (1U << (32 - blocksizeinbits - 1));
        size_t totalnvalue(1);
        for (size_t i = 0; i < len; i += maxsize) {
            size_t l = maxsize;
            if (i + maxsize > len) {
                l = len - i;
                assert(l <= maxsize);
            }
            size_t thisnvalue = nvalue - totalnvalue;
            assert(in + i + l <= finalin);
            __encodeArray(&in[i], l, out, thisnvalue);
            totalnvalue += thisnvalue;
            assert(totalnvalue <= nvalue);
            out += thisnvalue;
        }
        nvalue = totalnvalue;
    }
    const uint32_t * decodeArray(const uint32_t *in, const size_t len,
            uint32_t *out, size_t &nvalue) {
        nvalue = *in++;
        if (nvalue == 0)
            return in;
        const uint32_t * const initin = in;
        const uint32_t * const finalin = in + len;
        size_t totalnvalue(0);
        while (totalnvalue < nvalue) {
            size_t thisnvalue = nvalue - totalnvalue;
            const uint32_t * const befin(in);
            assert(finalin <= len + in);
            const uint32_t maxb = *in++;
            if (maxb == 32) {
                in
                        = __decodeArray<uint32_t> (in, finalin - in, out,
                                thisnvalue);
            } else if (maxb == 16) {
                in
                        = __decodeArray<uint16_t> (in, finalin - in, out,
                                thisnvalue);
            } else if (maxb == 8) {
                in = __decodeArray<uint8_t> (in, finalin - in, out, thisnvalue);
            } else {
                throw logic_error("corrupted?");
            }
            assert(in > befin);
            assert(in <= finalin);
            out += thisnvalue;
            totalnvalue += thisnvalue;
            assert(totalnvalue <= nvalue);
        }
        assert(in <= len + initin);
        assert(in <= finalin);
        nvalue = totalnvalue;
        return in;

    }

    uint32_t howmanybits(const uint32_t *in, const size_t len) {
        uint32_t accumulator = 0;
        for (uint32_t k = 0; k < len; ++k) {
            accumulator |= in[k];
        }
        if (accumulator >= (1U << 16))
            return 32;
        if (accumulator >= (1U << 8))
            return 16;
        return 8;
    }

    void __encodeArray(const uint32_t *in, const size_t len, uint32_t *out,
            size_t &nvalue) {
        const uint32_t maxb = howmanybits(in, len);
        checkifdivisibleby(len, BlockSize);
        const uint32_t * const initout(out);
        vector<DATATYPE> exceptions(len);
        DATATYPE * __restrict__ i = &exceptions[0];
        const uint32_t b = determineBestBase(in, len, maxb);
        *out++ = maxb;
        *out++ = len;
        *out++ = b;

        for (size_t k = 0; k < len / BlockSize; ++k) {
            uint32_t * const headerout(out);
            ++out;
            size_t firstexcept = compressblockPFOR(in, out, b, i);
            out += (BlockSize * b) / 32;
            in += BlockSize;
            const uint32_t bitsforfirstexcept = blocksizeinbits;
            const uint32_t firstexceptmask = (1U << blocksizeinbits) - 1;
            const size_t exceptindex = i - &exceptions[0];
            *headerout = (firstexcept & firstexceptmask) | (exceptindex
                    << bitsforfirstexcept);
        }
        const size_t howmanyexcept = i - &exceptions[0];
        if (maxb == 32) {
            for (uint32_t t = 0; t < howmanyexcept; ++t)
                *out++ = exceptions[t];
        } else if (maxb == 16) {
            uint16_t * out16 = reinterpret_cast<uint16_t *> (out);
            for (uint32_t t = 0; t < howmanyexcept; ++t)
                *out16++ = exceptions[t];
            out
                    = reinterpret_cast<uint32_t *> ((reinterpret_cast<uintptr_t> (out16)
                            + 3) & ~3);
        } else if (maxb == 8) {
            uint8_t * out8 = reinterpret_cast<uint8_t *> (out);
            for (uint32_t t = 0; t < howmanyexcept; ++t)
                *out8++ = exceptions[t];
            out
                    = reinterpret_cast<uint32_t *> ((reinterpret_cast<uintptr_t> (out8)
                            + 3) & ~3);
        } else {
            throw logic_error("should not happen");
        }

        nvalue = out - initout;
    }
    template<class EXCEPTTYPE>
    const uint32_t * __decodeArray(const uint32_t *in, const size_t len,
            uint32_t *out, size_t &nvalue) {
        const uint32_t * const initin(in);
        nvalue = *in++;
        checkifdivisibleby(nvalue, BlockSize);
        const uint32_t b = *in++;
        const EXCEPTTYPE * __restrict__ except =
                reinterpret_cast<const EXCEPTTYPE *> (in + nvalue * b / 32
                        + nvalue / BlockSize);
        const uint32_t bitsforfirstexcept = blocksizeinbits;
        const uint32_t firstexceptmask = (1U << blocksizeinbits) - 1;
        const EXCEPTTYPE * endexceptpointer = except;
        const EXCEPTTYPE * const initexcept(except);
        for (size_t k = 0; k < nvalue / BlockSize; ++k) {
            const uint32_t * const headerin(in);
            ++in;
            const uint32_t firstexcept = *headerin & firstexceptmask;
            const uint32_t exceptindex = *headerin >> bitsforfirstexcept;
            endexceptpointer = initexcept + exceptindex;
            uncompressblockPFOR(in, out, b, except, endexceptpointer,
                    firstexcept);
            in += (BlockSize * b) / 32;
            out += BlockSize;
        }
        assert(initin + len >= in);
        assert(
                initin + len
                        >= reinterpret_cast<const uint32_t *> (endexceptpointer));
        return reinterpret_cast<const uint32_t *> ((reinterpret_cast<uintptr_t> (endexceptpointer)
                + 3) & ~3);
    }

    template<class EXCEPTTYPE>
    void uncompressblockPFOR(
            const uint32_t * __restrict__ inputbegin,// points to the first packed word
            DATATYPE * __restrict__ outputbegin,
            const uint32_t b,
            const EXCEPTTYPE *__restrict__ & i, // i points to value of the first exception
            const EXCEPTTYPE * __restrict__ end_exception,
            size_t next_exception // points to the position of the first exception
    ) {
        unpackblock(inputbegin, reinterpret_cast<uint32_t *> (outputbegin), b); /* bit-unpack the values */
        for (size_t cur = next_exception; i != end_exception; cur
                = next_exception) {
            next_exception = cur + static_cast<size_t> (outputbegin[cur]) + 1;
            outputbegin[cur] = *(i++);
        }
    }

    virtual string name() const {
        ostringstream convert;
        convert << "PFor2008";
        return convert.str();
    }

};
#endif /* PFOR2008_H_ */
