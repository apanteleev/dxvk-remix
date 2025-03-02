/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx_texturemanager.h"
#include "../../util/thread.h"
#include "dxvk_context.h"
#include "dxvk_device.h"
#include <chrono>

#include "rtx_texture.h"
#include "rtx_io.h"

namespace dxvk {
  RtxTextureManager::RtxTextureManager(const Rc<DxvkDevice>& device)
  : m_device(device),
    m_ctx(m_device->createContext()) {
  }

  RtxTextureManager::~RtxTextureManager() {
    {
      std::unique_lock<dxvk::mutex> lock(m_queueMutex);
      m_stopped.store(true);
    }

    m_thread.join();
  }

  void RtxTextureManager::start() {
    m_thread = dxvk::thread([this]() { threadFunc(); });
  }

  void RtxTextureManager::scheduleTextureUpload(TextureRef& texture, Rc<DxvkContext>& immediateContext, bool allowAsync) {
    const Rc<ManagedTexture>& managedTexture = texture.getManagedTexture();
    if (managedTexture.ptr() == nullptr)
      return;

    switch (managedTexture->state) {
    case ManagedTexture::State::kVidMem:
      if (texture.finalizePendingPromotion()) {
        // Texture reached its final destination
        return;
      }
      break;
    case ManagedTexture::State::kQueuedForUpload:
#ifdef WITH_RTXIO
      if (RtxIo::enabled()) {
        if (RtxIo::get().isComplete(managedTexture->completionSyncpt)) {
          managedTexture->state = ManagedTexture::State::kVidMem;
          texture.finalizePendingPromotion();
        }
      }
#endif
      return;
    case ManagedTexture::State::kFailed:
    case ManagedTexture::State::kHostMem:
      // We need to schedule an upload
      break;
    }

    int preloadMips = allowAsync ? calcPreloadMips(managedTexture->futureImageDesc.mipLevels) : managedTexture->futureImageDesc.mipLevels;

    if (RtxIo::enabled()) {
      // When we get here with a texture in VID mem, the texture is considered already preloaded with RTXIO
      preloadMips = managedTexture->state != ManagedTexture::State::kVidMem ? preloadMips : 0;
    }

    if (preloadMips) {
      try {
        assert(managedTexture->linearImageDataSmallMips);

        int largestMipToPreload = managedTexture->futureImageDesc.mipLevels - uint32_t(preloadMips);
        if (largestMipToPreload < managedTexture->numLargeMips && managedTexture->linearImageDataLargeMips == nullptr) {
          TextureUtils::loadTexture(managedTexture, m_device, m_ctx, TextureUtils::MemoryAperture::HOST, TextureUtils::MipsToLoad::LowMips);
        }
        
        TextureUtils::promoteHostToVid(m_device, immediateContext, managedTexture, largestMipToPreload);
      }
      catch(const DxvkError& e) {
        managedTexture->state = ManagedTexture::State::kFailed;
        Logger::err("Failed to create image for VidMem promotion!");
        Logger::err(e.message());
        return;
      }
    }
    
    const bool asyncUpload = (preloadMips < managedTexture->futureImageDesc.mipLevels);
    if (asyncUpload) {
      { std::unique_lock<dxvk::mutex> lock(m_queueMutex);
        m_textureQueue.push(managedTexture);
        ++m_texturesPending;
        managedTexture->state = ManagedTexture::State::kQueuedForUpload;
        managedTexture->frameQueuedForUpload = m_device->getCurrentFrameId();
      }
      
      m_condOnAdd.notify_one();
    } else {
      // if we're not queueing for upload, make sure we don't hang on to low mip data
      if (managedTexture->linearImageDataLargeMips) {
        managedTexture->linearImageDataLargeMips.reset();
      }
    }
  }

  void RtxTextureManager::unloadTexture(const Rc<ManagedTexture>& texture) {
    texture->demote();
  }

  void RtxTextureManager::synchronize(bool dropRequests) {
    ZoneScoped;

    std::unique_lock<dxvk::mutex> lock(m_queueMutex);
    
    m_dropRequests = dropRequests;

    m_condOnSync.wait(lock, [this] {
      return !m_texturesPending.load();
    });

    m_dropRequests = false;
  }

  void RtxTextureManager::kickoff() {
    if (m_texturesPending == 0) {
      m_kickoff = true;
      m_condOnAdd.notify_one();
    }
  }

  int RtxTextureManager::calcPreloadMips(int mipLevels)
  {
    if (RtxOptions::Get()->enableAsyncTextureUpload()) {
      return clamp(RtxOptions::Get()->asyncTextureUploadPreloadMips(), 0, mipLevels);
    } else {
      return mipLevels;
    }
  }

