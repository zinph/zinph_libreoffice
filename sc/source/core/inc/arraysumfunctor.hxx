/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#ifndef INCLUDED_SC_SOURCE_CORE_INC_ARRAYSUMFUNCTOR_HXX
#define INCLUDED_SC_SOURCE_CORE_INC_ARRAYSUMFUNCTOR_HXX

#include <emmintrin.h>
#include <tools/cpuid.hxx>

namespace sc
{

struct ArraySumFunctor
{
private:
    const double* mpArray;
    size_t mnSize;

public:
    ArraySumFunctor(const double* pArray, size_t nSize)
        : mpArray(pArray)
        , mnSize(nSize)
    {
    }

    double operator() () const
    {
        static bool hasSSE2 = tools::cpuid::hasSSE2();
        printf("SSE used %d\n", hasSSE2);

        double fSum = 0.0;
        size_t i = 0;

        if (hasSSE2)
            fSum += executeSSE2(i);
        else
            fSum += executeUnrolled(i);

        // sum rest of the array

        for (; i < mnSize; ++i)
            fSum += mpArray[i];

        return fSum;
    }

private:
    inline double executeSSE2(size_t& i) const
    {
        double fSum = 0.0;
        size_t nUnrolledSize = mnSize - (mnSize % 4);

        if (nUnrolledSize > 0)
        {
            register __m128d sum1 = _mm_set_pd(0.0, 0.0);
            register __m128d sum2 = _mm_set_pd(0.0, 0.0);

            const double* pCurrent = mpArray;

            for (; i < nUnrolledSize; i += 4)
            {
                sum1 = _mm_add_pd(sum1, _mm_loadu_pd(pCurrent));
                pCurrent += 2;

                sum2 = _mm_add_pd(sum2, _mm_loadu_pd(pCurrent));
                pCurrent += 2;
            }
            sum1 = _mm_add_pd(sum1, sum2);

            double temp;

            _mm_storel_pd(&temp, sum1);
            fSum += temp;

            _mm_storeh_pd(&temp, sum1);
            fSum += temp;
        }
        return fSum;
    }

    inline double executeUnrolled(size_t& i) const
    {
        size_t nUnrolledSize = mnSize - (mnSize % 4);

        if (nUnrolledSize > 0)
        {
            double sum0 = 0.0;
            double sum1 = 0.0;
            double sum2 = 0.0;
            double sum3 = 0.0;

            const double* pCurrent = mpArray;

            for (; i < nUnrolledSize; i += 4)
            {
                sum0 += *pCurrent++;
                sum1 += *pCurrent++;
                sum2 += *pCurrent++;
                sum3 += *pCurrent++;
            }
            return sum0 + sum1 + sum2 + sum3;
        }
        return 0.0;
    }
};

} // end namespace sc

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
