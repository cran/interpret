// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "PrecompiledHeader.h"

#include <stdlib.h> // free
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

void EbmInteractionState::Free(EbmInteractionState * const pInteractionDetection) {
   LOG_0(TraceLevelInfo, "Entered EbmInteractionState::Free");

   if(nullptr != pInteractionDetection) {
      pInteractionDetection->m_dataSet.Destruct();
      free(pInteractionDetection->m_aFeatures);
      free(pInteractionDetection);
   }

   LOG_0(TraceLevelInfo, "Exited EbmInteractionState::Free");
}

EbmInteractionState * EbmInteractionState::Allocate(
   const ptrdiff_t runtimeLearningTypeOrCountTargetClasses,
   const size_t cFeatures,
   const FloatEbmType * const optionalTempParams,
   const EbmNativeFeature * const aNativeFeatures,
   const size_t cSamples,
   const void * const aTargets,
   const IntEbmType * const aBinnedData,
   const FloatEbmType * const aPredictorScores
) {
   // optionalTempParams isn't used by default.  It's meant to provide an easy way for python or other higher
   // level languages to pass EXPERIMENTAL temporary parameters easily to the C++ code.
   UNUSED(optionalTempParams);

   LOG_0(TraceLevelInfo, "Entered EbmInteractionState::Allocate");

   LOG_0(TraceLevelInfo, "EbmInteractionState::Allocate starting feature processing");
   Feature * aFeatures = nullptr;
   if(0 != cFeatures) {
      aFeatures = EbmMalloc<Feature>(cFeatures);
      if(nullptr == aFeatures) {
         LOG_0(TraceLevelWarning, "WARNING EbmInteractionState::Allocate nullptr == aFeatures");
         return nullptr;
      }
      const EbmNativeFeature * pFeatureInitialize = aNativeFeatures;
      const EbmNativeFeature * const pFeatureEnd = &aNativeFeatures[cFeatures];
      EBM_ASSERT(pFeatureInitialize < pFeatureEnd);
      size_t iFeatureInitialize = 0;
      do {
         static_assert(
            FeatureType::Ordinal == static_cast<FeatureType>(FeatureTypeOrdinal), "FeatureType::Ordinal must have the same value as FeatureTypeOrdinal"
            );
         static_assert(
            FeatureType::Nominal == static_cast<FeatureType>(FeatureTypeNominal), "FeatureType::Nominal must have the same value as FeatureTypeNominal"
            );
         if(FeatureTypeOrdinal != pFeatureInitialize->featureType && FeatureTypeNominal != pFeatureInitialize->featureType) {
            LOG_0(TraceLevelError, "ERROR EbmInteractionState::Allocate featureType must either be FeatureTypeOrdinal or FeatureTypeNominal");
            free(aFeatures);
            return nullptr;
         }
         FeatureType featureType = static_cast<FeatureType>(pFeatureInitialize->featureType);

         IntEbmType countBins = pFeatureInitialize->countBins;
         if(countBins < 0) {
            LOG_0(TraceLevelError, "ERROR EbmInteractionState::Allocate countBins cannot be negative");
            free(aFeatures);
            return nullptr;
         }
         if(0 == countBins && 0 != cSamples) {
            LOG_0(TraceLevelError, "ERROR EbmInteractionState::Allocate countBins cannot be zero if 0 < cSamples");
            free(aFeatures);
            return nullptr;
         }
         if(!IsNumberConvertable<size_t>(countBins)) {
            LOG_0(TraceLevelWarning, "WARNING EbmInteractionState::Allocate countBins is too high for us to allocate enough memory");
            free(aFeatures);
            return nullptr;
         }
         size_t cBins = static_cast<size_t>(countBins);
         if(0 == cBins) {
            // we can handle 0 == cBins even though that's a degenerate case that shouldn't be boosted on.  0 bins
            // can only occur if there were zero training and zero validation cases since the 
            // features would require a value, even if it was 0.
            LOG_0(TraceLevelInfo, "INFO EbmInteractionState::Allocate feature with 0 values");
         } else if(1 == cBins) {
            // we can handle 1 == cBins even though that's a degenerate case that shouldn't be boosted on. 
            // Dimensions with 1 bin don't contribute anything since they always have the same value.
            LOG_0(TraceLevelInfo, "INFO EbmInteractionState::Allocate feature with 1 value");
         }
         if(EBM_FALSE != pFeatureInitialize->hasMissing && EBM_TRUE != pFeatureInitialize->hasMissing) {
            LOG_0(TraceLevelError, "ERROR EbmInteractionState::Allocate hasMissing must either be EBM_TRUE or EBM_FALSE");
            free(aFeatures);
            return nullptr;
         }
         bool bMissing = EBM_FALSE != pFeatureInitialize->hasMissing;

         aFeatures[iFeatureInitialize].Initialize(cBins, iFeatureInitialize, featureType, bMissing);

         EBM_ASSERT(EBM_FALSE == pFeatureInitialize->hasMissing); // TODO : implement this, then remove this assert
         EBM_ASSERT(FeatureTypeOrdinal == pFeatureInitialize->featureType); // TODO : implement this, then remove this assert

         ++iFeatureInitialize;
         ++pFeatureInitialize;
      } while(pFeatureEnd != pFeatureInitialize);
   }
   LOG_0(TraceLevelInfo, "EbmInteractionState::Allocate done feature processing");

   EbmInteractionState * const pRet = EbmMalloc<EbmInteractionState>();
   if(nullptr == pRet) {
      free(aFeatures);
      return nullptr;
   }
   pRet->InitializeZero();

   pRet->m_runtimeLearningTypeOrCountTargetClasses = runtimeLearningTypeOrCountTargetClasses;
   pRet->m_cFeatures = cFeatures;
   pRet->m_aFeatures = aFeatures;
   pRet->m_cLogEnterMessages = 1000;
   pRet->m_cLogExitMessages = 1000;

   if(pRet->m_dataSet.Initialize(
      cFeatures,
      aFeatures,
      cSamples,
      aBinnedData,
      aTargets,
      aPredictorScores,
      runtimeLearningTypeOrCountTargetClasses
   )) {
      LOG_0(TraceLevelWarning, "WARNING EbmInteractionState::Allocate m_dataSet.Initialize");
      EbmInteractionState::Free(pRet);
      return nullptr;
   }

   LOG_0(TraceLevelInfo, "Exited EbmInteractionState::Allocate");
   return pRet;
}

