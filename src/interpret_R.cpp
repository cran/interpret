// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include <cmath> // std::isnan, std::isinf
#include <limits> // std::numeric_limits
#include <cstring> // memcpy, strcmp
#include <algorithm> // std::min, std::max

#include "ebm_native.h"
#include "EbmInternal.h"
#include "Logging.h"
#include "Booster.h"
#include "InteractionDetection.h"

#include <Rinternals.h>
#include <R_ext/Visibility.h>

// when R compiles this library, on some systems it can generate a "NOTE installed size is.." meaning the C++ compiled into a library produces too big a
// library.  We would want to disable the -g flag (with -g0), but according to this, it's not possible currently:
// https://stat.ethz.ch/pipermail/r-devel/2016-October/073273.html

// TODO: switch logging to use the R logging infrastructure when invoked from R, BUT calling error or warning will generate longjumps, which 
//   bypass the regular return mechanisms.  We need to use R_tryCatch (which is older than R_UnwindProtect) to not leak memory that we allocate 
//   before calling the R error or warning functions

INLINE_ALWAYS bool IsSingleDoubleVector(const SEXP sexp) {
   if(REALSXP != TYPEOF(sexp)) {
      return false;
   }
   if(R_xlen_t { 1 } != xlength(sexp)) {
      return false;
   }
   return true;
}

INLINE_ALWAYS bool IsSingleIntVector(const SEXP sexp) {
   if(INTSXP != TYPEOF(sexp)) {
      return false;
   }
   if(R_xlen_t { 1 } != xlength(sexp)) {
      return false;
   }
   return true;
}

INLINE_ALWAYS bool IsSingleBoolVector(const SEXP sexp) {
   if(LGLSXP != TYPEOF(sexp)) {
      return false;
   }
   if(R_xlen_t { 1 } != xlength(sexp)) {
      return false;
   }
   return true;
}

INLINE_ALWAYS bool IsDoubleToIntEbmTypeIndexValid(const double val) {
   if(std::isnan(val)) {
      return false;
   }
   static_assert(std::numeric_limits<double>::is_iec559, "we need is_iec559 to know that comparisons to infinity and -infinity to normal numbers work");
   if(val < double { 0 }) {
      return false;
   }
   if(std::min(
      static_cast<double>(std::numeric_limits<size_t>::max()), 
      std::min(double { R_XLEN_T_MAX }, static_cast<double>(std::numeric_limits<IntEbmType>::max()))) < val
   ) {
      return false;
   }
   return true;
}

void BoostingFinalizer(SEXP boostingRPointer) {
   EBM_ASSERT(nullptr != boostingRPointer); // shouldn't be possible
   if(EXTPTRSXP == TYPEOF(boostingRPointer)) {
      PEbmBoosting pEbmBoosting = static_cast<PEbmBoosting>(R_ExternalPtrAddr(boostingRPointer));
      if(nullptr != pEbmBoosting) {
         FreeBoosting(pEbmBoosting);
         R_ClearExternalPtr(boostingRPointer);
      }
   }
}

void InteractionFinalizer(SEXP interactionRPointer) {
   EBM_ASSERT(nullptr != interactionRPointer); // shouldn't be possible
   if(EXTPTRSXP == TYPEOF(interactionRPointer)) {
      PEbmInteraction pInteraction = static_cast<PEbmInteraction>(R_ExternalPtrAddr(interactionRPointer));
      if(nullptr != pInteraction) {
         FreeInteraction(pInteraction);
         R_ClearExternalPtr(interactionRPointer);
      }
   }
}

EbmNativeFeature * ConvertFeatures(const SEXP features, size_t * const pcFeatures) {
   if(VECSXP != TYPEOF(features)) {
      LOG_0(TraceLevelError, "ERROR ConvertFeatures VECSXP != TYPEOF(features)");
      return nullptr;
   }
   const R_xlen_t countFeaturesR = xlength(features);
   if(!IsNumberConvertable<size_t>(countFeaturesR)) {
      LOG_0(TraceLevelError, "ERROR ConvertFeatures !IsNumberConvertable<size_t>(countFeaturesR)");
      return nullptr;
   }
   const size_t cFeatures = static_cast<size_t>(countFeaturesR);
   if(!IsNumberConvertable<IntEbmType>(cFeatures)) {
      LOG_0(TraceLevelError, "ERROR ConvertFeatures !IsNumberConvertable<IntEbmType>(cFeatures)");
      return nullptr;
   }
   *pcFeatures = cFeatures;

   EbmNativeFeature * const aFeatures = reinterpret_cast<EbmNativeFeature *>(R_alloc(cFeatures, static_cast<int>(sizeof(EbmNativeFeature))));
   // R_alloc doesn't return nullptr, so we don't need to check aFeatures
   EbmNativeFeature * pFeature = aFeatures;
   for(size_t iFeature = 0; iFeature < cFeatures; ++iFeature) {
      const SEXP oneFeature = VECTOR_ELT(features, iFeature);
      EBM_ASSERT(nullptr != oneFeature);
      if(VECSXP != TYPEOF(oneFeature)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatures VECSXP != TYPEOF(oneFeature)");
         return nullptr;
      }
      constexpr size_t cItems = 3;
      if(R_xlen_t { cItems } != xlength(oneFeature)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatures R_xlen_t { cItems } != xlength(oneFeature)");
         return nullptr;
      }
      const SEXP fieldNames = getAttrib(oneFeature, R_NamesSymbol);
      EBM_ASSERT(nullptr != fieldNames);
      if(STRSXP != TYPEOF(fieldNames)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatures STRSXP != TYPEOF(fieldNames)");
         return nullptr;
      }
      if(R_xlen_t { cItems } != xlength(fieldNames)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatures R_xlen_t { cItems } != xlength(fieldNames)");
         return nullptr;
      }

      bool bCountBinsFound = false;
      bool bHasMissingFound = false;
      bool bFeatureTypeFound = false;
      for(size_t iName = 0; iName < cItems; ++iName) {
         const SEXP nameR = STRING_ELT(fieldNames, iName);
         if(CHARSXP != TYPEOF(nameR)) {
            LOG_0(TraceLevelError, "ERROR ConvertFeatures CHARSXP != TYPEOF(nameR)");
            return nullptr;
         }
         const char * pName = CHAR(nameR);
         if(0 == strcmp("n_bins", pName)) {
            if(bCountBinsFound) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures bCountBinsFound");
               return nullptr;
            }

            SEXP val = VECTOR_ELT(oneFeature, iName);
            if(REALSXP != TYPEOF(val)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures REALSXP != TYPEOF(value)");
               return nullptr;
            }
            if(1 != xlength(val)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures 1 != xlength(val)");
               return nullptr;
            }

            double countBinsDouble = REAL(val)[0];
            if(!IsDoubleToIntEbmTypeIndexValid(countBinsDouble)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures !IsDoubleToIntEbmTypeIndexValid(countBinsDouble)");
               return nullptr;
            }
            pFeature->countBins = static_cast<IntEbmType>(countBinsDouble);
            bCountBinsFound = true;
         } else if(0 == strcmp("has_missing", pName)) {
            if(bHasMissingFound) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures bHasMissingFound");
               return nullptr;
            }

            SEXP val = VECTOR_ELT(oneFeature, iName);
            if(LGLSXP != TYPEOF(val)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures LGLSXP != TYPEOF(value)");
               return nullptr;
            }
            if(1 != xlength(val)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures 1 != xlength(val)");
               return nullptr;
            }

            const Rboolean hasMissing = static_cast<Rboolean>(LOGICAL(val)[0]);
            if(Rboolean::FALSE != hasMissing && Rboolean::TRUE != hasMissing) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures Rboolean::FALSE != hasMissing && Rboolean::TRUE != hasMissing");
               return nullptr;
            }
            pFeature->hasMissing = Rboolean::FALSE != hasMissing ? EBM_TRUE : EBM_FALSE;
            bHasMissingFound = true;
         } else if(0 == strcmp("feature_type", pName)) {
            if(bFeatureTypeFound) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures bFeatureTypeFound");
               return nullptr;
            }

            SEXP val = VECTOR_ELT(oneFeature, iName);

            if(STRSXP != TYPEOF(val)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures STRSXP != TYPEOF(value)");
               return nullptr;
            }
            if(1 != xlength(val)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures 1 != xlength(val)");
               return nullptr;
            }

            const SEXP featureTypeR = STRING_ELT(val, 0);
            if(CHARSXP != TYPEOF(featureTypeR)) {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures CHARSXP != TYPEOF(featureTypeR)");
               return nullptr;
            }

            bFeatureTypeFound = true;
            const char * pFeatureType = CHAR(featureTypeR);
            if(0 == strcmp("ordinal", pFeatureType)) {
               pFeature->featureType = FeatureTypeOrdinal;
            } else if(0 == strcmp("nominal", pFeatureType)) {
               pFeature->featureType = FeatureTypeNominal;
            } else {
               LOG_0(TraceLevelError, "ERROR ConvertFeatures unrecognized pFeatureType");
               return nullptr;
            }
         } else {
            LOG_0(TraceLevelError, "ERROR ConvertFeatures unrecognized pName");
            return nullptr;
         }
      }
      if(!bCountBinsFound) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatures !bCountBinsFound");
         return nullptr;
      }
      if(!bHasMissingFound) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatures !bHasMissingFound");
         return nullptr;
      }
      if(!bFeatureTypeFound) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatures !bFeatureTypeFound");
         return nullptr;
      }
      ++pFeature;
   }
   return aFeatures;
}