  void RtxTextureManager::threadFunc() {
    ZoneScoped;

    env::setThreadName("rtx-texture-manager");

    Rc<ManagedTexture> texture;

    m_ctx->beginRecording(m_device->createCommandList());

    try {
      while (!m_stopped.load()) {
        { std::unique_lock<dxvk::mutex> lock(m_queueMutex);
          if (texture.ptr()) {
            if (--m_texturesPending == 0)
              m_condOnSync.notify_one();

            texture = nullptr;
          }

          if (m_textureQueue.empty()) {
            m_condOnAdd.wait(lock, [this] {
              return !m_textureQueue.empty() || m_stopped.load() || m_kickoff;
            });
          }

          if (m_stopped.load())
            break;

          if (!m_textureQueue.empty()) {
            texture = std::move(m_textureQueue.front());
            m_textureQueue.pop();
          }
        }

#ifdef WITH_RTXIO
        if (m_kickoff || m_dropRequests) {
          if (RtxIo::enabled()) {
            RtxIo::get().flush(!m_dropRequests);
          }
          m_kickoff = false;
        }
#endif

        if (texture.ptr()) {
          const bool alwaysWait = RtxOptions::Get()->alwaysWaitForAsyncTextures();

          // Wait until the next frame since the texture's been queued for upload, to relieve some pressure from frames
          // where many new textures are created by the game. In that case, texture uploads slow down the main and CS threads,
          // thus making the frame longer.
          // Note: RTX IO will manage dispatches on its own and does not need to be cooled down.
          if (!RtxIo::enabled()) {
            while (!m_dropRequests && !m_stopped && !alwaysWait && texture->frameQueuedForUpload >= m_device->getCurrentFrameId())
              Sleep(1);
          }

          if (m_dropRequests) {
            texture->state = ManagedTexture::State::kFailed;
            texture->demote();
          } else
            uploadTexture(texture);
        }
      }
    }
    catch (const DxvkError& e) {
      Logger::err("Exception on TextureManager thread!");
      Logger::err(e.message());
    }
  }

  void RtxTextureManager::uploadTexture(const Rc<ManagedTexture>& texture)
  {
    ZoneScoped;

    if (texture->state != ManagedTexture::State::kQueuedForUpload)
      return;

    try {
      if (!RtxIo::enabled()) {
        assert(texture->numLargeMips > 0);
        assert(!texture->linearImageDataLargeMips);
      }

      TextureUtils::loadTexture(texture, m_device, m_ctx, TextureUtils::MemoryAperture::HOST, TextureUtils::MipsToLoad::LowMips);

      if (!RtxIo::enabled()) {
        TextureUtils::promoteHostToVid(m_device, m_ctx, texture);
        m_ctx->flushCommandList();
        texture->linearImageDataLargeMips.reset();
      }
    }
    catch (const DxvkError& e) {
      texture->state = ManagedTexture::State::kFailed;
      Logger::err("Failed to finish texture promotion to VidMem!");
      Logger::err(e.message());
    }
  }

  Rc<ManagedTexture> RtxTextureManager::preloadTexture(const Rc<AssetData>& assetData,
    ColorSpace colorSpace, const Rc<DxvkContext>& context, bool forceLoad) {

    const XXH64_hash_t hash = assetData->hash();

    auto it = m_textures.find(hash);
    if (it != m_textures.end()) {
      return it->second;
    }

    auto texture = TextureUtils::createTexture(assetData, colorSpace);

    TextureUtils::loadTexture(texture,
      m_device,
      context,
      TextureUtils::MemoryAperture::HOST,
      forceLoad ? TextureUtils::MipsToLoad::All : TextureUtils::MipsToLoad::HighMips,
      m_minimumMipLevel);

    // The content suggested we keep this texture always loaded, never demote.
    texture->canDemote = !forceLoad;

    return m_textures.emplace(hash, texture).first->second;
  }

  void RtxTextureManager::releaseTexture(const Rc<ManagedTexture>& texture) {
    if (texture != nullptr) {
      unloadTexture(texture);

      m_textures.erase(texture->assetData->hash());
    }
  }

  void RtxTextureManager::demoteTexturesFromVidmem() {
    for (const auto& pair : m_textures) {
      unloadTexture(pair.second);
    }
  }

  uint32_t RtxTextureManager::updateMipMapSkipLevel(const Rc<DxvkContext>& context) {
    // Check video memory
    VkPhysicalDeviceMemoryProperties memory = m_device->adapter()->memoryProperties();
    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();
    VkDeviceSize availableMemorySizeMib = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
      bool isDeviceLocal = memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
      if (!isDeviceLocal) {
        continue;
      }

      VkDeviceSize memSizeMib = memHeapInfo.heaps[i].memoryBudget >> 20;
      VkDeviceSize memUsedMib = memHeapInfo.heaps[i].memoryAllocated >> 20;

      availableMemorySizeMib = std::max(memSizeMib - memUsedMib, availableMemorySizeMib);
    }

    const int GB = 1024;
    RtxContext* rtxContext = dynamic_cast<RtxContext*>(context.ptr());
    if (rtxContext && !rtxContext->getResourceManager().isResourceReady()) {
      // Assume raytracing resources like buffers occupy 2GB
      const int pipelineResourceMemorySize = 2 * GB;
      availableMemorySizeMib = std::max(int(availableMemorySizeMib) - pipelineResourceMemorySize, 0);
    }

    // Check system memory
    uint64_t availableSystemMemorySizeByte;
    if (dxvk::env::getAvailableSystemPhysicalMemory(availableSystemMemorySizeByte)) {
      // This function is invoked during initialization, and the game may not have loaded other data.
      // Reserve 2GB space for other game data.
      // TODO: The OpacityMicromapMemoryManager also allocate memory adaptively and it may eat up the memory
      // saved here. Need to figure out a way to control global memory consumption.
      VkDeviceSize assetReservedSizeMib = std::max(static_cast<int>(availableSystemMemorySizeByte >> 20) - 2 * GB, 0);
      availableMemorySizeMib = std::min(availableMemorySizeMib, assetReservedSizeMib);
    }

    int assetSizeMib = RtxOptions::Get()->assetEstimatedSizeGB() * GB;
    for (m_minimumMipLevel = 0; assetSizeMib > availableMemorySizeMib && m_minimumMipLevel < 2; m_minimumMipLevel++) {
      // Skip one more mip map level
      assetSizeMib /= 4;
    }

    return m_minimumMipLevel;
  }

}
