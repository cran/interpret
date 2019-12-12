// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "PrecompiledHeader.h"

#include <stddef.h> // size_t, ptrdiff_t

#include "ebm_native.h" // FloatEbmType
#include "EbmInternal.h" // INLINE_ALWAYS
#include "Logging.h" // EBM_ASSERT & LOG
#include "EbmStatisticUtils.h"

#include "FeatureAtomic.h"
#include "FeatureGroup.h"

#include "HistogramTargetEntry.h"
#include "HistogramBucket.h"

#include "InteractionDetection.h"

#include "TensorTotalsSum.h"

template<ptrdiff_t compilerLearningTypeOrCountTargetClasses>
class FindBestInteractionGainPairsInternal final {
public:

   FindBestInteractionGainPairsInternal() = delete; // this is a static class.  Do not construct

   static FloatEbmType Func(
      EbmInteractionState * const pEbmInteractionState,
      const FeatureGroup * const pFeatureGroup,
      const size_t cSamplesRequiredForChildSplitMin,
      HistogramBucketBase * pAuxiliaryBucketZoneBase,
      HistogramBucketBase * const aHistogramBucketsBase
#ifndef NDEBUG
      , const HistogramBucketBase * const aHistogramBucketsDebugCopyBase
      , const unsigned char * const aHistogramBucketsEndDebug
#endif // NDEBUG
   ) {
      constexpr bool bClassification = IsClassification(compilerLearningTypeOrCountTargetClasses);

      HistogramBucket<IsClassification(compilerLearningTypeOrCountTargetClasses)> * pAuxiliaryBucketZone =
         pAuxiliaryBucketZoneBase->GetHistogramBucket<bClassification>();

      HistogramBucket<IsClassification(compilerLearningTypeOrCountTargetClasses)> * const aHistogramBuckets =
         aHistogramBucketsBase->GetHistogramBucket<bClassification>();

#ifndef NDEBUG
      const HistogramBucket<IsClassification(compilerLearningTypeOrCountTargetClasses)> * const aHistogramBucketsDebugCopy =
         aHistogramBucketsDebugCopyBase->GetHistogramBucket<bClassification>();
#endif // NDEBUG

      const ptrdiff_t learningTypeOrCountTargetClasses = GET_LEARNING_TYPE_OR_COUNT_TARGET_CLASSES(
         compilerLearningTypeOrCountTargetClasses,
         pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses()
      );

      const size_t cVectorLength = GetVectorLength(learningTypeOrCountTargetClasses);
      const size_t cBytesPerHistogramBucket = GetHistogramBucketSize(bClassification, cVectorLength);

      HistogramBucket<bClassification> * pTotalsLowLow =
         GetHistogramBucketByIndex<bClassification>(cBytesPerHistogramBucket, pAuxiliaryBucketZone, 0);
      HistogramBucket<bClassification> * pTotalsLowHigh =
         GetHistogramBucketByIndex<bClassification>(cBytesPerHistogramBucket, pAuxiliaryBucketZone, 1);
      HistogramBucket<bClassification> * pTotalsHighLow =
         GetHistogramBucketByIndex<bClassification>(cBytesPerHistogramBucket, pAuxiliaryBucketZone, 2);
      HistogramBucket<bClassification> * pTotalsHighHigh =
         GetHistogramBucketByIndex<bClassification>(cBytesPerHistogramBucket, pAuxiliaryBucketZone, 3);

      const size_t cBinsDimension1 = pFeatureGroup->GetFeatureGroupEntries()[0].m_pFeature->GetCountBins();
      const size_t cBinsDimension2 = pFeatureGroup->GetFeatureGroupEntries()[1].m_pFeature->GetCountBins();
      // this function can handle 1 == cBins even though that's a degenerate case that shouldn't be boosted on 
      // (dimensions with 1 bin don't contribute anything since they always have the same value)
      EBM_ASSERT(1 <= cBinsDimension1);
      // this function can handle 1 == cBins even though that's a degenerate case that shouldn't be boosted on 
      // (dimensions with 1 bin don't contribute anything since they always have the same value)
      EBM_ASSERT(1 <= cBinsDimension2);

      EBM_ASSERT(0 < cSamplesRequiredForChildSplitMin);

      // never return anything above zero, which might happen due to numeric instability if we set this lower than 0
      FloatEbmType bestSplittingScore = FloatEbmType { 0 };

      size_t aiStart[k_cDimensionsMax];

      EBM_ASSERT(1 < cBinsDimension1);
      size_t iBin1 = 0;
      do {
         aiStart[0] = iBin1;
         EBM_ASSERT(1 < cBinsDimension2);
         size_t iBin2 = 0;
         do {
            aiStart[1] = iBin2;

            TensorTotalsSum<compilerLearningTypeOrCountTargetClasses, 2>(
               learningTypeOrCountTargetClasses,
               pFeatureGroup,
               aHistogramBuckets,
               aiStart,
               0x00,
               pTotalsLowLow
#ifndef NDEBUG
               , aHistogramBucketsDebugCopy
               , aHistogramBucketsEndDebug
#endif // NDEBUG
               );
            if(LIKELY(cSamplesRequiredForChildSplitMin <= pTotalsLowLow->GetCountSamplesInBucket())) {
               TensorTotalsSum<compilerLearningTypeOrCountTargetClasses, 2>(
                  learningTypeOrCountTargetClasses,
                  pFeatureGroup,
                  aHistogramBuckets,
                  aiStart,
                  0x02,
                  pTotalsLowHigh
#ifndef NDEBUG
                  , aHistogramBucketsDebugCopy
                  , aHistogramBucketsEndDebug
#endif // NDEBUG
                  );
               if(LIKELY(cSamplesRequiredForChildSplitMin <= pTotalsLowHigh->GetCountSamplesInBucket())) {
                  TensorTotalsSum<compilerLearningTypeOrCountTargetClasses, 2>(
                     learningTypeOrCountTargetClasses,
                     pFeatureGroup,
                     aHistogramBuckets,
                     aiStart,
                     0x01,
                     pTotalsHighLow
#ifndef NDEBUG
                     , aHistogramBucketsDebugCopy
                     , aHistogramBucketsEndDebug
#endif // NDEBUG
                     );
                  if(LIKELY(cSamplesRequiredForChildSplitMin <= pTotalsHighLow->GetCountSamplesInBucket())) {
                     TensorTotalsSum<compilerLearningTypeOrCountTargetClasses, 2>(
                        learningTypeOrCountTargetClasses,
                        pFeatureGroup,
                        aHistogramBuckets,
                        aiStart,
                        0x03,
                        pTotalsHighHigh
#ifndef NDEBUG
                        , aHistogramBucketsDebugCopy
                        , aHistogramBucketsEndDebug
#endif // NDEBUG
                        );
                     if(LIKELY(cSamplesRequiredForChildSplitMin <= pTotalsHighHigh->GetCountSamplesInBucket())) {
                        FloatEbmType splittingScore = 0;

                        FloatEbmType cLowLowSamplesInBucket = static_cast<FloatEbmType>(pTotalsLowLow->GetCountSamplesInBucket());
                        FloatEbmType cLowHighSamplesInBucket = static_cast<FloatEbmType>(pTotalsLowHigh->GetCountSamplesInBucket());
                        FloatEbmType cHighLowSamplesInBucket = static_cast<FloatEbmType>(pTotalsHighLow->GetCountSamplesInBucket());
                        FloatEbmType cHighHighSamplesInBucket = static_cast<FloatEbmType>(pTotalsHighHigh->GetCountSamplesInBucket());

                        HistogramBucketVectorEntry<bClassification> * const pHistogramBucketVectorEntryTotalsLowLow =
                           pTotalsLowLow->GetHistogramBucketVectorEntry();
                        HistogramBucketVectorEntry<bClassification> * const pHistogramBucketVectorEntryTotalsLowHigh =
                           pTotalsLowHigh->GetHistogramBucketVectorEntry();
                        HistogramBucketVectorEntry<bClassification> * const pHistogramBucketVectorEntryTotalsHighLow =
                           pTotalsHighLow->GetHistogramBucketVectorEntry();
                        HistogramBucketVectorEntry<bClassification> * const pHistogramBucketVectorEntryTotalsHighHigh =
                           pTotalsHighHigh->GetHistogramBucketVectorEntry();

                        for(size_t iVector = 0; iVector < cVectorLength; ++iVector) {
                           // TODO : we can make this faster by doing the division in ComputeNodeSplittingScore after we add all the numerators 
                           // (but only do this after we've determined the best node splitting score for classification, and the NewtonRaphsonStep for gain

                           const FloatEbmType splittingScoreUpdate1 = EbmStatistics::ComputeNodeSplittingScore(
                              pHistogramBucketVectorEntryTotalsLowLow[iVector].m_sumResidualError,
                              cLowLowSamplesInBucket
                           );
                           EBM_ASSERT(std::isnan(splittingScoreUpdate1) || FloatEbmType { 0 } <= splittingScoreUpdate1);
                           splittingScore += splittingScoreUpdate1;
                           const FloatEbmType splittingScoreUpdate2 = EbmStatistics::ComputeNodeSplittingScore(
                              pHistogramBucketVectorEntryTotalsLowHigh[iVector].m_sumResidualError, cLowHighSamplesInBucket);
                           EBM_ASSERT(std::isnan(splittingScoreUpdate2) || FloatEbmType { 0 } <= splittingScoreUpdate2);
                           splittingScore += splittingScoreUpdate2;
                           const FloatEbmType splittingScoreUpdate3 = EbmStatistics::ComputeNodeSplittingScore(
                              pHistogramBucketVectorEntryTotalsHighLow[iVector].m_sumResidualError, cHighLowSamplesInBucket);
                           EBM_ASSERT(std::isnan(splittingScoreUpdate3) || FloatEbmType { 0 } <= splittingScoreUpdate3);
                           splittingScore += splittingScoreUpdate3;
                           const FloatEbmType splittingScoreUpdate4 = EbmStatistics::ComputeNodeSplittingScore(
                              pHistogramBucketVectorEntryTotalsHighHigh[iVector].m_sumResidualError, cHighHighSamplesInBucket);
                           EBM_ASSERT(std::isnan(splittingScoreUpdate4) || FloatEbmType { 0 } <= splittingScoreUpdate4);
                           splittingScore += splittingScoreUpdate4;
                        }
                        EBM_ASSERT(std::isnan(splittingScore) || FloatEbmType { 0 } <= splittingScore); // sumations of positive numbers should be positive

                        // if we get a NaN result, we'd like to propagate it by making bestSplit NaN.  The rules for NaN values say that non equality
                        // comparisons are all false so, let's flip this comparison such that it should be true for NaN values.  If the compiler violates 
                        // NaN comparions rules, no big deal.  NaN values will get us soon and shut down boosting.
                        if(UNLIKELY(/* DO NOT CHANGE THIS WITHOUT READING THE ABOVE. WE DO THIS STRANGE COMPARISON FOR NaN values*/
                           !(splittingScore <= bestSplittingScore))) {
                           bestSplittingScore = splittingScore;
                        } else {
                           EBM_ASSERT(!std::isnan(splittingScore));
                        }
                     }
                  }
               }
            }
            ++iBin2;
         } while(iBin2 < cBinsDimension2 - 1);
         ++iBin1;
      } while(iBin1 < cBinsDimension1 - 1);

      return bestSplittingScore;
   }
};