EbmNativeFeatureGroup * ConvertFeatureGroups(const SEXP featureGroups, size_t * const pcFeatureGroups) {
   if(VECSXP != TYPEOF(featureGroups)) {
      LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups VECSXP != TYPEOF(featureGroups)");
      return nullptr;
   }

   const R_xlen_t countFeatureGroupsR = xlength(featureGroups);
   if(!IsNumberConvertable<size_t>(countFeatureGroupsR)) {
      LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups !IsNumberConvertable<size_t>(countFeatureGroupsR)");
      return nullptr;
   }
   const size_t cFeatureGroups = static_cast<size_t>(countFeatureGroupsR);
   if(!IsNumberConvertable<IntEbmType>(cFeatureGroups)) {
      LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups !IsNumberConvertable<IntEbmType>(cFeatureGroups)");
      return nullptr;
   }
   *pcFeatureGroups = cFeatureGroups;

   EbmNativeFeatureGroup * const aFeatureGroups = reinterpret_cast<EbmNativeFeatureGroup *>(
      R_alloc(cFeatureGroups, static_cast<int>(sizeof(EbmNativeFeatureGroup))));
   // R_alloc doesn't return nullptr, so we don't need to check aFeatureGroups
   EbmNativeFeatureGroup * pFeatureGroup = aFeatureGroups;
   for(size_t iFeatureGroup = 0; iFeatureGroup < cFeatureGroups; ++iFeatureGroup) {
      const SEXP oneFeatureGroup = VECTOR_ELT(featureGroups, iFeatureGroup);
      EBM_ASSERT(nullptr != oneFeatureGroup);
      if(VECSXP != TYPEOF(oneFeatureGroup)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups VECSXP != TYPEOF(oneFeatureGroup)");
         return nullptr;
      }

      constexpr size_t cItems = 1;
      if(R_xlen_t { cItems } != xlength(oneFeatureGroup)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups R_xlen_t { cItems } != xlength(oneFeatureGroup)");
         return nullptr;
      }
      const SEXP fieldNames = getAttrib(oneFeatureGroup, R_NamesSymbol);
      EBM_ASSERT(nullptr != fieldNames);
      if(STRSXP != TYPEOF(fieldNames)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups STRSXP != TYPEOF(fieldNames)");
         return nullptr;
      }
      if(R_xlen_t { cItems } != xlength(fieldNames)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups R_xlen_t { cItems } != xlength(fieldNames)");
         return nullptr;
      }

      const SEXP nameR = STRING_ELT(fieldNames, 0);
      if(CHARSXP != TYPEOF(nameR)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups CHARSXP != TYPEOF(nameR)");
         return nullptr;
      }
      const char * pName = CHAR(nameR);
      if(0 != strcmp("n_features", pName)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups 0 != strcmp(\"n_features\", pName");
         return nullptr;
      }

      SEXP val = VECTOR_ELT(oneFeatureGroup, 0);
      if(REALSXP != TYPEOF(val)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups REALSXP != TYPEOF(value)");
         return nullptr;
      }
      if(1 != xlength(val)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups 1 != xlength(val)");
         return nullptr;
      }

      double countFeaturesInGroupDouble = REAL(val)[0];
      if(!IsDoubleToIntEbmTypeIndexValid(countFeaturesInGroupDouble)) {
         LOG_0(TraceLevelError, "ERROR ConvertFeatureGroups !IsDoubleToIntEbmTypeIndexValid(countFeaturesInGroupDouble)");
         return nullptr;
      }
      pFeatureGroup->countFeaturesInGroup = static_cast<IntEbmType>(countFeaturesInGroupDouble);

      ++pFeatureGroup;
   }
   return aFeatureGroups;
}

size_t CountFeatureGroupsIndexes(const size_t cFeatureGroups, const EbmNativeFeatureGroup * const aFeatureGroups) {
   size_t cFeatureGroupsIndexes = 0;
   if(0 != cFeatureGroups) {
      const EbmNativeFeatureGroup * pFeatureGroup = aFeatureGroups;
      const EbmNativeFeatureGroup * const pFeatureGroupEnd = aFeatureGroups + cFeatureGroups;
      do {
         const IntEbmType countFeaturesInGroup = pFeatureGroup->countFeaturesInGroup;
         if(!IsNumberConvertable<size_t>(countFeaturesInGroup)) {
            LOG_0(TraceLevelError, "ERROR CountFeatureGroupsIndexes !IsNumberConvertable<size_t>(countFeaturesInGroup)");
            return SIZE_MAX;
         }
         const size_t cFeaturesInGroup = static_cast<size_t>(countFeaturesInGroup);
         if(IsAddError(cFeatureGroupsIndexes, cFeaturesInGroup)) {
            LOG_0(TraceLevelError, "ERROR CountFeatureGroupsIndexes IsAddError(cFeatureGroupsIndexes, cFeaturesInGroup)");
            return SIZE_MAX;
         }
         cFeatureGroupsIndexes += cFeaturesInGroup;
         ++pFeatureGroup;
      } while(pFeatureGroupEnd != pFeatureGroup);
   }
   return cFeatureGroupsIndexes;
}

bool ConvertDoublesToIndexes(const SEXP items, size_t * const pcItems, const IntEbmType * * const pRet) {
   EBM_ASSERT(nullptr != items);
   EBM_ASSERT(nullptr != pcItems);
   if(REALSXP != TYPEOF(items)) {
      LOG_0(TraceLevelError, "ERROR ConvertDoublesToIndexes REALSXP != TYPEOF(items)");
      return true;
   }
   const R_xlen_t countItemsR = xlength(items);
   if(!IsNumberConvertable<size_t>(countItemsR)) {
      LOG_0(TraceLevelError, "ERROR ConvertDoublesToIndexes !IsNumberConvertable<size_t>(countItemsR)");
      return true;
   }
   const size_t cItems = static_cast<size_t>(countItemsR);
   if(!IsNumberConvertable<IntEbmType>(cItems)) {
      LOG_0(TraceLevelError, "ERROR ConvertDoublesToIndexes !IsNumberConvertable<IntEbmType>(cItems)");
      return true;
   }
   *pcItems = cItems;

   IntEbmType * aItems = nullptr;
   if(0 != cItems) {
      aItems = reinterpret_cast<IntEbmType *>(R_alloc(cItems, static_cast<int>(sizeof(IntEbmType))));
      // R_alloc doesn't return nullptr, so we don't need to check aItems
      IntEbmType * pItem = aItems;
      const IntEbmType * const pItemEnd = aItems + cItems;
      const double * pOriginal = REAL(items);
      do {
         const double val = *pOriginal;
         if(!IsDoubleToIntEbmTypeIndexValid(val)) {
            LOG_0(TraceLevelError, "ERROR ConvertDoublesToIndexes !IsDoubleToIntEbmTypeIndexValid(val)");
            return true;
         }
         *pItem = static_cast<IntEbmType>(val);
         ++pItem;
         ++pOriginal;
      } while(pItemEnd != pItem);
   }
   *pRet = aItems;
   return false;
}

bool ConvertDoublesToDoubles(const SEXP items, size_t * const pcItems, const FloatEbmType * * const pRet) {
   EBM_ASSERT(nullptr != items);
   EBM_ASSERT(nullptr != pcItems);
   if(REALSXP != TYPEOF(items)) {
      LOG_0(TraceLevelError, "ERROR ConvertDoublesToDoubles REALSXP != TYPEOF(items)");
      return true;
   }
   const R_xlen_t countItemsR = xlength(items);
   if(!IsNumberConvertable<size_t>(countItemsR)) {
      LOG_0(TraceLevelError, "ERROR ConvertDoublesToDoubles !IsNumberConvertable<size_t>(countItemsR)");
      return true;
   }
   const size_t cItems = static_cast<size_t>(countItemsR);
   if(!IsNumberConvertable<IntEbmType>(cItems)) {
      LOG_0(TraceLevelError, "ERROR ConvertDoublesToDoubles !IsNumberConvertable<IntEbmType>(cItems)");
      return true;
   }
   *pcItems = cItems;

   FloatEbmType * aItems = nullptr;
   if(0 != cItems) {
      aItems = reinterpret_cast<FloatEbmType *>(R_alloc(cItems, static_cast<int>(sizeof(FloatEbmType))));
      // R_alloc doesn't return nullptr, so we don't need to check aItems
      FloatEbmType * pItem = aItems;
      const FloatEbmType * const pItemEnd = aItems + cItems;
      const double * pOriginal = REAL(items);
      do {
         const double val = *pOriginal;
         *pItem = static_cast<FloatEbmType>(val);
         ++pItem;
         ++pOriginal;
      } while(pItemEnd != pItem);
   }
   *pRet = aItems;
   return false;
}