// a*PredictorScores = logOdds for binary classification
// a*PredictorScores = logWeights for multiclass classification
// a*PredictorScores = predictedValue for regression
static EbmInteractionState * AllocateInteraction(
   IntEbmType countFeatures, 
   const EbmNativeFeature * features, 
   const ptrdiff_t runtimeLearningTypeOrCountTargetClasses, 
   IntEbmType countSamples, 
   const void * targets, 
   const IntEbmType * binnedData, 
   const FloatEbmType * predictorScores,
   const FloatEbmType * const optionalTempParams
) {
   // TODO : give AllocateInteraction the same calling parameter order as InitializeInteractionClassification

   if(countFeatures < 0) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction countFeatures must be positive");
      return nullptr;
   }
   if(0 != countFeatures && nullptr == features) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction features cannot be nullptr if 0 < countFeatures");
      return nullptr;
   }
   if(countSamples < 0) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction countSamples must be positive");
      return nullptr;
   }
   if(0 != countSamples && nullptr == targets) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction targets cannot be nullptr if 0 < countSamples");
      return nullptr;
   }
   if(0 != countSamples && 0 != countFeatures && nullptr == binnedData) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction binnedData cannot be nullptr if 0 < countSamples AND 0 < countFeatures");
      return nullptr;
   }
   if(0 != countSamples && nullptr == predictorScores) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction predictorScores cannot be nullptr if 0 < countSamples");
      return nullptr;
   }
   if(!IsNumberConvertable<size_t>(countFeatures)) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction !IsNumberConvertable<size_t>(countFeatures)");
      return nullptr;
   }
   if(!IsNumberConvertable<size_t>(countSamples)) {
      LOG_0(TraceLevelError, "ERROR AllocateInteraction !IsNumberConvertable<size_t>(countSamples)");
      return nullptr;
   }

   size_t cFeatures = static_cast<size_t>(countFeatures);
   size_t cSamples = static_cast<size_t>(countSamples);

   EbmInteractionState * const pEbmInteractionState = EbmInteractionState::Allocate(
      runtimeLearningTypeOrCountTargetClasses,
      cFeatures,
      optionalTempParams,
      features,
      cSamples,
      targets,
      binnedData,
      predictorScores
   );
   if(UNLIKELY(nullptr == pEbmInteractionState)) {
      LOG_0(TraceLevelWarning, "WARNING AllocateInteraction nullptr == pEbmInteractionState");
      return nullptr;
   }
   return pEbmInteractionState;
}