template<ptrdiff_t compilerLearningTypeOrCountTargetClassesPossible>
class FindBestInteractionGainPairsTarget final {
public:

   FindBestInteractionGainPairsTarget() = delete; // this is a static class.  Do not construct

   INLINE_ALWAYS static FloatEbmType Func(
      EbmInteractionState * const pEbmInteractionState,
      const FeatureGroup * const pFeatureGroup,
      const size_t cSamplesRequiredForChildSplitMin,
      HistogramBucketBase * pAuxiliaryBucketZone,
      HistogramBucketBase * const aHistogramBuckets
#ifndef NDEBUG
      , const HistogramBucketBase * const aHistogramBucketsDebugCopy
      , const unsigned char * const aHistogramBucketsEndDebug
#endif // NDEBUG
   ) {
      static_assert(IsClassification(compilerLearningTypeOrCountTargetClassesPossible), "compilerLearningTypeOrCountTargetClassesPossible needs to be a classification");
      static_assert(compilerLearningTypeOrCountTargetClassesPossible <= k_cCompilerOptimizedTargetClassesMax, "We can't have this many items in a data pack.");

      const ptrdiff_t runtimeLearningTypeOrCountTargetClasses = pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses();
      EBM_ASSERT(IsClassification(runtimeLearningTypeOrCountTargetClasses));
      EBM_ASSERT(runtimeLearningTypeOrCountTargetClasses <= k_cCompilerOptimizedTargetClassesMax);

      if(compilerLearningTypeOrCountTargetClassesPossible == runtimeLearningTypeOrCountTargetClasses) {
         return FindBestInteractionGainPairsInternal<compilerLearningTypeOrCountTargetClassesPossible>::Func(
            pEbmInteractionState,
            pFeatureGroup,
            cSamplesRequiredForChildSplitMin,
            pAuxiliaryBucketZone,
            aHistogramBuckets
#ifndef NDEBUG
            , aHistogramBucketsDebugCopy
            , aHistogramBucketsEndDebug
#endif // NDEBUG
         );
      } else {
         return FindBestInteractionGainPairsTarget<compilerLearningTypeOrCountTargetClassesPossible + 1>::Func(
            pEbmInteractionState,
            pFeatureGroup,
            cSamplesRequiredForChildSplitMin,
            pAuxiliaryBucketZone,
            aHistogramBuckets
#ifndef NDEBUG
            , aHistogramBucketsDebugCopy
            , aHistogramBucketsEndDebug
#endif // NDEBUG
         );
      }
   }
};