SEXP GenerateRandomNumber_R(
   SEXP randomSeed,
   SEXP stageRandomizationMix
) {
   EBM_ASSERT(nullptr != randomSeed);
   EBM_ASSERT(nullptr != stageRandomizationMix);

   if(!IsSingleIntVector(randomSeed)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsSingleIntVector(randomSeed)");
      return R_NilValue;
   }
   const SeedEbmType randomSeedLocal = INTEGER(randomSeed)[0];

   if(!IsSingleIntVector(stageRandomizationMix)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsSingleIntVector(stageRandomizationMix)");
      return R_NilValue;
   }
   const SeedEbmType stageRandomizationMixLocal = INTEGER(stageRandomizationMix)[0];

   const SeedEbmType retSeed = GenerateRandomNumber(randomSeedLocal, stageRandomizationMixLocal);

   SEXP ret = PROTECT(allocVector(INTSXP, R_xlen_t { 1 }));
   INTEGER(ret)[0] = retSeed;
   UNPROTECT(1);
   return ret;
}

SEXP GenerateQuantileBinCuts_R(
   SEXP randomSeed,
   SEXP featureValues,
   SEXP countSamplesPerBinMin,
   SEXP isHumanized,
   SEXP countBinCuts
) {
   EBM_ASSERT(nullptr != randomSeed);
   EBM_ASSERT(nullptr != featureValues);
   EBM_ASSERT(nullptr != countSamplesPerBinMin);
   EBM_ASSERT(nullptr != isHumanized);
   EBM_ASSERT(nullptr != countBinCuts);

   if(!IsSingleIntVector(randomSeed)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsSingleIntVector(randomSeed)");
      return R_NilValue;
   }
   const SeedEbmType randomSeedLocal = INTEGER(randomSeed)[0];

   const FloatEbmType * aFeatureValues = nullptr;
   size_t cFeatureValues;
   if(ConvertDoublesToDoubles(featureValues, &cFeatureValues, &aFeatureValues)) {
      // we've already logged any errors
      return R_NilValue;
   }
   EBM_ASSERT(IsNumberConvertable<IntEbmType>(cFeatureValues)); // ConvertDoublesToDoubles checks this

   if(!IsSingleDoubleVector(countSamplesPerBinMin)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsSingleDoubleVector(countSamplesPerBinMin)");
      return R_NilValue;
   }
   const double countSamplesPerBinMinDouble = REAL(countSamplesPerBinMin)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(countSamplesPerBinMinDouble)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsDoubleToIntEbmTypeIndexValid(countSamplesPerBinMinDouble)");
      return R_NilValue;
   }
   const IntEbmType countSamplesPerBinMinIntEbmType = static_cast<IntEbmType>(countSamplesPerBinMinDouble);

   if(!IsSingleBoolVector(isHumanized)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsSingleBoolVector(isHumanized)");
      return R_NilValue;
   }

   const Rboolean isHumanizedR = static_cast<Rboolean>(LOGICAL(isHumanized)[0]);
   if(Rboolean::FALSE != isHumanizedR && Rboolean::TRUE != isHumanizedR) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R Rboolean::FALSE != isHumanizedR && Rboolean::TRUE != isHumanizedR");
      return R_NilValue;
   }
   const bool bHumanized = Rboolean::FALSE != isHumanizedR;

   if(!IsSingleDoubleVector(countBinCuts)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsSingleDoubleVector(countBinCuts)");
      return R_NilValue;
   }
   const double countBinCutsDouble = REAL(countBinCuts)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(countBinCutsDouble)) {
      LOG_0(TraceLevelError, "ERROR GenerateQuantileBinCuts_R !IsDoubleToIntEbmTypeIndexValid(countBinCutsDouble)");
      return R_NilValue;
   }
   IntEbmType countBinCutsIntEbmType = static_cast<IntEbmType>(countBinCutsDouble);
   EBM_ASSERT(IsNumberConvertable<size_t>(countBinCuts)); // IsDoubleToIntEbmTypeIndexValid checks this

   FloatEbmType * const binCutsLowerBoundInclusive = reinterpret_cast<FloatEbmType *>(
      R_alloc(static_cast<size_t>(countBinCutsIntEbmType), static_cast<int>(sizeof(FloatEbmType))));
   // R_alloc doesn't return nullptr, so we don't need to check aItems

   const IntEbmType ret = GenerateQuantileBinCuts(
      randomSeedLocal,
      static_cast<IntEbmType>(cFeatureValues),
      aFeatureValues,
      countSamplesPerBinMinIntEbmType,
      bHumanized ? EBM_TRUE : EBM_FALSE,
      &countBinCutsIntEbmType,
      binCutsLowerBoundInclusive,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr
   );

   if(0 != ret) {
      return R_NilValue;
   } else {
      if(!IsNumberConvertable<R_xlen_t>(countBinCutsIntEbmType)) {
         return R_NilValue;
      }
      SEXP ret = PROTECT(allocVector(REALSXP, static_cast<R_xlen_t>(countBinCutsIntEbmType)));
      // we've allocated this memory, so it should be reachable, so these numbers should multiply
      EBM_ASSERT(!IsMultiplyError(sizeof(*binCutsLowerBoundInclusive), static_cast<size_t>(countBinCutsIntEbmType)));
      memcpy(REAL(ret), binCutsLowerBoundInclusive, sizeof(*binCutsLowerBoundInclusive) * static_cast<size_t>(countBinCutsIntEbmType));
      UNPROTECT(1);
      return ret;
   }
}

SEXP Discretize_R(
   SEXP featureValues,
   SEXP binCutsLowerBoundInclusive,
   SEXP discretizedOut
) {
   EBM_ASSERT(nullptr != featureValues);
   EBM_ASSERT(nullptr != binCutsLowerBoundInclusive);
   EBM_ASSERT(nullptr != discretizedOut);

   const FloatEbmType * aFeatureValues = nullptr;
   size_t cFeatureValues;
   if(ConvertDoublesToDoubles(featureValues, &cFeatureValues, &aFeatureValues)) {
      // we've already logged any errors
      return R_NilValue;
   }
   EBM_ASSERT(IsNumberConvertable<IntEbmType>(cFeatureValues)); // ConvertDoublesToDoubles checks this

   const FloatEbmType * aBinCutsLowerBoundInclusive = nullptr;
   size_t cBinCuts;
   if(ConvertDoublesToDoubles(binCutsLowerBoundInclusive, &cBinCuts, &aBinCutsLowerBoundInclusive)) {
      // we've already logged any errors
      return R_NilValue;
   }
   EBM_ASSERT(IsNumberConvertable<IntEbmType>(cBinCuts)); // ConvertDoublesToDoubles checks this

   if(REALSXP != TYPEOF(discretizedOut)) {
      LOG_0(TraceLevelError, "ERROR Discretize_R REALSXP != TYPEOF(discretizedOut)");
      return R_NilValue;
   }
   const R_xlen_t countDiscretizedOutR = xlength(discretizedOut);
   if(!IsNumberConvertable<size_t>(countDiscretizedOutR)) {
      LOG_0(TraceLevelError, "ERROR Discretize_R !IsNumberConvertable<size_t>(countDiscretizedOutR)");
      return R_NilValue;
   }
   const size_t cDiscretizedOut = static_cast<size_t>(countDiscretizedOutR);
   if(cFeatureValues != cDiscretizedOut) {
      LOG_0(TraceLevelError, "ERROR Discretize_R cFeatureValues != cDiscretizedOut");
      return R_NilValue;
   }

   if(0 != cFeatureValues) {
      IntEbmType * const aDiscretized = 
         reinterpret_cast<IntEbmType *>(R_alloc(cFeatureValues, static_cast<int>(sizeof(IntEbmType))));

      if(0 != Discretize(
         static_cast<IntEbmType>(cFeatureValues),
         aFeatureValues,
         static_cast<IntEbmType>(cBinCuts),
         aBinCutsLowerBoundInclusive,
         aDiscretized
      )) {
         // we've already logged any errors
         return R_NilValue;
      }

      double * pDiscretizedOut = REAL(discretizedOut);
      const IntEbmType * pDiscretized = aDiscretized;
      const IntEbmType * const pDiscretizedEnd = aDiscretized + cFeatureValues;
      do {
         const IntEbmType val = *pDiscretized;
         *pDiscretizedOut = static_cast<double>(val);
         ++pDiscretizedOut;
         ++pDiscretized;
      } while(pDiscretizedEnd != pDiscretized);
   }

   // this return isn't useful beyond that it's not R_NilValue, which would signify error
   SEXP ret = PROTECT(allocVector(REALSXP, R_xlen_t { 1 }));
   REAL(ret)[0] = static_cast<double>(cFeatureValues);
   UNPROTECT(1);
   return ret;
}

