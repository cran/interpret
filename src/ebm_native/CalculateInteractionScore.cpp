// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "PrecompiledHeader.h"

#include <stddef.h> // size_t, ptrdiff_t
#include <limits> // numeric_limits

#include "ebm_native.h"
#include "EbmInternal.h"
#include "Logging.h" // EBM_ASSERT & LOG
// feature includes
#include "FeatureAtomic.h"
#include "FeatureGroup.h"
// dataset depends on features
#include "DataSetInteraction.h"
#include "CachedThreadResourcesInteraction.h"

#include "InteractionDetection.h"

#include "TensorTotalsSum.h"

extern void BinInteraction(
   EbmInteractionState * const pEbmInteractionState,
   const FeatureGroup * const pFeatureGroup,
   HistogramBucketBase * const aHistogramBuckets
#ifndef NDEBUG
   , const unsigned char * const aHistogramBucketsEndDebug
#endif // NDEBUG
);

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
);

static bool CalculateInteractionScoreInternal(
   CachedInteractionThreadResources * const pCachedThreadResources,
   EbmInteractionState * const pEbmInteractionState,
   const FeatureGroup * const pFeatureGroup,
   const size_t cSamplesRequiredForChildSplitMin,
   FloatEbmType * const pInteractionScoreReturn
) {
   // TODO : we NEVER use the denominator term in HistogramBucketVectorEntry when calculating interaction scores, but we're spending time calculating 
   // it, and it's taking up precious memory.  We should eliminate the denominator term HERE in our datastructures OR we should think whether we can 
   // use the denominator as part of the gain function!!!

   const ptrdiff_t runtimeLearningTypeOrCountTargetClasses = pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses();
   const bool bClassification = IsClassification(runtimeLearningTypeOrCountTargetClasses);

   LOG_0(TraceLevelVerbose, "Entered CalculateInteractionScoreInternal");

   const size_t cDimensions = pFeatureGroup->GetCountFeatures();
   EBM_ASSERT(1 <= cDimensions); // situations with 0 dimensions should have been filtered out before this function was called (but still inside the C++)

   size_t cAuxillaryBucketsForBuildFastTotals = 0;
   size_t cTotalBucketsMainSpace = 1;
   for(size_t iDimension = 0; iDimension < cDimensions; ++iDimension) {
      const size_t cBins = pFeatureGroup->GetFeatureGroupEntries()[iDimension].m_pFeature->GetCountBins();
      EBM_ASSERT(2 <= cBins); // situations with 1 bin should have been filtered out before this function was called (but still inside the C++)
      // if cBins could be 1, then we'd need to check at runtime for overflow of cAuxillaryBucketsForBuildFastTotals
      // if this wasn't true then we'd have to check IsAddError(cAuxillaryBucketsForBuildFastTotals, cTotalBucketsMainSpace) at runtime
      EBM_ASSERT(cAuxillaryBucketsForBuildFastTotals < cTotalBucketsMainSpace);
      // since cBins must be 2 or more, cAuxillaryBucketsForBuildFastTotals must grow slower than cTotalBucketsMainSpace, and we checked at allocation 
      // that cTotalBucketsMainSpace would not overflow
      EBM_ASSERT(!IsAddError(cAuxillaryBucketsForBuildFastTotals, cTotalBucketsMainSpace));
      // this can overflow, but if it does then we're guaranteed to catch the overflow via the multiplication check below
      cAuxillaryBucketsForBuildFastTotals += cTotalBucketsMainSpace;
      if(IsMultiplyError(cTotalBucketsMainSpace, cBins)) {
         // unlike in the boosting code where we check at allocation time if the tensor created overflows on multiplication
         // we don't know what group of features our caller will give us for calculating the interaction scores,
         // so we need to check if our caller gave us a tensor that overflows multiplication
         LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScoreInternal IsMultiplyError(cTotalBucketsMainSpace, cBins)");
         return true;
      }
      cTotalBucketsMainSpace *= cBins;
      // if this wasn't true then we'd have to check IsAddError(cAuxillaryBucketsForBuildFastTotals, cTotalBucketsMainSpace) at runtime
      EBM_ASSERT(cAuxillaryBucketsForBuildFastTotals < cTotalBucketsMainSpace);
   }

   const size_t cAuxillaryBucketsForSplitting = 4;
   const size_t cAuxillaryBuckets =
      cAuxillaryBucketsForBuildFastTotals < cAuxillaryBucketsForSplitting ? cAuxillaryBucketsForSplitting : cAuxillaryBucketsForBuildFastTotals;
   if(IsAddError(cTotalBucketsMainSpace, cAuxillaryBuckets)) {
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScoreInternal IsAddError(cTotalBucketsMainSpace, cAuxillaryBuckets)");
      return true;
   }
   const size_t cTotalBuckets = cTotalBucketsMainSpace + cAuxillaryBuckets;

   const size_t cVectorLength = GetVectorLength(runtimeLearningTypeOrCountTargetClasses);

   if(GetHistogramBucketSizeOverflow(bClassification, cVectorLength)) {
      LOG_0(
         TraceLevelWarning,
         "WARNING CalculateInteractionScoreInternal GetHistogramBucketSizeOverflow<bClassification>(cVectorLength)"
      );
      return true;
   }
   const size_t cBytesPerHistogramBucket = GetHistogramBucketSize(bClassification, cVectorLength);
   if(IsMultiplyError(cTotalBuckets, cBytesPerHistogramBucket)) {
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScoreInternal IsMultiplyError(cTotalBuckets, cBytesPerHistogramBucket)");
      return true;
   }
   const size_t cBytesBuffer = cTotalBuckets * cBytesPerHistogramBucket;

   // this doesn't need to be freed since it's tracked and re-used by the class CachedInteractionThreadResources
   HistogramBucketBase * const aHistogramBuckets = pCachedThreadResources->GetThreadByteBuffer1(cBytesBuffer);
   if(UNLIKELY(nullptr == aHistogramBuckets)) {
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScoreInternal nullptr == aHistogramBuckets");
      return true;
   }

   if(bClassification) {
      HistogramBucket<true> * const aHistogramBucketsLocal = aHistogramBuckets->GetHistogramBucket<true>();
      for(size_t i = 0; i < cTotalBuckets; ++i) {
         HistogramBucket<true> * const pHistogramBucket =
            GetHistogramBucketByIndex(cBytesPerHistogramBucket, aHistogramBucketsLocal, i);
         pHistogramBucket->Zero(cVectorLength);
      }
   } else {
      HistogramBucket<false> * const aHistogramBucketsLocal = aHistogramBuckets->GetHistogramBucket<false>();
      for(size_t i = 0; i < cTotalBuckets; ++i) {
         HistogramBucket<false> * const pHistogramBucket =
            GetHistogramBucketByIndex(cBytesPerHistogramBucket, aHistogramBucketsLocal, i);
         pHistogramBucket->Zero(cVectorLength);
      }
   }

   HistogramBucketBase * pAuxiliaryBucketZone =
      GetHistogramBucketByIndex(cBytesPerHistogramBucket, aHistogramBuckets, cTotalBucketsMainSpace);

#ifndef NDEBUG
   const unsigned char * const aHistogramBucketsEndDebug = reinterpret_cast<unsigned char *>(aHistogramBuckets) + cBytesBuffer;
#endif // NDEBUG

   BinInteraction(
      pEbmInteractionState,
      pFeatureGroup,
      aHistogramBuckets
#ifndef NDEBUG
      , aHistogramBucketsEndDebug
#endif // NDEBUG
   );

#ifndef NDEBUG
   // make a copy of the original binned buckets for debugging purposes
   size_t cTotalBucketsDebug = 1;
   for(size_t iDimensionDebug = 0; iDimensionDebug < cDimensions; ++iDimensionDebug) {
      const size_t cBins = pFeatureGroup->GetFeatureGroupEntries()[iDimensionDebug].m_pFeature->GetCountBins();
      EBM_ASSERT(!IsMultiplyError(cTotalBucketsDebug, cBins)); // we checked this above
      cTotalBucketsDebug *= cBins;
   }
   // we wouldn't have been able to allocate our main buffer above if this wasn't ok
   EBM_ASSERT(!IsMultiplyError(cTotalBucketsDebug, cBytesPerHistogramBucket));
   HistogramBucketBase * const aHistogramBucketsDebugCopy =
      EbmMalloc<HistogramBucketBase>(cTotalBucketsDebug, cBytesPerHistogramBucket);
   if(nullptr != aHistogramBucketsDebugCopy) {
      // if we can't allocate, don't fail.. just stop checking
      const size_t cBytesBufferDebug = cTotalBucketsDebug * cBytesPerHistogramBucket;
      memcpy(aHistogramBucketsDebugCopy, aHistogramBuckets, cBytesBufferDebug);
   }
#endif // NDEBUG

   TensorTotalsBuild(
      runtimeLearningTypeOrCountTargetClasses,
      pFeatureGroup,
      pAuxiliaryBucketZone,
      aHistogramBuckets
#ifndef NDEBUG
      , aHistogramBucketsDebugCopy
      , aHistogramBucketsEndDebug
#endif // NDEBUG
   );

   if(2 == cDimensions) {
      LOG_0(TraceLevelVerbose, "CalculateInteractionScoreInternal Starting bin sweep loop");

      FloatEbmType bestSplittingScore = FindBestInteractionGainPairs(
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

      LOG_0(TraceLevelVerbose, "CalculateInteractionScoreInternal Done bin sweep loop");

      if(nullptr != pInteractionScoreReturn) {
         // we started our score at zero, and didn't replace with anything lower, so it can't be below zero
         // if we collected a NaN value, then we kept it
         EBM_ASSERT(std::isnan(bestSplittingScore) || FloatEbmType { 0 } <= bestSplittingScore);
         EBM_ASSERT((!bClassification) || !std::isinf(bestSplittingScore));

         // if bestSplittingScore was NaN we make it zero so that it's not included.  If infinity, also don't include it since we overloaded something
         // even though bestSplittingScore shouldn't be +-infinity for classification, we check it for +-infinity 
         // here since it's most efficient to check that the exponential is all ones, which is the case only for +-infinity and NaN, but not others

         // comparing to max is a good way to check for +infinity without using infinity, which can be problematic on
         // some compilers with some compiler settings.  Using <= helps avoid optimization away because the compiler
         // might assume that nothing is larger than max if it thinks there's no +infinity

         if(UNLIKELY(UNLIKELY(std::isnan(bestSplittingScore)) || 
            UNLIKELY(std::numeric_limits<FloatEbmType>::max() <= bestSplittingScore))) {
            bestSplittingScore = FloatEbmType { 0 };
         }
         *pInteractionScoreReturn = bestSplittingScore;
      }
   } else {
      EBM_ASSERT(false); // we only support pairs currently
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScoreInternal 2 != cDimensions");

      // TODO: handle this better
      if(nullptr != pInteractionScoreReturn) {
         // for now, just return any interactions that have other than 2 dimensions as zero, which means they won't be considered
         *pInteractionScoreReturn = FloatEbmType { 0 };
      }
   }

#ifndef NDEBUG
   free(aHistogramBucketsDebugCopy);
#endif // NDEBUG

   LOG_0(TraceLevelVerbose, "Exited CalculateInteractionScoreInternal");
   return false;
}

// we made this a global because if we had put this variable inside the EbmInteractionState object, then we would need to dereference that before getting 
// the count.  By making this global we can send a log message incase a bad EbmInteractionState object is sent into us we only decrease the count if the 
// count is non-zero, so at worst if there is a race condition then we'll output this log message more times than desired, but we can live with that
static int g_cLogCalculateInteractionScoreParametersMessages = 10;

EBM_NATIVE_IMPORT_EXPORT_BODY IntEbmType EBM_NATIVE_CALLING_CONVENTION CalculateInteractionScore(
   PEbmInteraction ebmInteraction,
   IntEbmType countFeaturesInGroup,
   const IntEbmType * featureIndexes,
   IntEbmType countSamplesRequiredForChildSplitMin,
   FloatEbmType * interactionScoreOut
) {
   LOG_COUNTED_N(
      &g_cLogCalculateInteractionScoreParametersMessages,
      TraceLevelInfo,
      TraceLevelVerbose,
      "CalculateInteractionScore parameters: ebmInteraction=%p, countFeaturesInGroup=%" IntEbmTypePrintf ", featureIndexes=%p, countSamplesRequiredForChildSplitMin=%" IntEbmTypePrintf ", interactionScoreOut=%p",
      static_cast<void *>(ebmInteraction),
      countFeaturesInGroup,
      static_cast<const void *>(featureIndexes),
      countSamplesRequiredForChildSplitMin,
      static_cast<void *>(interactionScoreOut)
   );

   EbmInteractionState * pEbmInteractionState = reinterpret_cast<EbmInteractionState *>(ebmInteraction);
   if(nullptr == pEbmInteractionState) {
      if(LIKELY(nullptr != interactionScoreOut)) {
         *interactionScoreOut = FloatEbmType { 0 };
      }
      LOG_0(TraceLevelError, "ERROR CalculateInteractionScore ebmInteraction cannot be nullptr");
      return 1;
   }

   LOG_COUNTED_0(pEbmInteractionState->GetPointerCountLogEnterMessages(), TraceLevelInfo, TraceLevelVerbose, "Entered CalculateInteractionScore");

   if(countFeaturesInGroup < 0) {
      if(LIKELY(nullptr != interactionScoreOut)) {
         *interactionScoreOut = FloatEbmType { 0 };
      }
      LOG_0(TraceLevelError, "ERROR CalculateInteractionScore countFeaturesInGroup must be positive");
      return 1;
   }
   if(0 != countFeaturesInGroup && nullptr == featureIndexes) {
      if(LIKELY(nullptr != interactionScoreOut)) {
         *interactionScoreOut = FloatEbmType { 0 };
      }
      LOG_0(TraceLevelError, "ERROR CalculateInteractionScore featureIndexes cannot be nullptr if 0 < countFeaturesInGroup");
      return 1;
   }
   if(!IsNumberConvertable<size_t>(countFeaturesInGroup)) {
      if(LIKELY(nullptr != interactionScoreOut)) {
         *interactionScoreOut = FloatEbmType { 0 };
      }
      LOG_0(TraceLevelError, "ERROR CalculateInteractionScore countFeaturesInGroup too large to index");
      return 1;
   }
   size_t cFeaturesInGroup = static_cast<size_t>(countFeaturesInGroup);
   if(0 == cFeaturesInGroup) {
      LOG_0(TraceLevelInfo, "INFO CalculateInteractionScore empty feature group");
      if(nullptr != interactionScoreOut) {
         // we return the lowest value possible for the interaction score, but we don't return an error since we handle it even though we'd prefer our 
         // caler be smarter about this condition
         *interactionScoreOut = FloatEbmType { 0 };
      }
      return 0;
   }
   if(0 == pEbmInteractionState->GetDataSetByFeature()->GetCountSamples()) {
      // if there are zero samples, there isn't much basis to say whether there are interactions, so just return zero
      LOG_0(TraceLevelInfo, "INFO CalculateInteractionScore zero samples");
      if(nullptr != interactionScoreOut) {
         // we return the lowest value possible for the interaction score, but we don't return an error since we handle it even though we'd prefer our 
         // caler be smarter about this condition
         *interactionScoreOut = 0;
      }
      return 0;
   }

   size_t cSamplesRequiredForChildSplitMin = size_t { 1 }; // this is the min value
   if(IntEbmType { 1 } <= countSamplesRequiredForChildSplitMin) {
      cSamplesRequiredForChildSplitMin = static_cast<size_t>(countSamplesRequiredForChildSplitMin);
      if(!IsNumberConvertable<size_t>(countSamplesRequiredForChildSplitMin)) {
         // we can never exceed a size_t number of samples, so let's just set it to the maximum if we were going to overflow because it will generate 
         // the same results as if we used the true number
         cSamplesRequiredForChildSplitMin = std::numeric_limits<size_t>::max();
      }
   } else {
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScore countSamplesRequiredForChildSplitMin can't be less than 1.  Adjusting to 1.");
   }

   const Feature * const aFeatures = pEbmInteractionState->GetFeatures();
   const IntEbmType * pFeatureGroupIndex = featureIndexes;
   const IntEbmType * const pFeatureGroupIndexEnd = featureIndexes + cFeaturesInGroup;

   do {
      const IntEbmType indexFeatureInterop = *pFeatureGroupIndex;
      if(indexFeatureInterop < 0) {
         if(LIKELY(nullptr != interactionScoreOut)) {
            *interactionScoreOut = FloatEbmType { 0 };
         }
         LOG_0(TraceLevelError, "ERROR CalculateInteractionScore featureIndexes value cannot be negative");
         return 1;
      }
      if(!IsNumberConvertable<size_t>(indexFeatureInterop)) {
         if(LIKELY(nullptr != interactionScoreOut)) {
            *interactionScoreOut = FloatEbmType { 0 };
         }
         LOG_0(TraceLevelError, "ERROR CalculateInteractionScore featureIndexes value too big to reference memory");
         return 1;
      }
      const size_t iFeatureForGroup = static_cast<size_t>(indexFeatureInterop);
      if(pEbmInteractionState->GetCountFeatures() <= iFeatureForGroup) {
         if(LIKELY(nullptr != interactionScoreOut)) {
            *interactionScoreOut = FloatEbmType { 0 };
         }
         LOG_0(TraceLevelError, "ERROR CalculateInteractionScore featureIndexes value must be less than the number of features");
         return 1;
      }
      const Feature * const pFeature = &aFeatures[iFeatureForGroup];
      if(pFeature->GetCountBins() <= 1) {
         if(nullptr != interactionScoreOut) {
            // we return the lowest value possible for the interaction score, but we don't return an error since we handle it even though we'd prefer 
            // our caler be smarter about this condition
            *interactionScoreOut = 0;
         }
         LOG_0(TraceLevelInfo, "INFO CalculateInteractionScore feature with 0/1 value");
         return 0;
      }
      ++pFeatureGroupIndex;
   } while(pFeatureGroupIndexEnd != pFeatureGroupIndex);

   if(k_cDimensionsMax < cFeaturesInGroup) {
      // if we try to run with more than k_cDimensionsMax we'll exceed our memory capacity, so let's exit here instead
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScore k_cDimensionsMax < cFeaturesInGroup");
      return 1;
   }

   // put the pFeatureGroup object on the stack. We want to put it into a FeatureGroup object since we want to share code with boosting, 
   // which calls things like building the tensor totals (which is templated to be compiled many times)
   char FeatureGroupBuffer[FeatureGroup::GetFeatureGroupCountBytes(k_cDimensionsMax)];
   FeatureGroup * const pFeatureGroup = reinterpret_cast<FeatureGroup *>(&FeatureGroupBuffer);
   pFeatureGroup->Initialize(cFeaturesInGroup, 0);

   pFeatureGroupIndex = featureIndexes; // restart from the start
   FeatureGroupEntry * pFeatureGroupEntry = pFeatureGroup->GetFeatureGroupEntries();
   do {
      const IntEbmType indexFeatureInterop = *pFeatureGroupIndex;
      EBM_ASSERT(0 <= indexFeatureInterop);
      EBM_ASSERT(IsNumberConvertable<size_t>(indexFeatureInterop)); // we already checked indexFeatureInterop was good above
      size_t iFeatureForGroup = static_cast<size_t>(indexFeatureInterop);
      EBM_ASSERT(iFeatureForGroup < pEbmInteractionState->GetCountFeatures());
      const Feature * const pFeature = &aFeatures[iFeatureForGroup];
      EBM_ASSERT(2 <= pFeature->GetCountBins()); // we should have filtered out anything with 1 bin above

      pFeatureGroupEntry->m_pFeature = pFeature;
      ++pFeatureGroupEntry;
      ++pFeatureGroupIndex;
   } while(pFeatureGroupIndexEnd != pFeatureGroupIndex);

   if(ptrdiff_t { 0 } == pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses() || ptrdiff_t { 1 } == pEbmInteractionState->GetRuntimeLearningTypeOrCountTargetClasses()) {
      LOG_0(TraceLevelInfo, "INFO CalculateInteractionScore target with 0/1 classes");
      if(nullptr != interactionScoreOut) {
         // if there is only 1 classification target, then we can predict the outcome with 100% accuracy and there is no need for logits or 
         // interactions or anything else.  We return 0 since interactions have no benefit
         *interactionScoreOut = FloatEbmType { 0 };
      }
      return 0;
   }

   // TODO : be smarter about our CachedInteractionThreadResources, otherwise why have it?
   CachedInteractionThreadResources * const pCachedThreadResources = CachedInteractionThreadResources::Allocate();
   if(nullptr == pCachedThreadResources) {
      return 1;
   }

   IntEbmType ret = CalculateInteractionScoreInternal(
      pCachedThreadResources,
      pEbmInteractionState,
      pFeatureGroup,
      cSamplesRequiredForChildSplitMin,
      interactionScoreOut
   );

   CachedInteractionThreadResources::Free(pCachedThreadResources);

   if(0 != ret) {
      LOG_N(TraceLevelWarning, "WARNING CalculateInteractionScore returned %" IntEbmTypePrintf, ret);
   }

   if(nullptr != interactionScoreOut) {
      // if *interactionScoreOut was negative for floating point instability reasons, we zero it so that we don't return a negative number to our caller
      EBM_ASSERT(FloatEbmType { 0 } <= *interactionScoreOut);
      LOG_COUNTED_N(
         pEbmInteractionState->GetPointerCountLogExitMessages(),
         TraceLevelInfo,
         TraceLevelVerbose,
         "Exited CalculateInteractionScore %" FloatEbmTypePrintf, *interactionScoreOut
      );
   } else {
      LOG_COUNTED_0(pEbmInteractionState->GetPointerCountLogExitMessages(), TraceLevelInfo, TraceLevelVerbose, "Exited CalculateInteractionScore");
   }
   return ret;
}