template<>
class FindBestInteractionGainPairsTarget<k_cCompilerOptimizedTargetClassesMax + 1> final {
public:

   FindBestInteractionGainPairsTarget() = delete; // this is a static class.  Do not construct

   INLINE_ALWAYS static FloatEbmType Func(
      EbmInteractionState * const pEbmInteractionState,
      const FeatureGroup * const pFeatureGroup,
      const size_t cSamplesRequiredForChildSplitMin,
      HistogramBucketBase * pAuxiliaryBucketZone,
      HistogramBucketBase * const aHistogramBuckets
#ifndef NDEBUG
      , const HistogramBucketBase * const aHistogramBucketsDebugCopy
      , const unsigned char * const aHistogramBucketsEndDebug
#endif // NDEBUG
   ) {
      static_assert(IsClassification(k_cCompilerOptimizedTargetClassesMax), "k_cCompilerOptimizedTargetClassesMax needs to be a classification");

      EBM_ASSERT(IsClassification(pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses()));
      EBM_ASSERT(k_cCompilerOptimizedTargetClassesMax < pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses());

      return FindBestInteractionGainPairsInternal<k_dynamicClassification>::Func(
         pEbmInteractionState,
         pFeatureGroup,
         cSamplesRequiredForChildSplitMin,
         pAuxiliaryBucketZone,
         aHistogramBuckets
#ifndef NDEBUG
         , aHistogramBucketsDebugCopy
         , aHistogramBucketsEndDebug
#endif // NDEBUG
      );
   }
};