SEXP SamplingWithoutReplacement_R(
   SEXP randomSeed,
   SEXP countIncluded,
   SEXP countSamples,
   SEXP isIncludedOut
) {
   EBM_ASSERT(nullptr != randomSeed);
   EBM_ASSERT(nullptr != countIncluded);
   EBM_ASSERT(nullptr != countSamples);
   EBM_ASSERT(nullptr != isIncludedOut);

   if(!IsSingleIntVector(randomSeed)) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R !IsSingleIntVector(randomSeed)");
      return R_NilValue;
   }
   const SeedEbmType randomSeedLocal = INTEGER(randomSeed)[0];

   if(!IsSingleDoubleVector(countIncluded)) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R !IsSingleDoubleVector(countIncluded)");
      return R_NilValue;
   }
   const double countIncludedDouble = REAL(countIncluded)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(countIncludedDouble)) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R !IsDoubleToIntEbmTypeIndexValid(countIncludedDouble)");
      return R_NilValue;
   }
   IntEbmType countIncludedIntEbmType = static_cast<IntEbmType>(countIncludedDouble);
   EBM_ASSERT(IsNumberConvertable<size_t>(countIncludedIntEbmType)); // IsDoubleToIntEbmTypeIndexValid checks this

   if(!IsSingleDoubleVector(countSamples)) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R !IsSingleDoubleVector(countSamples)");
      return R_NilValue;
   }
   const double countSamplesDouble = REAL(countSamples)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(countSamplesDouble)) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R !IsDoubleToIntEbmTypeIndexValid(countSamplesDouble)");
      return R_NilValue;
   }
   IntEbmType countSamplesIntEbmType = static_cast<IntEbmType>(countSamplesDouble);
   EBM_ASSERT(IsNumberConvertable<size_t>(countSamplesIntEbmType)); // IsDoubleToIntEbmTypeIndexValid checks this

   if(LGLSXP != TYPEOF(isIncludedOut)) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R LGLSXP != TYPEOF(isIncludedOut)");
      return R_NilValue;
   }
   const R_xlen_t isIncludedOutR = xlength(isIncludedOut);
   if(!IsNumberConvertable<size_t>(isIncludedOutR)) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R !IsNumberConvertable<size_t>(isIncludedOutR)");
      return R_NilValue;
   }
   const size_t cIsIncludedOut = static_cast<size_t>(isIncludedOutR);
   if(static_cast<size_t>(countSamplesIntEbmType) != cIsIncludedOut) {
      LOG_0(TraceLevelError, "ERROR SamplingWithoutReplacement_R static_cast<size_t>(countSamplesIntEbmType) != cIsIncludedOut");
      return R_NilValue;
   }

   if(0 != cIsIncludedOut) {
      IntEbmType * const aIsIncluded =
         reinterpret_cast<IntEbmType *>(R_alloc(cIsIncludedOut, static_cast<int>(sizeof(IntEbmType))));

      SamplingWithoutReplacement(
         randomSeedLocal,
         countIncludedIntEbmType,
         countSamplesIntEbmType,
         aIsIncluded
      );

      int * pIsIncludedOut = LOGICAL(isIncludedOut);
      const IntEbmType * pIsIncluded = aIsIncluded;
      const IntEbmType * const pIsIncludedEnd = aIsIncluded + cIsIncludedOut;
      do {
         const IntEbmType val = *pIsIncluded;
         *pIsIncludedOut = static_cast<int>(EBM_FALSE != val ? Rboolean::TRUE : Rboolean::FALSE);
         ++pIsIncludedOut;
         ++pIsIncluded;
      } while(pIsIncludedEnd != pIsIncluded);
   }

   // this return isn't useful beyond that it's not R_NilValue, which would signify error
   SEXP ret = PROTECT(allocVector(REALSXP, R_xlen_t { 1 }));
   REAL(ret)[0] = static_cast<double>(cIsIncludedOut);
   UNPROTECT(1);
   return ret;
}

SEXP InitializeBoostingClassification_R(
   SEXP randomSeed,
   SEXP countTargetClasses,
   SEXP features,
   SEXP featureGroups,
   SEXP featureGroupIndexes,
   SEXP trainingBinnedData,
   SEXP trainingTargets,
   SEXP trainingPredictorScores,
   SEXP validationBinnedData,
   SEXP validationTargets,
   SEXP validationPredictorScores,
   SEXP countInnerBags
) {
   EBM_ASSERT(nullptr != randomSeed);
   EBM_ASSERT(nullptr != countTargetClasses);
   EBM_ASSERT(nullptr != features);
   EBM_ASSERT(nullptr != featureGroups);
   EBM_ASSERT(nullptr != featureGroupIndexes);
   EBM_ASSERT(nullptr != trainingBinnedData);
   EBM_ASSERT(nullptr != trainingTargets);
   EBM_ASSERT(nullptr != trainingPredictorScores);
   EBM_ASSERT(nullptr != validationBinnedData);
   EBM_ASSERT(nullptr != validationTargets);
   EBM_ASSERT(nullptr != validationPredictorScores);
   EBM_ASSERT(nullptr != countInnerBags);

   if(!IsSingleIntVector(randomSeed)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R !IsSingleIntVector(randomSeed)");
      return R_NilValue;
   }
   const SeedEbmType randomSeedLocal = INTEGER(randomSeed)[0];

   if(!IsSingleDoubleVector(countTargetClasses)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R !IsSingleDoubleVector(countTargetClasses)");
      return R_NilValue;
   }
   double countTargetClassesDouble = REAL(countTargetClasses)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(countTargetClassesDouble)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R !IsDoubleToIntEbmTypeIndexValid(countTargetClassesDouble)");
      return R_NilValue;
   }
   EBM_ASSERT(IsNumberConvertable<size_t>(countTargetClassesDouble)); // IsDoubleToIntEbmTypeIndexValid checks this
   const size_t cTargetClasses = static_cast<size_t>(countTargetClassesDouble);
   if(!IsNumberConvertable<ptrdiff_t>(cTargetClasses)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R !IsNumberConvertable<ptrdiff_t>(cTargetClasses)");
      return R_NilValue;
   }
   const size_t cVectorLength = GetVectorLength(static_cast<ptrdiff_t>(cTargetClasses));

   size_t cFeatures;
   EbmNativeFeature * const aFeatures = ConvertFeatures(features, &cFeatures);
   if(nullptr == aFeatures) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countFeatures = static_cast<IntEbmType>(cFeatures); // the validity of this conversion was checked in ConvertFeatures(...)

   size_t cFeatureGroups;
   EbmNativeFeatureGroup * const aFeatureGroups = ConvertFeatureGroups(featureGroups, &cFeatureGroups);
   if(nullptr == aFeatureGroups) {
      // we've already logged any errors
      return R_NilValue;
   }
   // the validity of this conversion was checked in ConvertFeatureGroups(...)
   const IntEbmType countFeatureGroups = static_cast<IntEbmType>(cFeatureGroups);

   const size_t cFeatureGroupsIndexesCheck = CountFeatureGroupsIndexes(cFeatureGroups, aFeatureGroups);
   if(SIZE_MAX == cFeatureGroupsIndexesCheck) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cFeatureGroupsIndexesActual;
   const IntEbmType * aFeatureGroupIndexes;
   if(ConvertDoublesToIndexes(featureGroupIndexes, &cFeatureGroupsIndexesActual, &aFeatureGroupIndexes)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(cFeatureGroupsIndexesActual != cFeatureGroupsIndexesCheck) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R cFeatureGroupsIndexesActual != cFeatureGroupsIndexesCheck");
      return R_NilValue;
   }

   size_t cTrainingBinnedData;
   const IntEbmType * aTrainingBinnedData;
   if(ConvertDoublesToIndexes(trainingBinnedData, &cTrainingBinnedData, &aTrainingBinnedData)) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cTrainingSamples;
   const IntEbmType * aTrainingTargets;
   if(ConvertDoublesToIndexes(trainingTargets, &cTrainingSamples, &aTrainingTargets)) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countTrainingSamples = static_cast<IntEbmType>(cTrainingSamples);

   if(IsMultiplyError(cTrainingSamples, cFeatures)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R IsMultiplyError(cTrainingSamples, cFeatures)");
      return R_NilValue;
   }
   if(cTrainingSamples * cFeatures != cTrainingBinnedData) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R cTrainingSamples * cFeatures != cTrainingBinnedData");
      return R_NilValue;
   }

   const FloatEbmType * aTrainingPredictorScores = nullptr;
   size_t cTrainingPredictorScores;
   if(ConvertDoublesToDoubles(trainingPredictorScores, &cTrainingPredictorScores, &aTrainingPredictorScores)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(IsMultiplyError(cTrainingSamples, cVectorLength)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R IsMultiplyError(cTrainingSamples, cVectorLength)");
      return R_NilValue;
   }
   if(cVectorLength * cTrainingSamples != cTrainingPredictorScores) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R cVectorLength * cTrainingSamples != cTrainingPredictorScores");
      return R_NilValue;
   }

   size_t cValidationBinnedData;
   const IntEbmType * aValidationBinnedData;
   if(ConvertDoublesToIndexes(validationBinnedData, &cValidationBinnedData, &aValidationBinnedData)) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cValidationSamples;
   const IntEbmType * aValidationTargets;
   if(ConvertDoublesToIndexes(validationTargets, &cValidationSamples, &aValidationTargets)) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countValidationSamples = static_cast<IntEbmType>(cValidationSamples);

   if(IsMultiplyError(cValidationSamples, cFeatures)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R IsMultiplyError(cValidationSamples, cFeatures)");
      return R_NilValue;
   }
   if(cValidationSamples * cFeatures != cValidationBinnedData) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R cValidationSamples * cFeatures != cValidationBinnedData");
      return R_NilValue;
   }

   const FloatEbmType * aValidationPredictorScores = nullptr;
   size_t cValidationPredictorScores;
   if(ConvertDoublesToDoubles(validationPredictorScores, &cValidationPredictorScores, &aValidationPredictorScores)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(IsMultiplyError(cValidationSamples, cVectorLength)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R IsMultiplyError(cValidationSamples, cVectorLength)");
      return R_NilValue;
   }
   if(cVectorLength * cValidationSamples != cValidationPredictorScores) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R cVectorLength * cValidationSamples != cValidationPredictorScores");
      return R_NilValue;
   }

   if(!IsSingleIntVector(countInnerBags)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R !IsSingleIntVector(countInnerBags)");
      return R_NilValue;
   }
   int countInnerBagsInt = INTEGER(countInnerBags)[0];
   if(!IsNumberConvertable<IntEbmType>(countInnerBagsInt)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingClassification_R !IsNumberConvertable<IntEbmType>(countInnerBagsInt)");
      return nullptr;
   }
   IntEbmType countInnerBagsLocal = static_cast<IntEbmType>(countInnerBagsInt);

   PEbmBoosting pEbmBoosting = InitializeBoostingClassification(
      randomSeedLocal,
      static_cast<IntEbmType>(cTargetClasses),
      countFeatures, 
      aFeatures, 
      countFeatureGroups, 
      aFeatureGroups, 
      aFeatureGroupIndexes, 
      countTrainingSamples, 
      aTrainingBinnedData, 
      aTrainingTargets, 
      aTrainingPredictorScores, 
      countValidationSamples, 
      aValidationBinnedData, 
      aValidationTargets, 
      aValidationPredictorScores, 
      countInnerBagsLocal, 
      nullptr
   );

   if(nullptr == pEbmBoosting) {
      return R_NilValue;
   } else {
      SEXP boostingRPointer = R_MakeExternalPtr(static_cast<void *>(pEbmBoosting), R_NilValue, R_NilValue); // makes an EXTPTRSXP
      PROTECT(boostingRPointer);

      R_RegisterCFinalizerEx(boostingRPointer, &BoostingFinalizer, Rboolean::TRUE);

      UNPROTECT(1);
      return boostingRPointer;
   }
}