EBM_NATIVE_IMPORT_EXPORT_BODY PEbmInteraction EBM_NATIVE_CALLING_CONVENTION InitializeInteractionClassification(
   IntEbmType countTargetClasses,
   IntEbmType countFeatures,
   const EbmNativeFeature * features,
   IntEbmType countSamples,
   const IntEbmType * binnedData,
   const IntEbmType * targets,
   const FloatEbmType * predictorScores,
   const FloatEbmType * optionalTempParams
) {
   LOG_N(
      TraceLevelInfo, 
      "Entered InitializeInteractionClassification: countTargetClasses=%" IntEbmTypePrintf ", countFeatures=%" IntEbmTypePrintf 
      ", features=%p, countSamples=%" IntEbmTypePrintf ", binnedData=%p, targets=%p, predictorScores=%p, optionalTempParams=%p",
      countTargetClasses, 
      countFeatures, 
      static_cast<const void *>(features), 
      countSamples, 
      static_cast<const void *>(binnedData), 
      static_cast<const void *>(targets), 
      static_cast<const void *>(predictorScores),
      static_cast<const void *>(optionalTempParams)
   );
   if(countTargetClasses < 0) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification countTargetClasses can't be negative");
      return nullptr;
   }
   if(0 == countTargetClasses && 0 != countSamples) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification countTargetClasses can't be zero unless there are no samples");
      return nullptr;
   }
   if(!IsNumberConvertable<ptrdiff_t>(countTargetClasses)) {
      LOG_0(TraceLevelWarning, "WARNING InitializeInteractionClassification !IsNumberConvertable<ptrdiff_t>(countTargetClasses)");
      return nullptr;
   }
   const ptrdiff_t runtimeLearningTypeOrCountTargetClasses = static_cast<ptrdiff_t>(countTargetClasses);
   PEbmInteraction pEbmInteraction = reinterpret_cast<PEbmInteraction>(AllocateInteraction(
      countFeatures, 
      features, 
      runtimeLearningTypeOrCountTargetClasses, 
      countSamples, 
      targets, 
      binnedData, 
      predictorScores,
      optionalTempParams
   ));
   LOG_N(TraceLevelInfo, "Exited InitializeInteractionClassification %p", static_cast<void *>(pEbmInteraction));
   return pEbmInteraction;
}

EBM_NATIVE_IMPORT_EXPORT_BODY PEbmInteraction EBM_NATIVE_CALLING_CONVENTION InitializeInteractionRegression(
   IntEbmType countFeatures,
   const EbmNativeFeature * features,
   IntEbmType countSamples,
   const IntEbmType * binnedData,
   const FloatEbmType * targets,
   const FloatEbmType * predictorScores,
   const FloatEbmType * optionalTempParams
) {
   LOG_N(TraceLevelInfo, "Entered InitializeInteractionRegression: countFeatures=%" IntEbmTypePrintf ", features=%p, countSamples=%" IntEbmTypePrintf 
      ", binnedData=%p, targets=%p, predictorScores=%p, optionalTempParams=%p",
      countFeatures, 
      static_cast<const void *>(features), 
      countSamples, 
      static_cast<const void *>(binnedData), 
      static_cast<const void *>(targets), 
      static_cast<const void *>(predictorScores),
      static_cast<const void *>(optionalTempParams)
   );
   PEbmInteraction pEbmInteraction = reinterpret_cast<PEbmInteraction>(AllocateInteraction(
      countFeatures, 
      features, 
      k_regression, 
      countSamples, 
      targets, 
      binnedData, 
      predictorScores,
      optionalTempParams
   ));
   LOG_N(TraceLevelInfo, "Exited InitializeInteractionRegression %p", static_cast<void *>(pEbmInteraction));
   return pEbmInteraction;
}

EBM_NATIVE_IMPORT_EXPORT_BODY void EBM_NATIVE_CALLING_CONVENTION FreeInteraction(
   PEbmInteraction ebmInteraction
) {
   LOG_N(TraceLevelInfo, "Entered FreeInteraction: ebmInteraction=%p", static_cast<void *>(ebmInteraction));
   EbmInteractionState * pEbmInteractionState = reinterpret_cast<EbmInteractionState *>(ebmInteraction);

   // pEbmInteractionState is allowed to be nullptr.  We handle that inside EbmInteractionState::Free
   EbmInteractionState::Free(pEbmInteractionState);
   
   LOG_0(TraceLevelInfo, "Exited FreeInteraction");
}
