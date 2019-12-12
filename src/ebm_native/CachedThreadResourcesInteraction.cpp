// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "PrecompiledHeader.h"

#include <stdlib.h> // free
#include <stddef.h> // size_t, ptrdiff_t

#include "EbmInternal.h" // INLINE_ALWAYS
#include "Logging.h" // EBM_ASSERT & LOG

#include "CachedThreadResourcesInteraction.h"

void CachedInteractionThreadResources::Free(CachedInteractionThreadResources * const pCachedResources) {
   LOG_0(TraceLevelInfo, "Entered CachedInteractionThreadResources::Free");

   free(pCachedResources->m_aThreadByteBuffer1);

   free(pCachedResources);

   LOG_0(TraceLevelInfo, "Exited CachedInteractionThreadResources::Free");
}

CachedInteractionThreadResources * CachedInteractionThreadResources::Allocate() {
   LOG_0(TraceLevelInfo, "Entered CachedInteractionThreadResources::Allocate");

   CachedInteractionThreadResources * const pNew = EbmMalloc<CachedInteractionThreadResources>();
   if(nullptr != pNew) {
      pNew->InitializeZero();
   }

   LOG_0(TraceLevelInfo, "Exited CachedInteractionThreadResources::Allocate");

   return pNew;
}

HistogramBucketBase * CachedInteractionThreadResources::GetThreadByteBuffer1(const size_t cBytesRequired) {
   HistogramBucketBase * aBuffer = m_aThreadByteBuffer1;
   if(UNLIKELY(m_cThreadByteBufferCapacity1 < cBytesRequired)) {
      m_cThreadByteBufferCapacity1 = cBytesRequired << 1;
      LOG_N(TraceLevelInfo, "Growing CachedInteractionThreadResources::ThreadByteBuffer1 to %zu", m_cThreadByteBufferCapacity1);

      free(aBuffer);
      aBuffer = static_cast<HistogramBucketBase *>(EbmMalloc<void>(m_cThreadByteBufferCapacity1));
      m_aThreadByteBuffer1 = aBuffer;
   }
   return aBuffer;
}