SEXP InitializeBoostingRegression_R(
   SEXP randomSeed,
   SEXP features,
   SEXP featureGroups,
   SEXP featureGroupIndexes,
   SEXP trainingBinnedData,
   SEXP trainingTargets,
   SEXP trainingPredictorScores,
   SEXP validationBinnedData,
   SEXP validationTargets,
   SEXP validationPredictorScores,
   SEXP countInnerBags
) {
   EBM_ASSERT(nullptr != randomSeed);
   EBM_ASSERT(nullptr != features);
   EBM_ASSERT(nullptr != featureGroups);
   EBM_ASSERT(nullptr != featureGroupIndexes);
   EBM_ASSERT(nullptr != trainingBinnedData);
   EBM_ASSERT(nullptr != trainingTargets);
   EBM_ASSERT(nullptr != trainingPredictorScores);
   EBM_ASSERT(nullptr != validationBinnedData);
   EBM_ASSERT(nullptr != validationTargets);
   EBM_ASSERT(nullptr != validationPredictorScores);
   EBM_ASSERT(nullptr != countInnerBags);

   if(!IsSingleIntVector(randomSeed)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R !IsSingleIntVector(randomSeed)");
      return R_NilValue;
   }
   const SeedEbmType randomSeedLocal = INTEGER(randomSeed)[0];

   size_t cFeatures;
   EbmNativeFeature * const aFeatures = ConvertFeatures(features, &cFeatures);
   if(nullptr == aFeatures) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countFeatures = static_cast<IntEbmType>(cFeatures); // the validity of this conversion was checked in ConvertFeatures(...)

   size_t cFeatureGroups;
   EbmNativeFeatureGroup * const aFeatureGroups = ConvertFeatureGroups(featureGroups, &cFeatureGroups);
   if(nullptr == aFeatureGroups) {
      // we've already logged any errors
      return R_NilValue;
   }
   // the validity of this conversion was checked in ConvertFeatureGroups(...)
   const IntEbmType countFeatureGroups = static_cast<IntEbmType>(cFeatureGroups);

   const size_t cFeatureGroupsIndexesCheck = CountFeatureGroupsIndexes(cFeatureGroups, aFeatureGroups);
   if(SIZE_MAX == cFeatureGroupsIndexesCheck) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cFeatureGroupsIndexesActual;
   const IntEbmType * aFeatureGroupIndexes;
   if(ConvertDoublesToIndexes(featureGroupIndexes, &cFeatureGroupsIndexesActual, &aFeatureGroupIndexes)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(cFeatureGroupsIndexesActual != cFeatureGroupsIndexesCheck) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R cFeatureGroupsIndexesActual != cFeatureGroupsIndexesCheck");
      return R_NilValue;
   }

   size_t cTrainingBinnedData;
   const IntEbmType * aTrainingBinnedData;
   if(ConvertDoublesToIndexes(trainingBinnedData, &cTrainingBinnedData, &aTrainingBinnedData)) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cTrainingSamples;
   const FloatEbmType * aTrainingTargets;
   if(ConvertDoublesToDoubles(trainingTargets, &cTrainingSamples, &aTrainingTargets)) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countTrainingSamples = static_cast<IntEbmType>(cTrainingSamples);

   if(IsMultiplyError(cTrainingSamples, cFeatures)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R IsMultiplyError(cTrainingSamples, cFeatures)");
      return R_NilValue;
   }
   if(cTrainingSamples * cFeatures != cTrainingBinnedData) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R cTrainingSamples * cFeatures != cTrainingBinnedData");
      return R_NilValue;
   }

   const FloatEbmType * aTrainingPredictorScores = nullptr;
   size_t cTrainingPredictorScores;
   if(ConvertDoublesToDoubles(trainingPredictorScores, &cTrainingPredictorScores, &aTrainingPredictorScores)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(cTrainingSamples != cTrainingPredictorScores) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R cTrainingSamples != cTrainingPredictorScores");
      return R_NilValue;
   }

   size_t cValidationBinnedData;
   const IntEbmType * aValidationBinnedData;
   if(ConvertDoublesToIndexes(validationBinnedData, &cValidationBinnedData, &aValidationBinnedData)) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cValidationSamples;
   const FloatEbmType * aValidationTargets;
   if(ConvertDoublesToDoubles(validationTargets, &cValidationSamples, &aValidationTargets)) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countValidationSamples = static_cast<IntEbmType>(cValidationSamples);

   if(IsMultiplyError(cValidationSamples, cFeatures)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R IsMultiplyError(cValidationSamples, cFeatures)");
      return R_NilValue;
   }
   if(cValidationSamples * cFeatures != cValidationBinnedData) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R cValidationSamples * cFeatures != cValidationBinnedData");
      return R_NilValue;
   }

   const FloatEbmType * aValidationPredictorScores = nullptr;
   size_t cValidationPredictorScores;
   if(ConvertDoublesToDoubles(validationPredictorScores, &cValidationPredictorScores, &aValidationPredictorScores)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(cValidationSamples != cValidationPredictorScores) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R cValidationSamples != cValidationPredictorScores");
      return R_NilValue;
   }

   if(!IsSingleIntVector(countInnerBags)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R !IsSingleIntVector(countInnerBags)");
      return R_NilValue;
   }
   int countInnerBagsInt = INTEGER(countInnerBags)[0];
   if(!IsNumberConvertable<IntEbmType>(countInnerBagsInt)) {
      LOG_0(TraceLevelError, "ERROR InitializeBoostingRegression_R !IsNumberConvertable<IntEbmType>(countInnerBagsInt)");
      return nullptr;
   }
   IntEbmType countInnerBagsLocal = static_cast<IntEbmType>(countInnerBagsInt);

   PEbmBoosting pEbmBoosting = InitializeBoostingRegression(
      randomSeedLocal,
      countFeatures,
      aFeatures, 
      countFeatureGroups, 
      aFeatureGroups, 
      aFeatureGroupIndexes, 
      countTrainingSamples, 
      aTrainingBinnedData, 
      aTrainingTargets, 
      aTrainingPredictorScores, 
      countValidationSamples, 
      aValidationBinnedData, 
      aValidationTargets, 
      aValidationPredictorScores, 
      countInnerBagsLocal, 
      nullptr
   );

   if(nullptr == pEbmBoosting) {
      return R_NilValue;
   } else {
      SEXP boostingRPointer = R_MakeExternalPtr(static_cast<void *>(pEbmBoosting), R_NilValue, R_NilValue); // makes an EXTPTRSXP
      PROTECT(boostingRPointer);

      R_RegisterCFinalizerEx(boostingRPointer, &BoostingFinalizer, Rboolean::TRUE);

      UNPROTECT(1);
      return boostingRPointer;
   }
}