extern FloatEbmType FindBestInteractionGainPairs(
   EbmInteractionState * const pEbmInteractionState,
   const FeatureGroup * const pFeatureGroup,
   const size_t cSamplesRequiredForChildSplitMin,
   HistogramBucketBase * pAuxiliaryBucketZone,
   HistogramBucketBase * const aHistogramBuckets
#ifndef NDEBUG
   , const HistogramBucketBase * const aHistogramBucketsDebugCopy
   , const unsigned char * const aHistogramBucketsEndDebug
#endif // NDEBUG
) {
   const ptrdiff_t runtimeLearningTypeOrCountTargetClasses = pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses();

   if(IsClassification(runtimeLearningTypeOrCountTargetClasses)) {
      return FindBestInteractionGainPairsTarget<2>::Func(
         pEbmInteractionState,
         pFeatureGroup,
         cSamplesRequiredForChildSplitMin,
         pAuxiliaryBucketZone,
         aHistogramBuckets
#ifndef NDEBUG
         , aHistogramBucketsDebugCopy
         , aHistogramBucketsEndDebug
#endif // NDEBUG
      );
   } else {
      EBM_ASSERT(IsRegression(runtimeLearningTypeOrCountTargetClasses));
      return FindBestInteractionGainPairsInternal<k_regression>::Func(
         pEbmInteractionState,
         pFeatureGroup,
         cSamplesRequiredForChildSplitMin,
         pAuxiliaryBucketZone,
         aHistogramBuckets
#ifndef NDEBUG
         , aHistogramBucketsDebugCopy
         , aHistogramBucketsEndDebug
#endif // NDEBUG
      );
   }
}