SEXP BoostingStep_R(
   SEXP ebmBoosting,
   SEXP indexFeatureGroup,
   SEXP learningRate,
   SEXP countTreeSplitsMax,
   SEXP countSamplesRequiredForChildSplitMin,
   SEXP trainingWeights,
   SEXP validationWeights
) {
   EBM_ASSERT(nullptr != ebmBoosting);
   EBM_ASSERT(nullptr != indexFeatureGroup);
   EBM_ASSERT(nullptr != learningRate);
   EBM_ASSERT(nullptr != countTreeSplitsMax);
   EBM_ASSERT(nullptr != countSamplesRequiredForChildSplitMin);
   EBM_ASSERT(nullptr != trainingWeights);
   EBM_ASSERT(nullptr != validationWeights);

   if(EXTPTRSXP != TYPEOF(ebmBoosting)) {
      LOG_0(TraceLevelError, "ERROR BoostingStep_R EXTPTRSXP != TYPEOF(ebmBoosting)");
      return R_NilValue;
   }
   EbmBoostingState * pEbmBoosting = static_cast<EbmBoostingState *>(R_ExternalPtrAddr(ebmBoosting));
   if(nullptr == pEbmBoosting) {
      LOG_0(TraceLevelError, "ERROR BoostingStep_R nullptr == pEbmBoosting");
      return R_NilValue;
   }

   if(!IsSingleDoubleVector(indexFeatureGroup)) {
      LOG_0(TraceLevelError, "ERROR BoostingStep_R !IsSingleDoubleVector(indexFeatureGroup)");
      return R_NilValue;
   }
   double doubleIndex = REAL(indexFeatureGroup)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(doubleIndex)) {
      LOG_0(TraceLevelError, "ERROR BoostingStep_R !IsDoubleToIntEbmTypeIndexValid(doubleIndex)");
      return R_NilValue;
   }
   IntEbmType iFeatureGroup = static_cast<IntEbmType>(doubleIndex);

   if(!IsSingleDoubleVector(learningRate)) {
      LOG_0(TraceLevelError, "ERROR BoostingStep_R !IsSingleDoubleVector(learningRate)");
      return R_NilValue;
   }
   double learningRateLocal = REAL(learningRate)[0];

   if(!IsSingleDoubleVector(countTreeSplitsMax)) {
      LOG_0(TraceLevelError, "ERROR BoostingStep_R !IsSingleDoubleVector(countTreeSplitsMax)");
      return R_NilValue;
   }
   double doubleCountTreeSplitsMax = REAL(countTreeSplitsMax)[0];
   IntEbmType cTreeSplitsMax;
   static_assert(std::numeric_limits<double>::is_iec559, "we need is_iec559 to know that comparisons to infinity and -infinity to normal numbers work");
   if(std::isnan(doubleCountTreeSplitsMax) || static_cast<double>(std::numeric_limits<IntEbmType>::max()) < doubleCountTreeSplitsMax) {
      LOG_0(TraceLevelWarning, "WARNING BoostingStep_R countTreeSplitsMax overflow");
      cTreeSplitsMax = std::numeric_limits<IntEbmType>::max();
   } else if(doubleCountTreeSplitsMax < static_cast<double>(std::numeric_limits<IntEbmType>::lowest())) {
      LOG_0(TraceLevelWarning, "WARNING BoostingStep_R countTreeSplitsMax underflow");
      cTreeSplitsMax = std::numeric_limits<IntEbmType>::lowest();
   } else {
      cTreeSplitsMax = static_cast<IntEbmType>(doubleCountTreeSplitsMax);
   }

   if(!IsSingleDoubleVector(countSamplesRequiredForChildSplitMin)) {
      LOG_0(TraceLevelError, "ERROR BoostingStep_R !IsSingleDoubleVector(countSamplesRequiredForChildSplitMin)");
      return R_NilValue;
   }
   double doubleCountSamplesRequiredForChildSplitMin = REAL(countSamplesRequiredForChildSplitMin)[0];
   IntEbmType cSamplesRequiredForChildSplitMin;
   static_assert(std::numeric_limits<double>::is_iec559, "we need is_iec559 to know that comparisons to infinity and -infinity to normal numbers work");
   if(std::isnan(doubleCountSamplesRequiredForChildSplitMin) || 
      static_cast<double>(std::numeric_limits<IntEbmType>::max()) < doubleCountSamplesRequiredForChildSplitMin
   ) {
      LOG_0(TraceLevelWarning, "WARNING BoostingStep_R countSamplesRequiredForChildSplitMin overflow");
      cSamplesRequiredForChildSplitMin = std::numeric_limits<IntEbmType>::max();
   } else if(doubleCountSamplesRequiredForChildSplitMin < static_cast<double>(std::numeric_limits<IntEbmType>::lowest())) {
      LOG_0(TraceLevelWarning, "WARNING BoostingStep_R countSamplesRequiredForChildSplitMin underflow");
      cSamplesRequiredForChildSplitMin = std::numeric_limits<IntEbmType>::lowest();
   } else {
      cSamplesRequiredForChildSplitMin = static_cast<IntEbmType>(doubleCountSamplesRequiredForChildSplitMin);
   }

   double * pTrainingWeights = nullptr;
   double * pValidationWeights = nullptr;
   if(NILSXP != TYPEOF(trainingWeights) || NILSXP != TYPEOF(validationWeights)) {
      if(REALSXP != TYPEOF(trainingWeights)) {
         LOG_0(TraceLevelError, "ERROR BoostingStep_R REALSXP != TYPEOF(trainingWeights)");
         return R_NilValue;
      }
      R_xlen_t trainingWeightsLength = xlength(trainingWeights);
      if(!IsNumberConvertable<size_t>(trainingWeightsLength)) {
         LOG_0(TraceLevelError, "ERROR BoostingStep_R !IsNumberConvertable<size_t>(trainingWeightsLength)");
         return R_NilValue;
      }
      size_t cTrainingWeights = static_cast<size_t>(trainingWeightsLength);
      if(cTrainingWeights != pEbmBoosting->GetTrainingSet()->GetCountSamples()) {
         LOG_0(TraceLevelError, "ERROR BoostingStep_R cTrainingWeights != pEbmBoosting->GetTrainingSet()->GetCountSamples()");
         return R_NilValue;
      }
      pTrainingWeights = REAL(trainingWeights);

      if(REALSXP != TYPEOF(validationWeights)) {
         LOG_0(TraceLevelError, "ERROR BoostingStep_R REALSXP != TYPEOF(validationWeights)");
         return R_NilValue;
      }
      R_xlen_t validationWeightsLength = xlength(validationWeights);
      if(!IsNumberConvertable<size_t>(validationWeightsLength)) {
         LOG_0(TraceLevelError, "ERROR BoostingStep_R !IsNumberConvertable<size_t>(validationWeightsLength)");
         return R_NilValue;
      }
      size_t cValidationWeights = static_cast<size_t>(validationWeightsLength);
      if(cValidationWeights != pEbmBoosting->GetValidationSet()->GetCountSamples()) {
         LOG_0(TraceLevelError, "ERROR BoostingStep_R cValidationWeights != pEbmBoosting->GetValidationSet()->GetCountSamples()");
         return R_NilValue;
      }
      pValidationWeights = REAL(validationWeights);
   }

   FloatEbmType validationMetricOut;
   if(0 != BoostingStep(
      reinterpret_cast<PEbmBoosting>(pEbmBoosting), 
      iFeatureGroup, 
      learningRateLocal, 
      cTreeSplitsMax, 
      cSamplesRequiredForChildSplitMin, 
      pTrainingWeights, 
      pValidationWeights, 
      &validationMetricOut
   )) {
      LOG_0(TraceLevelWarning, "WARNING BoostingStep_R BoostingStep returned error code");
      return R_NilValue;
   }

   SEXP ret = PROTECT(allocVector(REALSXP, R_xlen_t { 1 }));
   REAL(ret)[0] = validationMetricOut;
   UNPROTECT(1);
   return ret;
}

SEXP GetBestModelFeatureGroup_R(
   SEXP ebmBoosting,
   SEXP indexFeatureGroup
) {
   EBM_ASSERT(nullptr != ebmBoosting); // shouldn't be possible
   EBM_ASSERT(nullptr != indexFeatureGroup); // shouldn't be possible

   if(EXTPTRSXP != TYPEOF(ebmBoosting)) {
      LOG_0(TraceLevelError, "ERROR GetBestModelFeatureGroup_R EXTPTRSXP != TYPEOF(ebmBoosting)");
      return R_NilValue;
   }
   EbmBoostingState * pEbmBoosting = static_cast<EbmBoostingState *>(R_ExternalPtrAddr(ebmBoosting));
   if(nullptr == pEbmBoosting) {
      LOG_0(TraceLevelError, "ERROR GetBestModelFeatureGroup_R nullptr == pEbmBoosting");
      return R_NilValue;
   }

   if(!IsSingleDoubleVector(indexFeatureGroup)) {
      LOG_0(TraceLevelError, "ERROR GetBestModelFeatureGroup_R !IsSingleDoubleVector(indexFeatureGroup)");
      return R_NilValue;
   }
   double doubleIndex = REAL(indexFeatureGroup)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(doubleIndex)) {
      LOG_0(TraceLevelError, "ERROR GetBestModelFeatureGroup_R !IsDoubleToIntEbmTypeIndexValid(doubleIndex)");
      return R_NilValue;
   }
   IntEbmType iFeatureGroup = static_cast<IntEbmType>(doubleIndex);
   // we check that iFeatureGroup can be converted to size_t in IsDoubleToIntEbmTypeIndexValid
   if(pEbmBoosting->GetCountFeatureGroups() <= static_cast<size_t>(iFeatureGroup)) {
      LOG_0(TraceLevelError, "ERROR GetBestModelFeatureGroup_R pEbmBoosting->GetCountFeatureGroups() <= static_cast<size_t>(iFeatureGroup)");
      return R_NilValue;
   }

   FloatEbmType * pModelFeatureGroupTensor = GetBestModelFeatureGroup(reinterpret_cast<PEbmBoosting>(pEbmBoosting), iFeatureGroup);
   if(nullptr == pModelFeatureGroupTensor) {
      LOG_0(TraceLevelWarning, "WARNING GetBestModelFeatureGroup_R nullptr == pModelFeatureGroupTensor");

      // if nullptr == pModelFeatureGroupTensor then either:
      //    1) m_cFeatureGroups was 0, in which case this function would have undefined behavior since the caller needs to indicate a valid 
      //       indexFeatureGroup, which is impossible, so we can do anything we like, include the below actions.
      //    2) m_runtimeLearningTypeOrCountTargetClasses was either 1 or 0 (and the learning type is classification), 
      //       which is legal, which we need to handle here
      SEXP ret = allocVector(REALSXP, R_xlen_t { 0 });
      return ret;
   }
   size_t cValues = GetVectorLength(pEbmBoosting->GetRuntimeLearningTypeOrCountTargetClasses());
   const FeatureGroup * const pFeatureGroup = pEbmBoosting->GetFeatureGroups()[static_cast<size_t>(iFeatureGroup)];
   const size_t cFeatures = pFeatureGroup->GetCountFeatures();
   if(0 != cFeatures) {
      const FeatureGroupEntry * pFeatureGroupEntry = pFeatureGroup->GetFeatureGroupEntries();
      const FeatureGroupEntry * const pFeatureGroupEntryEnd = &pFeatureGroupEntry[cFeatures];
      do {
         const size_t cBins = pFeatureGroupEntry->m_pFeature->GetCountBins();
         EBM_ASSERT(!IsMultiplyError(cBins, cValues)); // we've allocated this memory, so it should be reachable, so these numbers should multiply
         cValues *= cBins;
         ++pFeatureGroupEntry;
      } while(pFeatureGroupEntryEnd != pFeatureGroupEntry);
   }
   if(!IsNumberConvertable<R_xlen_t>(cValues)) {
      return R_NilValue;
   }
   SEXP ret = PROTECT(allocVector(REALSXP, static_cast<R_xlen_t>(cValues)));
   EBM_ASSERT(!IsMultiplyError(sizeof(double), cValues)); // we've allocated this memory, so it should be reachable, so these numbers should multiply
   memcpy(REAL(ret), pModelFeatureGroupTensor, sizeof(double) * cValues);
   UNPROTECT(1);
   return ret;
}

SEXP GetCurrentModelFeatureGroup_R(
   SEXP ebmBoosting,
   SEXP indexFeatureGroup
) {
   EBM_ASSERT(nullptr != ebmBoosting); // shouldn't be possible
   EBM_ASSERT(nullptr != indexFeatureGroup); // shouldn't be possible

   if(EXTPTRSXP != TYPEOF(ebmBoosting)) {
      LOG_0(TraceLevelError, "ERROR GetCurrentModelFeatureGroup_R EXTPTRSXP != TYPEOF(ebmBoosting)");
      return R_NilValue;
   }
   EbmBoostingState * pEbmBoosting = static_cast<EbmBoostingState *>(R_ExternalPtrAddr(ebmBoosting));
   if(nullptr == pEbmBoosting) {
      LOG_0(TraceLevelError, "ERROR GetCurrentModelFeatureGroup_R nullptr == pEbmBoosting");
      return R_NilValue;
   }

   if(!IsSingleDoubleVector(indexFeatureGroup)) {
      LOG_0(TraceLevelError, "ERROR GetCurrentModelFeatureGroup_R !IsSingleDoubleVector(indexFeatureGroup)");
      return R_NilValue;
   }
   double doubleIndex = REAL(indexFeatureGroup)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(doubleIndex)) {
      LOG_0(TraceLevelError, "ERROR GetCurrentModelFeatureGroup_R !IsDoubleToIntEbmTypeIndexValid(doubleIndex)");
      return R_NilValue;
   }
   IntEbmType iFeatureGroup = static_cast<IntEbmType>(doubleIndex);
   // we check that iFeatureGroup can be converted to size_t in IsDoubleToIntEbmTypeIndexValid
   if(pEbmBoosting->GetCountFeatureGroups() <= static_cast<size_t>(iFeatureGroup)) {
      LOG_0(TraceLevelError, "ERROR GetCurrentModelFeatureGroup_R pEbmBoosting->GetCountFeatureGroups() <= static_cast<size_t>(iFeatureGroup)");
      return R_NilValue;
   }

   FloatEbmType * pModelFeatureGroupTensor = GetCurrentModelFeatureGroup(reinterpret_cast<PEbmBoosting>(pEbmBoosting), iFeatureGroup);
   if(nullptr == pModelFeatureGroupTensor) {
      LOG_0(TraceLevelWarning, "WARNING GetCurrentModelFeatureGroup_R nullptr == pModelFeatureGroupTensor");

      // if nullptr == pModelFeatureGroupTensor then either:
      //    1) m_cFeatureGroups was 0, in which case this function would have undefined behavior since the caller needs to indicate a valid 
      //       indexFeatureGroup, which is impossible, so we can do anything we like, include the below actions.
      //    2) m_runtimeLearningTypeOrCountTargetClasses was either 1 or 0 (and the learning type is classification), which is legal, 
      //       which we need to handle here
      SEXP ret = allocVector(REALSXP, R_xlen_t { 0 });
      return ret;
   }
   size_t cValues = GetVectorLength(pEbmBoosting->GetRuntimeLearningTypeOrCountTargetClasses());
   const FeatureGroup * const pFeatureGroup = pEbmBoosting->GetFeatureGroups()[static_cast<size_t>(iFeatureGroup)];
   const size_t cFeatures = pFeatureGroup->GetCountFeatures();
   if(0 != cFeatures) {
      const FeatureGroupEntry * pFeatureGroupEntry = pFeatureGroup->GetFeatureGroupEntries();
      const FeatureGroupEntry * const pFeatureGroupEntryEnd = &pFeatureGroupEntry[cFeatures];
      do {
         const size_t cBins = pFeatureGroupEntry->m_pFeature->GetCountBins();
         EBM_ASSERT(!IsMultiplyError(cBins, cValues)); // we've allocated this memory, so it should be reachable, so these numbers should multiply
         cValues *= cBins;
         ++pFeatureGroupEntry;
      } while(pFeatureGroupEntryEnd != pFeatureGroupEntry);
   }
   if(!IsNumberConvertable<R_xlen_t>(cValues)) {
      return R_NilValue;
   }
   SEXP ret = PROTECT(allocVector(REALSXP, static_cast<R_xlen_t>(cValues)));
   EBM_ASSERT(!IsMultiplyError(sizeof(double), cValues)); // we've allocated this memory, so it should be reachable, so these numbers should multiply
   memcpy(REAL(ret), pModelFeatureGroupTensor, sizeof(double) * cValues);
   UNPROTECT(1);
   return ret;
}

SEXP FreeBoosting_R(
   SEXP ebmBoosting
) {
   BoostingFinalizer(ebmBoosting);
   return R_NilValue;
}


SEXP InitializeInteractionClassification_R(
   SEXP countTargetClasses,
   SEXP features,
   SEXP binnedData,
   SEXP targets,
   SEXP predictorScores
) {
   EBM_ASSERT(nullptr != countTargetClasses);
   EBM_ASSERT(nullptr != features);
   EBM_ASSERT(nullptr != binnedData);
   EBM_ASSERT(nullptr != targets);
   EBM_ASSERT(nullptr != predictorScores);

   if(!IsSingleDoubleVector(countTargetClasses)) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification_R !IsSingleDoubleVector(countTargetClasses)");
      return R_NilValue;
   }
   double countTargetClassesDouble = REAL(countTargetClasses)[0];
   if(!IsDoubleToIntEbmTypeIndexValid(countTargetClassesDouble)) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification_R !IsDoubleToIntEbmTypeIndexValid(countTargetClassesDouble)");
      return R_NilValue;
   }
   const size_t cTargetClasses = static_cast<size_t>(countTargetClassesDouble);
   if(!IsNumberConvertable<ptrdiff_t>(cTargetClasses)) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification_R !IsNumberConvertable<ptrdiff_t>(cTargetClasses)");
      return R_NilValue;
   }
   const size_t cVectorLength = GetVectorLength(static_cast<ptrdiff_t>(cTargetClasses));

   size_t cFeatures;
   EbmNativeFeature * const aFeatures = ConvertFeatures(features, &cFeatures);
   if(nullptr == aFeatures) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countFeatures = static_cast<IntEbmType>(cFeatures); // the validity of this conversion was checked in ConvertFeatures(...)

   size_t cBinnedData;
   const IntEbmType * aBinnedData;
   if(ConvertDoublesToIndexes(binnedData, &cBinnedData, &aBinnedData)) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cSamples;
   const IntEbmType * aTargets;
   if(ConvertDoublesToIndexes(targets, &cSamples, &aTargets)) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countSamples = static_cast<IntEbmType>(cSamples);

   if(IsMultiplyError(cSamples, cFeatures)) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification_R IsMultiplyError(cSamples, cFeatures)");
      return R_NilValue;
   }
   if(cSamples * cFeatures != cBinnedData) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification_R cSamples * cFeatures != cBinnedData");
      return R_NilValue;
   }

   const FloatEbmType * aPredictorScores = nullptr;
   size_t cPredictorScores;
   if(ConvertDoublesToDoubles(predictorScores, &cPredictorScores, &aPredictorScores)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(IsMultiplyError(cSamples, cVectorLength)) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification_R IsMultiplyError(cSamples, cVectorLength)");
      return R_NilValue;
   }
   if(cVectorLength * cSamples != cPredictorScores) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionClassification_R cVectorLength * cSamples != cPredictorScores");
      return R_NilValue;
   }

   PEbmInteraction pEbmInteraction = InitializeInteractionClassification(
      static_cast<IntEbmType>(cTargetClasses), 
      countFeatures, 
      aFeatures, 
      countSamples, 
      aBinnedData, 
      aTargets, 
      aPredictorScores,
      nullptr
   );

   if(nullptr == pEbmInteraction) {
      return R_NilValue;
   } else {
      SEXP interactionRPointer = R_MakeExternalPtr(static_cast<void *>(pEbmInteraction), R_NilValue, R_NilValue); // makes an EXTPTRSXP
      PROTECT(interactionRPointer);

      R_RegisterCFinalizerEx(interactionRPointer, &InteractionFinalizer, Rboolean::TRUE);

      UNPROTECT(1);
      return interactionRPointer;
   }
}

SEXP InitializeInteractionRegression_R(
   SEXP features,
   SEXP binnedData,
   SEXP targets,
   SEXP predictorScores
) {
   EBM_ASSERT(nullptr != features);
   EBM_ASSERT(nullptr != binnedData);
   EBM_ASSERT(nullptr != targets);
   EBM_ASSERT(nullptr != predictorScores);

   size_t cFeatures;
   EbmNativeFeature * const aFeatures = ConvertFeatures(features, &cFeatures);
   if(nullptr == aFeatures) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countFeatures = static_cast<IntEbmType>(cFeatures); // the validity of this conversion was checked in ConvertFeatures(...)

   size_t cBinnedData;
   const IntEbmType * aBinnedData;
   if(ConvertDoublesToIndexes(binnedData, &cBinnedData, &aBinnedData)) {
      // we've already logged any errors
      return R_NilValue;
   }

   size_t cSamples;
   const FloatEbmType * aTargets;
   if(ConvertDoublesToDoubles(targets, &cSamples, &aTargets)) {
      // we've already logged any errors
      return R_NilValue;
   }
   const IntEbmType countSamples = static_cast<IntEbmType>(cSamples);

   if(IsMultiplyError(cSamples, cFeatures)) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionRegression_R IsMultiplyError(cSamples, cFeatures)");
      return R_NilValue;
   }
   if(cSamples * cFeatures != cBinnedData) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionRegression_R cSamples * cFeatures != cBinnedData");
      return R_NilValue;
   }

   const FloatEbmType * aPredictorScores = nullptr;
   size_t cPredictorScores;
   if(ConvertDoublesToDoubles(predictorScores, &cPredictorScores, &aPredictorScores)) {
      // we've already logged any errors
      return R_NilValue;
   }
   if(cSamples != cPredictorScores) {
      LOG_0(TraceLevelError, "ERROR InitializeInteractionRegression_R cSamples != cPredictorScores");
      return R_NilValue;
   }

   PEbmInteraction pEbmInteraction = InitializeInteractionRegression(
      countFeatures, 
      aFeatures, 
      countSamples, 
      aBinnedData, 
      aTargets, 
      aPredictorScores,
      nullptr
   );

   if(nullptr == pEbmInteraction) {
      return R_NilValue;
   } else {
      SEXP interactionRPointer = R_MakeExternalPtr(static_cast<void *>(pEbmInteraction), R_NilValue, R_NilValue); // makes an EXTPTRSXP
      PROTECT(interactionRPointer);

      R_RegisterCFinalizerEx(interactionRPointer, &InteractionFinalizer, Rboolean::TRUE);

      UNPROTECT(1);
      return interactionRPointer;
   }
}

SEXP CalculateInteractionScore_R(
   SEXP ebmInteraction,
   SEXP featureIndexes,
   SEXP countSamplesRequiredForChildSplitMin
) {
   EBM_ASSERT(nullptr != ebmInteraction); // shouldn't be possible
   EBM_ASSERT(nullptr != featureIndexes); // shouldn't be possible
   EBM_ASSERT(nullptr != countSamplesRequiredForChildSplitMin);

   if(EXTPTRSXP != TYPEOF(ebmInteraction)) {
      LOG_0(TraceLevelError, "ERROR CalculateInteractionScore_R EXTPTRSXP != TYPEOF(ebmInteraction)");
      return R_NilValue;
   }
   EbmInteractionState * pEbmInteraction = static_cast<EbmInteractionState *>(R_ExternalPtrAddr(ebmInteraction));
   if(nullptr == pEbmInteraction) {
      LOG_0(TraceLevelError, "ERROR CalculateInteractionScore_R nullptr == pEbmInteraction");
      return R_NilValue;
   }

   size_t cFeaturesInGroup;
   const IntEbmType * aFeatureIndexes;
   if(ConvertDoublesToIndexes(featureIndexes, &cFeaturesInGroup, &aFeatureIndexes)) {
      // we've already logged any errors
      return R_NilValue;
   }
   IntEbmType countFeaturesInGroup = static_cast<IntEbmType>(cFeaturesInGroup);

   if(!IsSingleDoubleVector(countSamplesRequiredForChildSplitMin)) {
      LOG_0(TraceLevelError, "ERROR CalculateInteractionScore_R !IsSingleDoubleVector(countSamplesRequiredForChildSplitMin)");
      return R_NilValue;
   }
   double doubleCountSamplesRequiredForChildSplitMin = REAL(countSamplesRequiredForChildSplitMin)[0];
   IntEbmType cSamplesRequiredForChildSplitMin;
   static_assert(std::numeric_limits<double>::is_iec559, "we need is_iec559 to know that comparisons to infinity and -infinity to normal numbers work");
   if(std::isnan(doubleCountSamplesRequiredForChildSplitMin) ||
      static_cast<double>(std::numeric_limits<IntEbmType>::max()) < doubleCountSamplesRequiredForChildSplitMin
      ) {
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScore_R countSamplesRequiredForChildSplitMin overflow");
      cSamplesRequiredForChildSplitMin = std::numeric_limits<IntEbmType>::max();
   } else if(doubleCountSamplesRequiredForChildSplitMin < static_cast<double>(std::numeric_limits<IntEbmType>::lowest())) {
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScore_R countSamplesRequiredForChildSplitMin underflow");
      cSamplesRequiredForChildSplitMin = std::numeric_limits<IntEbmType>::lowest();
   } else {
      cSamplesRequiredForChildSplitMin = static_cast<IntEbmType>(doubleCountSamplesRequiredForChildSplitMin);
   }

   FloatEbmType interactionScoreOut;
   if(0 != CalculateInteractionScore(reinterpret_cast<PEbmInteraction>(pEbmInteraction), countFeaturesInGroup, aFeatureIndexes, cSamplesRequiredForChildSplitMin, &interactionScoreOut)) {
      LOG_0(TraceLevelWarning, "WARNING CalculateInteractionScore_R CalculateInteractionScore returned error code");
      return R_NilValue;
   }

   SEXP ret = PROTECT(allocVector(REALSXP, R_xlen_t { 1 }));
   REAL(ret)[0] = interactionScoreOut;
   UNPROTECT(1);
   return ret;
}

SEXP FreeInteraction_R(
   SEXP ebmInteraction
) {
   InteractionFinalizer(ebmInteraction);
   return R_NilValue;
}

static const R_CallMethodDef g_exposedFunctions[] = {
   { "GenerateRandomNumber_R", (DL_FUNC)&GenerateRandomNumber_R, 2 },
   { "GenerateQuantileBinCuts_R", (DL_FUNC)&GenerateQuantileBinCuts_R, 5 },
   { "Discretize_R", (DL_FUNC)&Discretize_R, 3 },
   { "SamplingWithoutReplacement_R", (DL_FUNC)&SamplingWithoutReplacement_R, 4 },
   { "InitializeBoostingClassification_R", (DL_FUNC)&InitializeBoostingClassification_R, 12 },
   { "InitializeBoostingRegression_R", (DL_FUNC)& InitializeBoostingRegression_R, 11 },
   { "BoostingStep_R", (DL_FUNC)& BoostingStep_R, 7 },
   { "GetBestModelFeatureGroup_R", (DL_FUNC)&GetBestModelFeatureGroup_R, 2 },
   { "GetCurrentModelFeatureGroup_R", (DL_FUNC)& GetCurrentModelFeatureGroup_R, 2 },
   { "FreeBoosting_R", (DL_FUNC)& FreeBoosting_R, 1 },
   { "InitializeInteractionClassification_R", (DL_FUNC)&InitializeInteractionClassification_R, 5 },
   { "InitializeInteractionRegression_R", (DL_FUNC)& InitializeInteractionRegression_R, 4 },
   { "CalculateInteractionScore_R", (DL_FUNC)& CalculateInteractionScore_R, 3 },
   { "FreeInteraction_R", (DL_FUNC)& FreeInteraction_R, 1 },
   { NULL, NULL, 0 }
};

extern "C" {
   void attribute_visible R_init_interpret(DllInfo * info) {
      R_registerRoutines(info, NULL, g_exposedFunctions, NULL, NULL);
      R_useDynamicSymbols(info, FALSE);
      R_forceSymbols(info, TRUE);
   }
} // extern "C"
