#pragma once

#define VEGAS_VERSION "DXVK v2.7.3-vegas"

#include <atomic> // [Vegas] Thread-safe state
#include <mutex>
#include "dxvk_barrier.h"
#include "dxvk_bind_mask.h"
#include "dxvk_cmdlist.h"
#include "dxvk_context_state.h"
#include "dxvk_descriptor_heap.h"
#include "dxvk_descriptor_worker.h"
#include "dxvk_implicit_resolve.h"
#include "dxvk_latency.h"
#include "dxvk_objects.h"
#include "dxvk_queue.h"
#include "dxvk_util.h"
#include "dxvk_vegas.h"

namespace dxvk {

  /**
   * \brief DXVK context
   * * Tracks pipeline state and records command lists.
   * This is where the actual rendering commands are
   * recorded.
   */
  class DxvkContext : public RcObject {
    constexpr static VkDeviceSize MaxDiscardSizeInRp = 256u << 10u;
    constexpr static VkDeviceSize MaxDiscardSize     =  16u << 10u;

    constexpr static uint32_t DirectMultiDrawBatchSize = 256u;
  public:
    
    DxvkContext(const Rc<DxvkDevice>& device);
    ~DxvkContext();

    /**
     * \brief Begins command buffer recording
     * * Begins recording a command list. This does
     * not alter any context state other than the
     * active command list.
     * \param [in] cmdList Target command list
     */
    void beginRecording(
      const Rc<DxvkCommandList>& cmdList);
    
    /**
     * \brief Ends command buffer recording
     * * Finishes recording the active command list.
     * The command list can then be submitted to
     * the device.
     * * This will not change any context state
     * other than the active command list.
     * \param [in] reason Optional debug label describing the reason
     * \returns Active command list
     */
    Rc<DxvkCommandList> endRecording(
      const VkDebugUtilsLabelEXT* reason);

    /**
     * \brief Ends frame
     *
     * Must be called once per frame before the
     * final call to \ref endRecording.
     */
    void endFrame();

    /**
     * \brief Begins latency tracking
     *
     * Notifies the beginning of a frame on the CS timeline
     * an ensures that subsequent submissions are associated
     * with the correct frame ID. Only one tracker can be
     * active at any given time.
     * \param [in] tracker Latency tracker object
     * \param [in] frameId Current frame ID
     */
    void beginLatencyTracking(
      const Rc<DxvkLatencyTracker>&     tracker,
            uint64_t                    frameId);

    /**
     * \brief Ends latency tracking
     *
     * Notifies the end of the frame. Ignored if the
     * tracker is not currently active.
     * \param [in] tracker Latency tracker object
     */
    void endLatencyTracking(
      const Rc<DxvkLatencyTracker>&     tracker);

    /**
     * \brief Flushes command buffer
     * * Transparently submits the current command
     * buffer and allocates a new one.
     * \param [in] reason Optional debug label describing the reason
     * \param [out] status Submission feedback
     */
    void flushCommandList(
      const VkDebugUtilsLabelEXT* reason,
            DxvkSubmitStatus* status);

    /**
     * \brief Synchronizes command list with WSI
     *
     * The next submission can be used to render
     * to the swap chain image and present after.
     */
    void synchronizeWsi(PresenterSync sync) {
      m_cmd->setWsiSemaphores(sync);
    }

    /**
     * \brief Begins external rendering
     *
     * Invalidates all state and provides the caller
     * with the objects necessary to start drawing.
     * \returns Current command list object
     */
    Rc<DxvkCommandList> beginExternalRendering();

    /**
     * \brief Begins generating query data
     * \param [in] query The query to end
     */
    void beginQuery(
      const Rc<DxvkQuery>&      query);
    
    /**
     * \brief Ends generating query data
     * \param [in] query The query to end
     */
    void endQuery(
      const Rc<DxvkQuery>&      query);
    
    /**
     * \brief Sets render targets
     * * Creates a framebuffer on the fly if necessary
     * and binds it using \c bindFramebuffer.
     * \param [in] targets Render targets to bind
     */
    void bindRenderTargets(
            DxvkRenderTargets&&   targets,
            VkImageAspectFlags    feedbackLoop) {
      if (likely(m_state.om.renderTargets != targets)) {
        m_state.om.renderTargets = std::move(targets);
        m_flags.set(DxvkContextFlag::GpDirtyRenderTargets);
      }

      if (unlikely(m_state.gp.state.om.feedbackLoop() != feedbackLoop)) {
        m_state.gp.state.om.setFeedbackLoop(feedbackLoop);

        m_flags.set(DxvkContextFlag::GpDirtyRenderTargets,
                    DxvkContextFlag::GpDirtyPipelineState);
      }
    }

    /**
     * \brief Binds indirect argument buffer
     * * Sets the buffers that are going to be used
     * for indirect draw and dispatch operations.
     * \param [in] argBuffer New argument buffer
     * \param [in] cntBuffer New count buffer
     */
    void bindDrawBuffers(
            DxvkBufferSlice&&     argBuffer,
            DxvkBufferSlice&&     cntBuffer) {
      m_state.id.argBuffer = std::move(argBuffer);
      m_state.id.cntBuffer = std::move(cntBuffer);

      m_flags.set(DxvkContextFlag::DirtyDrawBuffer);
    }

    /**
     * \brief Binds index buffer
     * * The index buffer will be used when
     * issuing \c drawIndexed commands.
     * \param [in] buffer New index buffer
     * \param [in] indexType Index type
     */
    void bindIndexBuffer(
            DxvkBufferSlice&&     buffer,
            VkIndexType           indexType) {
      m_state.vi.indexBuffer = std::move(buffer);
      m_state.vi.indexType   = indexType;

      m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer);
    }

    /**
     * \brief Binds index buffer range
     * * Canges the offset and size of the bound index buffer.
     * \param [in] offset Index buffer offset
     * \param [in] length Index buffer size
     * \param [in] indexType Index type
     */
    void bindIndexBufferRange(
            VkDeviceSize          offset,
            VkDeviceSize          length,
            VkIndexType           indexType) {
      m_state.vi.indexBuffer.setRange(offset, length);
      m_state.vi.indexType = indexType;

      m_flags.set(DxvkContextFlag::GpDirtyIndexBuffer);
    }

    /**
     * \brief Binds buffer to the UBO set
     * * Can be used for uniform and storage buffers bound that
     * are used within the UBO descriptor set. Storage buffers
     * within the view set must be bound via a view.
     * \param [in] stages Shader stages that access the binding
     * \param [in] slot Resource binding slot
     * \param [in] buffer Buffer to bind
     */
    void bindUniformBuffer(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            DxvkBufferSlice&&     buffer) {
      m_uniformBuffers[slot] = std::move(buffer);

      m_descriptorState.dirtyBuffers(stages);
    }

    /**
     * \brief Changes bound range of a uniform buffer
     * * Can be used to quickly bind a new sub-range of
     * a buffer rather than re-binding the entire buffer.
     */
    void bindUniformBufferRange(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            VkDeviceSize          offset,
            VkDeviceSize          length) {
      m_uniformBuffers[slot].setRange(offset, length);

      m_descriptorState.dirtyBuffers(stages);
    }
    
    /**
     * \brief Binds image view
     *
     * \param [in] stages Shader stages that access the binding
     * \param [in] slot Resource binding slot
     * \param [in] view Image view to bind
     */
    void bindResourceImageView(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            Rc<DxvkImageView>&&   view) {
      if (likely(m_resources[slot].imageView != view || m_resources[slot].bufferView)) {
        m_resources[slot].bufferView = nullptr;
        m_resources[slot].imageView = std::move(view);

        m_descriptorState.dirtyViews(stages);
      }
    }

    /**
     * \brief Binds buffer view
     *
     * \param [in] stages Shader stages that access the binding
     * \param [in] slot Resource binding slot
     * \param [in] view Buffer view to bind
     */
    void bindResourceBufferView(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            Rc<DxvkBufferView>&&  view) {
      if (likely(m_resources[slot].bufferView != view || m_resources[slot].imageView)) {
        m_resources[slot].imageView = nullptr;
        m_resources[slot].bufferView = std::move(view);

        m_descriptorState.dirtyViews(stages);
      }
    }

    /**
     * \brief Binds image sampler
     * * Binds a sampler that can be used together with
     * an image in order to read from a texture.
     * \param [in] stages Shader stages that access the binding
     * \param [in] slot Resource binding slot
     * \param [in] sampler Sampler view to bind
     */
    void bindResourceSampler(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            Rc<DxvkSampler>&&     sampler) {
      if (likely(m_samplers[slot] != sampler)) {
        m_samplers[slot] = std::move(sampler);

        m_descriptorState.dirtySamplers(stages);
      }
    }

    /**
     * \brief Binds a shader to a given state
     * * \param [in] stage Target shader stage
     * \param [in] shader The shader to bind
     */
    template<VkShaderStageFlagBits Stage>
    void bindShader(
            Rc<DxvkShader>&&      shader) {
      switch (Stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
          m_state.gp.shaders.vs = std::move(shader);
          break;

        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
          m_state.gp.shaders.tcs = std::move(shader);
          break;

        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
          m_state.gp.shaders.tes = std::move(shader);
          break;

        case VK_SHADER_STAGE_GEOMETRY_BIT:
          m_state.gp.shaders.gs = std::move(shader);
          break;

        case VK_SHADER_STAGE_FRAGMENT_BIT:
          m_state.gp.shaders.fs = std::move(shader);
          break;

        case VK_SHADER_STAGE_COMPUTE_BIT:
          m_state.cp.shaders.cs = std::move(shader);
          break;

        default:
          return;
      }

      if (Stage == VK_SHADER_STAGE_COMPUTE_BIT) {
        m_flags.set(
          DxvkContextFlag::CpDirtyPipelineState);
      } else {
        m_flags.set(
          DxvkContextFlag::GpDirtyPipeline,
          DxvkContextFlag::GpDirtyPipelineState);
      }
    }
    
    /**
     * \brief Binds vertex buffer
     * * \param [in] binding Vertex buffer binding
     * \param [in] buffer New vertex buffer
     * \param [in] stride Stride between vertices
     */
    void bindVertexBuffer(
            uint32_t              binding,
            DxvkBufferSlice&&     buffer,
            uint32_t              stride) {
      m_state.vi.vertexBuffers[binding] = std::move(buffer);
      m_state.vi.vertexStrides[binding] = stride;
      m_flags.set(DxvkContextFlag::GpDirtyVertexBuffers);
    }

    /**
     * \brief Binds vertex buffer range
     * * Only changes offsets of a bound vertex buffer.
     * \param [in] binding Vertex buffer binding
     * \param [in] offset Vertex buffer offset
     * \param [in] length Vertex buffer size
     * \param [in] stride Stride between vertices
     */
    void bindVertexBufferRange(
            uint32_t              binding,
            VkDeviceSize          offset,
            VkDeviceSize          length,
            uint32_t              stride) {
      m_state.vi.vertexBuffers[binding].setRange(offset, length);
      m_state.vi.vertexStrides[binding] = stride;
      m_flags.set(DxvkContextFlag::GpDirtyVertexBuffers);
    }

    /**
     * \brief Binds transform feedback buffer
     * * \param [in] binding Xfb buffer binding
     * \param [in] buffer The buffer to bind
     * \param [in] counter Xfb counter buffer
     */
    void bindXfbBuffer(
            uint32_t              binding,
            DxvkBufferSlice&&     buffer,
            DxvkBufferSlice&&     counter) {
      m_state.xfb.buffers [binding] = std::move(buffer);
      m_state.xfb.counters[binding] = std::move(counter);

      m_flags.set(DxvkContextFlag::GpDirtyXfbBuffers);
    }

    void blitImageView(
      const Rc<DxvkImageView>&    dstView,
      const VkOffset3D* dstOffsets,
      const Rc<DxvkImageView>&    srcView,
      const VkOffset3D* srcOffsets,
            VkFilter              filter);
    
    void changeImageLayout(
      const Rc<DxvkImage>&        image,
            VkImageLayout         layout);
    
    void clearBuffer(
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          offset,
            VkDeviceSize          length,
            uint32_t              value);
    
    void clearBufferView(
      const Rc<DxvkBufferView>&   bufferView,
            VkDeviceSize          offset,
            VkDeviceSize          length,
            VkClearColorValue     value);
    
    void clearRenderTarget(
      const Rc<DxvkImageView>&    imageView,
            VkImageAspectFlags    clearAspects,
            VkClearValue          clearValue,
            VkImageAspectFlags    discardAspects);

    void clearImageView(
      const Rc<DxvkImageView>&    imageView,
            VkOffset3D            offset,
            VkExtent3D            extent,
            VkImageAspectFlags    aspect,
            VkClearValue          value);
    
    void copyBuffer(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcOffset,
            VkDeviceSize          numBytes);
    
    void copyBufferRegion(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
            VkDeviceSize          srcOffset,
            VkDeviceSize          numBytes);
    
    void copyBufferToImage(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
            VkExtent3D            dstExtent,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcOffset,
            VkDeviceSize          rowAlignment,
            VkDeviceSize          sliceAlignment,
            VkFormat              srcFormat);
    
    void copyImage(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
      const Rc<DxvkImage>&        srcImage,
            VkImageSubresourceLayers srcSubresource,
            VkOffset3D            srcOffset,
            VkExtent3D            extent);
    
    void copyImageRegion(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
            VkOffset3D            srcOffset,
            VkExtent3D            extent);
    
    void copyImageToBuffer(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
            VkDeviceSize          rowAlignment,
            VkDeviceSize          sliceAlignment,
            VkFormat              dstFormat,
      const Rc<DxvkImage>&        srcImage,
            VkImageSubresourceLayers srcSubresource,
            VkOffset3D            srcOffset,
            VkExtent3D            srcExtent);
    
    void copyPackedBufferImage(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstBufferOffset,
            VkOffset3D            dstOffset,
            VkExtent3D            dstSize,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcBufferOffset,
            VkOffset3D            srcOffset,
            VkExtent3D            srcSize,
            VkExtent3D            extent,
            VkDeviceSize          elementSize);

    void copySparsePagesToBuffer(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
      const Rc<DxvkPagedResource>& srcResource,
            uint32_t              pageCount,
      const uint32_t* pages);

    void copySparsePagesFromBuffer(
      const Rc<DxvkPagedResource>& dstResource,
            uint32_t              pageCount,
      const uint32_t* pages,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcOffset);

    void discardImage(
      const Rc<DxvkImage>&          image);

    void dispatch(
            uint32_t              x,
            uint32_t              y,
            uint32_t              z);
    
    void dispatchIndirect(
            VkDeviceSize          offset);
            
    void draw(
      uint32_t vertexCount,
      uint32_t instanceCount,
      uint32_t firstVertex,
      uint32_t firstInstance);
    
    void draw(
            uint32_t              count,
      const VkDrawIndirectCommand* draws);

    void drawIndirect(
            VkDeviceSize          offset,
            uint32_t              count,
            uint32_t              stride,
            bool                  unroll);
    
    void drawIndirectCount(
            VkDeviceSize          offset,
            VkDeviceSize          countOffset,
            uint32_t              maxCount,
            uint32_t              stride);
    
    void drawIndexed(
      uint32_t indexCount,
      uint32_t instanceCount,
      uint32_t firstIndex,
      int32_t  vertexOffset,
      uint32_t firstInstance);
    
    void drawIndexed(
            uint32_t              count,
      const VkDrawIndexedIndirectCommand* draws);

    void drawIndexedIndirect(
            VkDeviceSize          offset,
            uint32_t              count,
            uint32_t              stride,
            bool                  unroll);

    void drawIndexedIndirectCount(
            VkDeviceSize          offset,
            VkDeviceSize          countOffset,
            uint32_t              maxCount,
            uint32_t              stride);
    
    void drawIndirectXfb(
            VkDeviceSize          counterOffset,
            uint32_t              counterDivisor,
            uint32_t              counterBias);
    
    void emitGraphicsBarrier(
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);

    void acquireExternalResource(
      const Rc<DxvkPagedResource>&    resource,
            VkImageLayout             layout);

    void releaseExternalResource(
      const Rc<DxvkPagedResource>&    resource,
            VkImageLayout             layout);

    void generateMipmaps(
      const Rc<DxvkImageView>&        imageView,
            VkFilter                  filter);

    void initBuffer(
      const Rc<DxvkBuffer>&           buffer);

    void initImage(
      const Rc<DxvkImage>&            image,
            VkImageLayout             initialLayout);

    void initSparseImage(
      const Rc<DxvkImage>&            image);

    void invalidateBuffer(
      const Rc<DxvkBuffer>&           buffer,
            Rc<DxvkResourceAllocation>&& slice);

    void ensureBufferAddress(
      const Rc<DxvkBuffer>&           buffer);

    void invalidateImage(
      const Rc<DxvkImage>&            image,
            Rc<DxvkResourceAllocation>&& slice,
            VkImageLayout             layout);
    
    void invalidateImageWithUsage(
      const Rc<DxvkImage>&            image,
            Rc<DxvkResourceAllocation>&& slice,
      const DxvkImageUsageInfo&       usageInfo,
            VkImageLayout             layout);

    bool ensureImageCompatibility(
      const Rc<DxvkImage>&            image,
      const DxvkImageUsageInfo&       usageInfo);

    void pushData(
            VkShaderStageFlags        stages,
            uint32_t                  offset,
            uint32_t                  size,
      const void* data) {
      uint32_t index = DxvkPushDataBlock::computeIndex(stages);

      uint32_t baseOffset = computePushDataBlockOffset(index);
      std::memcpy(&m_state.pc.constantData[baseOffset + offset], data, size);

      m_flags.set(DxvkContextFlag::DirtyPushData);
    }

    void resolveImage(
      const Rc<DxvkImage>&            dstImage,
      const Rc<DxvkImage>&            srcImage,
      const VkImageResolve&           region,
            VkFormat                  format,
            VkResolveModeFlagBits     mode,
            VkResolveModeFlagBits     stencilMode);

    void transformImage(
      const Rc<DxvkImage>&            dstImage,
      const VkImageSubresourceRange&  dstSubresources,
            VkImageLayout             srcLayout,
            VkImageLayout             dstLayout);
    
    void updateBuffer(
      const Rc<DxvkBuffer>&           buffer,
            VkDeviceSize              offset,
            VkDeviceSize              size,
      const void* data);
    
    void uploadBuffer(
      const Rc<DxvkBuffer>&           buffer,
      const Rc<DxvkBuffer>&           source,
            VkDeviceSize              sourceOffset);
    
    void uploadImage(
      const Rc<DxvkImage>&            image,
      const Rc<DxvkBuffer>&           source,
            VkDeviceSize              sourceOffset,
            VkDeviceSize              subresourceAlignment,
            VkFormat                  format);

    void setViewports(
            uint32_t            viewportCount,
      const DxvkViewport* viewports);

    void setBlendConstants(
            DxvkBlendConstants  blendConstants);
    
    void setDepthBias(
            DxvkDepthBias       depthBias);

    void setDepthBiasRepresentation(
            DxvkDepthBiasRepresentation  depthBiasRepresentation);
    
    void setDepthBounds(
            DxvkDepthBounds     depthBounds);
    
    void setStencilReference(
            uint32_t            reference);
    
    void setInputAssemblyState(
      const DxvkInputAssemblyState& ia);
    
    void setInputLayout(
            uint32_t              attributeCount,
      const DxvkVertexInput* attributes,
            uint32_t              bindingCount,
      const DxvkVertexInput* bindings);

    void setRasterizerState(
      const DxvkRasterizerState& rs);
    
    void setMultisampleState(
      const DxvkMultisampleState& ms);
    
    void setDepthStencilState(
      const DxvkDepthStencilState& ds);
    
    void setLogicOpState(
      const DxvkLogicOpState&   lo);
    
    void setBlendMode(
            uint32_t            attachment,
      const DxvkBlendMode&      blendMode);
    
    void setSpecConstant(
            VkPipelineBindPoint pipeline,
            uint32_t            index,
            uint32_t            value) {
      auto& scState = pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS
        ? m_state.gp.constants : m_state.cp.constants;
      
      if (scState.data[index] != value) {
        scState.data[index] = value;

        if (scState.mask & (1u << index)) {
          m_flags.set(pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS
            ? DxvkContextFlag::GpDirtySpecConstants
            : DxvkContextFlag::CpDirtySpecConstants);
        }
      }
    }
    
    void setBarrierControl(
            DxvkBarrierControlFlags control);

    void updatePageTable(
      const DxvkSparseBindInfo&   bindInfo,
            DxvkSparseBindFlags   flags);

    void launchCuKernelNVX(
      const VkCuLaunchInfoNVX& nvxLaunchInfo,
      const std::vector<std::pair<Rc<DxvkBuffer>, DxvkAccessFlags>>& buffers,
      const std::vector<std::pair<Rc<DxvkImage>, DxvkAccessFlags>>& images);
    
    void signalGpuEvent(
      const Rc<DxvkEvent>&      event);
    
    void writeTimestamp(
      const Rc<DxvkQuery>&      query);
    
    void signal(
      const Rc<sync::Signal>&   signal,
            uint64_t            value);

    void waitFence(const Rc<DxvkFence>& fence, uint64_t value);

    void signalFence(const Rc<DxvkFence>& fence, uint64_t value);

    void beginDebugLabel(const VkDebugUtilsLabelEXT& label);

    void endDebugLabel();

    void insertDebugLabel(const VkDebugUtilsLabelEXT& label);

    void addStatCtr(DxvkStatCounter counter, uint64_t value) {
      if (m_cmd != nullptr)
        m_cmd->addStatCtr(counter, value);
    }

    void setDebugName(const Rc<DxvkPagedResource>& resource, const char* name);

  private:
    
    Rc<DxvkDevice>          m_device;
    DxvkObjects* m_common;

    uint64_t                m_trackingId = 0u;
    uint32_t                m_renderPassIndex = 0u;
    
    Rc<DxvkCommandList>     m_cmd;
    Rc<DxvkBuffer>          m_zeroBuffer;

    DxvkContextFlags        m_flags;
    DxvkContextState        m_state;
    DxvkContextFeatures     m_features;
    DxvkDescriptorState     m_descriptorState;

    Rc<DxvkDescriptorPool>  m_descriptorPool;

    Rc<DxvkResourceDescriptorHeap> m_descriptorHeap;

    DxvkBarrierBatch        m_sdmaAcquires;
    DxvkBarrierBatch        m_sdmaBarriers;
    DxvkBarrierBatch        m_initAcquires;
    DxvkBarrierBatch        m_initBarriers;
    DxvkBarrierBatch        m_execBarriers;
    DxvkBarrierTracker      m_barrierTracker;
    DxvkBarrierControlFlags m_barrierControl;

    small_vector<DxvkResourceAccess, MaxNumRenderTargets + 1u> m_rtAccess;

    DxvkGpuQueryManager     m_queryManager;

    DxvkGlobalPipelineBarrier m_renderPassBarrierSrc = { };
    DxvkGlobalPipelineBarrier m_renderPassBarrierDst = { };

    std::vector<DxvkDeferredClear> m_deferredClears;
    std::array<DxvkDeferredResolve, MaxNumRenderTargets + 1u> m_deferredResolves = { };

    struct {
      std::vector<VkWriteDescriptorSet> writes;
      std::vector<DxvkLegacyDescriptor> infos;
    } m_legacyDescriptors;

    std::array<Rc<DxvkSampler>, MaxNumSamplerSlots> m_samplers;
    std::array<DxvkBufferSlice, MaxNumUniformBufferSlots> m_uniformBuffers;
    std::array<DxvkViewPair, MaxNumResourceSlots> m_resources;

    std::array<DxvkGraphicsPipeline*, 4096> m_gpLookupCache = { };
    std::array<DxvkComputePipeline*,   256> m_cpLookupCache = { };

    std::vector<VkImageMemoryBarrier2> m_imageLayoutTransitions;

    std::vector<util::DxvkDebugLabel> m_debugLabelStack;

    std::vector<Rc<DxvkImage>> m_nonDefaultLayoutImages;

    DxvkDescriptorCopyWorker m_descriptorWorker;

    Rc<DxvkLatencyTracker>  m_latencyTracker;
    uint64_t                m_latencyFrameId = 0u;
    bool                    m_endLatencyTracking = false;

    DxvkImplicitResolveTracker  m_implicitResolves;

    // ========== [Vegas] SELF-AWARE OPTIMIZATION STATE ==========
private:
    VegasProfile                m_vegasProfile;
    std::atomic<uint32_t>       m_drawsSinceSubmit{0};
    
    void initVegasProfile();
    bool checkAsyncCompilationCompat() const;
// ========== END [Vegas] ==========

    void blitImageFb(
            Rc<DxvkImageView>     dstView,
      const VkOffset3D* dstOffsets,
            Rc<DxvkImageView>     srcView,
      const VkOffset3D* srcOffsets,
            VkFilter              filter);

    void blitImageHw(
      const Rc<DxvkImageView>&    dstView,
      const VkOffset3D* dstOffsets,
      const Rc<DxvkImageView>&    srcView,
      const VkOffset3D* srcOffsets,
            VkFilter              filter);

    template<bool ToImage>
    void copyImageBufferData(
            DxvkCmdBuffer          cmd,
      const Rc<DxvkImage>&         image,
      const VkImageSubresourceLayers& imageSubresource,
            VkOffset3D            imageOffset,
            VkExtent3D            imageExtent,
            VkImageLayout         imageLayout,
      const DxvkResourceBufferInfo& bufferSlice,
            VkDeviceSize          bufferRowAlignment,
            VkDeviceSize          bufferSliceAlignment);

    void copyBufferToImageHw(
      const Rc<DxvkImage>&        image,
      const VkImageSubresourceLayers& imageSubresource,
            VkOffset3D            imageOffset,
            VkExtent3D            imageExtent,
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          bufferOffset,
            VkDeviceSize          bufferRowAlignment,
            VkDeviceSize          bufferSliceAlignment);

    void copyBufferToImageFb(
      const Rc<DxvkImage>&        image,
      const VkImageSubresourceLayers& imageSubresource,
            VkOffset3D            imageOffset,
            VkExtent3D            imageExtent,
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          bufferOffset,
            VkDeviceSize          bufferRowAlignment,
            VkDeviceSize          bufferSliceAlignment,
            VkFormat              bufferFormat);

    void copyImageToBufferHw(
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          bufferOffset,
            VkDeviceSize          bufferRowAlignment,
            VkDeviceSize          bufferSliceAlignment,
      const Rc<DxvkImage>&        image,
            VkImageSubresourceLayers imageSubresource,
            VkOffset3D            imageOffset,
            VkExtent3D            imageExtent);

    void copyImageToBufferCs(
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          bufferOffset,
            VkDeviceSize          bufferRowAlignment,
            VkDeviceSize          bufferSliceAlignment,
            VkFormat              bufferFormat,
      const Rc<DxvkImage>&        image,
            VkImageSubresourceLayers imageSubresource,
            VkOffset3D            imageOffset,
            VkExtent3D            imageExtent);

    void clearImageViewFb(
      const Rc<DxvkImageView>&    imageView,
            VkOffset3D            offset,
            VkExtent3D            extent,
            VkImageAspectFlags    aspect,
            VkClearValue          value);
    
    void clearImageViewCs(
      const Rc<DxvkImageView>&    imageView,
            VkOffset3D            offset,
            VkExtent3D            extent,
            VkClearValue          value);
    
    void copyImageHw(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
      const Rc<DxvkImage>&        srcImage,
            VkImageSubresourceLayers srcSubresource,
            VkOffset3D            srcOffset,
            VkExtent3D            extent);
    
    void copyImageFb(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
      const Rc<DxvkImage>&        srcImage,
            VkImageSubresourceLayers srcSubresource,
            VkOffset3D            srcOffset,
            VkExtent3D            extent);

    bool copyImageClear(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
            VkExtent3D            dstExtent,
      const Rc<DxvkImage>&        srcImage,
            VkImageSubresourceLayers srcSubresource);

    template<bool ToBuffer>
    void copySparsePages(
      const Rc<DxvkPagedResource>& sparse,
            uint32_t              pageCount,
      const uint32_t* pages,
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          offset);

    template<bool ToBuffer>
    void copySparseBufferPages(
      const Rc<DxvkBuffer>&       sparse,
            uint32_t              pageCount,
      const uint32_t* pages,
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          offset);

    template<bool ToBuffer>
    void copySparseImagePages(
      const Rc<DxvkImage>&        sparse,
            uint32_t              pageCount,
      const uint32_t* pages,
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          offset);

    template<bool Indexed, typename T>
    void drawGeneric(
            uint32_t              count,
      const T* draws);

    template<bool Indexed>
    void drawIndirectGeneric(
            VkDeviceSize          offset,
            uint32_t              count,
            uint32_t              stride,
            bool                  unroll);

    template<bool Indexed>
    void drawIndirectCountGeneric(
            VkDeviceSize          offset,
            VkDeviceSize          countOffset,
            uint32_t              maxCount,
            uint32_t              stride);

    void resolveImageHw(
      const Rc<DxvkImage>&            dstImage,
      const Rc<DxvkImage>&            srcImage,
      const VkImageResolve&           region);
    
    void resolveImageRp(
      const Rc<DxvkImage>&            dstImage,
      const Rc<DxvkImage>&            srcImage,
      const VkImageResolve&           region,
            VkFormat                  format,
            VkResolveModeFlagBits     mode,
            VkResolveModeFlagBits     stencilMode,
            bool                      flushClears);

    void resolveImageFb(
      const Rc<DxvkImage>&            dstImage,
      const Rc<DxvkImage>&            srcImage,
      const VkImageResolve&           region,
            VkFormat                  format,
            VkResolveModeFlagBits     depthMode,
            VkResolveModeFlagBits     stencilMode);

    bool resolveImageClear(
      const Rc<DxvkImage>&            dstImage,
      const Rc<DxvkImage>&            srcImage,
      const VkImageResolve&           region,
            VkFormat                  format);

    bool resolveImageInline(
      const Rc<DxvkImage>&            dstImage,
      const Rc<DxvkImage>&            srcImage,
      const VkImageResolve&           region,
            VkFormat                  format,
            VkResolveModeFlagBits     depthMode,
            VkResolveModeFlagBits     stencilMode);

    void uploadImageFb(
      const Rc<DxvkImage>&            image,
      const Rc<DxvkBuffer>&           source,
            VkDeviceSize              sourceOffset,
            VkDeviceSize              subresourceAlignment,
            VkFormat                  format);

    void uploadImageHw(
      const Rc<DxvkImage>&            image,
      const Rc<DxvkBuffer>&           source,
            VkDeviceSize              subresourceAlignment,
            VkDeviceSize              sourceOffset);

    VkAttachmentStoreOp determineClearStoreOp(
            VkAttachmentLoadOp        loadOp) const;

    void performClear(
      const Rc<DxvkImageView>&        imageView,
            int32_t                   attachmentIndex,
            VkImageAspectFlags        discardAspects,
            VkImageAspectFlags        clearAspects,
            VkClearValue              clearValue);

    void deferClear(
      const Rc<DxvkImageView>&        imageView,
            VkImageAspectFlags        clearAspects,
            VkClearValue              clearValue);

    void deferDiscard(
      const Rc<DxvkImageView>&        imageView,
            VkImageAspectFlags        discardAspects);

    void hoistInlineClear(
            DxvkDeferredClear&        clear,
            VkRenderingAttachmentInfo& attachment,
            VkImageAspectFlagBits     aspect);

    void flushClearsInline();

    void flushClears(
            bool                      useRenderPass);

    void flushRenderPassDiscards();

    void flushRenderPassResolves();

    void flushResolves();

    void finalizeLoadStoreOps();

    void adjustAttachmentLoadStoreOps(
            VkRenderingAttachmentInfo&  attachment,
            DxvkAccess                  access) const;

    void startRenderPass();
    void spillRenderPass(bool suspend);
    
    void acquireRenderTargets(
      const DxvkFramebufferInfo&  framebufferInfo,
            DxvkRenderPassOps&    ops);

    void releaseRenderTargets();

    void renderPassBindFramebuffer(
      const DxvkFramebufferInfo&  framebufferInfo,
            DxvkRenderPassOps&    ops);
    
    void renderPassUnbindFramebuffer();
    
    void resetRenderPassOps(
      const DxvkRenderTargets&    renderTargets,
            DxvkRenderPassOps&    renderPassOps);

    void startTransformFeedback();
    void pauseTransformFeedback();
    
    void unbindComputePipeline();
    bool updateComputePipelineState();
    
    void unbindGraphicsPipeline();
    bool updateGraphicsPipeline(bool async = false);
    bool updateGraphicsPipelineState();

    uint32_t getGraphicsPipelineDebugColor() const;

    template<VkPipelineBindPoint BindPoint>
    void resetSpecConstants(
            uint32_t                newMask);

    template<VkPipelineBindPoint BindPoint>
    void updateSpecConstants();

    void invalidateState();

    template<VkPipelineBindPoint BindPoint>
    void updateSamplerSet(const DxvkPipelineLayout* layout);

    template<VkPipelineBindPoint BindPoint>
    bool updateResourceBindings(const DxvkPipelineBindings* layout);

    template<VkPipelineBindPoint BindPoint>
    void updateDescriptorSetsBindings(const DxvkPipelineBindings* layout);

    template<VkPipelineBindPoint BindPoint>
    bool updateDescriptorBufferBindings(const DxvkPipelineBindings* layout);

    template<VkPipelineBindPoint BindPoint>
    void updatePushDataBindings(const DxvkPipelineBindings* layout);

    void updateComputeShaderResources();
    bool updateGraphicsShaderResources();

    DxvkFramebufferInfo makeFramebufferInfo(
      const DxvkRenderTargets&      renderTargets);

    void updateRenderTargets();
    
    bool flushDeferredClear(
      const DxvkImage&              image,
      const VkImageSubresourceRange& subresources);

    DxvkDeferredClear* findDeferredClear(
      const DxvkImage&              image,
      const VkImageSubresourceRange& subresources);

    DxvkDeferredClear* findOverlappingDeferredClear(
      const DxvkImage&              image,
      const VkImageSubresourceRange& subresources);

    void updateIndexBufferBinding();
    void updateVertexBufferBindings();

    void updateTransformFeedbackBuffers();
    void updateTransformFeedbackState();

    void updateDynamicState();

    template<VkPipelineBindPoint BindPoint>
    void updatePushData();
    
    template<bool Indirect, bool Resolve = true>
    bool commitComputeState();
    
    template<bool Indexed, bool Indirect, bool Resolve = true>
    bool commitGraphicsState();
    
    template<VkPipelineBindPoint BindPoint>
    bool checkResourceHazards(
      const DxvkPipelineBindings* layout);

    template<bool Indirect>
    bool checkComputeHazards();

    template<bool Indexed, bool Indirect>
    bool checkGraphicsHazards();

    template<VkPipelineBindPoint BindPoint>
    bool checkBufferBarrier(
      const DxvkBufferSlice&        bufferSlice,
            VkAccessFlags             access,
            DxvkAccessOp              accessOp);

    template<VkPipelineBindPoint BindPoint>
    bool checkBufferViewBarrier(
      const Rc<DxvkBufferView>&       bufferView,
            VkAccessFlags             access,
            DxvkAccessOp              accessOp);

    template<VkPipelineBindPoint BindPoint>
    bool checkImageViewBarrier(
      const Rc<DxvkImageView>&        imageView,
            VkAccessFlags             access,
            DxvkAccessOp              accessOp);

    template<VkPipelineBindPoint BindPoint>
    DxvkAccessFlags getAllowedStorageHazards() {
      if (m_barrierControl.isClear() || m_flags.test(DxvkContextFlag::ForceWriteAfterWriteSync))
        return DxvkAccessFlags();

      if constexpr (BindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        // If there are any pending accesses that are not directly related
        // to shader dispatches, always insert a barrier if there is a hazard.
        VkPipelineStageFlags2 stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                        | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;

        if (!m_execBarriers.hasPendingStages(~stageMask)) {
          if (m_barrierControl.test(DxvkBarrierControl::ComputeAllowReadWriteOverlap))
            return DxvkAccessFlags(DxvkAccess::Write, DxvkAccess::Read);
          else if (m_barrierControl.test(DxvkBarrierControl::ComputeAllowWriteOnlyOverlap))
            return DxvkAccessFlags(DxvkAccess::Write);
        }
      } else {
        // For graphics, the only type of unrelated access we have to worry about
        // is transform feedback writes, in which case inserting a barrier is fine.
        if (m_barrierControl.test(DxvkBarrierControl::GraphicsAllowReadWriteOverlap))
          return DxvkAccessFlags(DxvkAccess::Write, DxvkAccess::Read);
      }

      return DxvkAccessFlags();
    }


    void emitMemoryBarrier(
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);

    void trackDrawBuffer();

    bool tryInvalidateDeviceLocalBuffer(
      const Rc<DxvkBuffer>&           buffer,
            VkDeviceSize              copySize);

    Rc<DxvkImageView> ensureImageViewCompatibility(
      const Rc<DxvkImageView>&        view,
            VkImageUsageFlagBits      usage);

    void relocateResources(
            size_t                    bufferCount,
      const DxvkRelocateBufferInfo* bufferInfos,
            size_t                    imageCount,
      const DxvkRelocateImageInfo* imageInfos);

    void relocateQueuedResources();

    Rc<DxvkSampler> createBlitSampler(
            VkFilter                  filter);

    DxvkGraphicsPipeline* lookupGraphicsPipeline(
      const DxvkGraphicsPipelineShaders&  shaders);

    DxvkComputePipeline* lookupComputePipeline(
      const DxvkComputePipelineShaders&   shaders);
    
    Rc<DxvkBuffer> createZeroBuffer(
            VkDeviceSize              size);

    void freeZeroBuffer();

    void resizeDescriptorArrays(
            uint32_t                  bindingCount);

    void flushImplicitResolves();

    void beginCurrentCommands();

    void endCurrentCommands();

    void splitCommands();

    void discardRenderTarget(
      const DxvkImage&                image,
      const VkImageSubresourceRange&  subresources);

    void flushImageLayoutTransitions(
            DxvkCmdBuffer             cmdBuffer);

    void addImageLayoutTransition(
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess);

    void addImageLayoutTransition(
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess,
            bool                      discard);

    void addImageInitTransition(
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess);

    void trackNonDefaultImageLayout(
            DxvkImage&                image);

    bool overlapsRenderTarget(
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources);

    bool restoreImageLayout(
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            bool                      keepAttachments);

    template<typename Pred>
    void restoreImageLayouts(
      const Pred&                     pred,
            bool                      keepAttachments);

    void prepareShaderReadableImages(
            bool                      renderPass);

    void prepareSharedImages();

    void transitionImageLayout(
            DxvkCmdBuffer             cmdBuffer,
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess,
            bool                      discard);

    void acquireResources(
            DxvkCmdBuffer             cmdBuffer,
            size_t                    count,
      const DxvkResourceAccess* batch,
            bool                      flushClears = true);

    void releaseResources(
            DxvkCmdBuffer             cmdBuffer,
            size_t                    count,
      const DxvkResourceAccess* batch);

    void syncResources(
            DxvkCmdBuffer             cmdBuffer,
            size_t                    count,
      const DxvkResourceAccess* batch,
            bool                      flushClears = true);

    void accessMemory(
            DxvkCmdBuffer             cmdBuffer,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess);

    void accessImage(
            DxvkCmdBuffer             cmdBuffer,
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            DxvkAccessOp              accessOp);

    void accessImage(
            DxvkCmdBuffer             cmdBuffer,
      const DxvkImageView&            imageView,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            DxvkAccessOp              accessOp);

    void accessImage(
            DxvkCmdBuffer             cmdBuffer,
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess,
            DxvkAccessOp              accessOp);

    void accessImageRegion(
            DxvkCmdBuffer             cmdBuffer,
            DxvkImage&                image,
      const VkImageSubresourceLayers& subresources,
            VkOffset3D                offset,
            VkExtent3D                extent,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            DxvkAccessOp              accessOp);

    void accessImageRegion(
            DxvkCmdBuffer             cmdBuffer,
            DxvkImage&                image,
      const VkImageSubresourceLayers& subresources,
            VkOffset3D                offset,
            VkExtent3D                extent,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess,
            DxvkAccessOp              accessOp);

    void accessImageTransfer(
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess);

    void accessBuffer(
            DxvkCmdBuffer             cmdBuffer,
            DxvkBuffer&               buffer,
            VkDeviceSize              offset,
            VkDeviceSize              size,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            DxvkAccessOp              accessOp);

    void accessBuffer(
            DxvkCmdBuffer             cmdBuffer,
            DxvkBuffer&               buffer,
            VkDeviceSize              offset,
            VkDeviceSize              size,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess,
            DxvkAccessOp              accessOp);

    void accessBuffer(
            DxvkCmdBuffer             cmdBuffer,
      const DxvkBufferSlice&          bufferSlice,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            DxvkAccessOp              accessOp);

    void accessBuffer(
            DxvkCmdBuffer             cmdBuffer,
      const DxvkBufferSlice&          bufferSlice,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess,
            DxvkAccessOp              accessOp);

    void accessBuffer(
            DxvkCmdBuffer             cmdBuffer,
            DxvkBufferView&           bufferView,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            DxvkAccessOp              accessOp);

    void accessBuffer(
            DxvkCmdBuffer             cmdBuffer,
            DxvkBufferView&           bufferView,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess,
            VkPipelineStageFlags2     dstStages,
            VkAccessFlags2            dstAccess,
            DxvkAccessOp              accessOp);

    void accessBufferTransfer(
            DxvkBuffer&               buffer,
            VkPipelineStageFlags2     srcStages,
            VkAccessFlags2            srcAccess);

    void accessDrawBuffer(
            VkDeviceSize              offset,
            uint32_t                  count,
            uint32_t                  stride,
            uint32_t                  size);

    void accessDrawCountBuffer(
            VkDeviceSize              offset);

    void flushBarriers();

    bool resourceHasAccess(
            DxvkBuffer&               buffer,
            VkDeviceSize              offset,
            VkDeviceSize              size,
            DxvkAccess                access,
            DxvkAccessOp              accessOp);

    bool resourceHasAccess(
            DxvkBufferView&           bufferView,
            DxvkAccess                access,
            DxvkAccessOp              accessOp);

    bool resourceHasAccess(
            DxvkImage&                image,
      const VkImageSubresourceRange&  subresources,
            DxvkAccess                access,
            DxvkAccessOp              accessOp);

    bool resourceHasAccess(
            DxvkImage&                image,
      const VkImageSubresourceLayers& subresources,
            VkOffset3D                offset,
            VkExtent3D                extent,
            DxvkAccess                access,
            DxvkAccessOp              accessOp);

    bool resourceHasAccess(
            DxvkImageView&            imageView,
            DxvkAccess                access,
            DxvkAccessOp              accessOp);

    DxvkBarrierBatch& getBarrierBatch(
            DxvkCmdBuffer             cmdBuffer);

    DxvkCmdBuffer prepareOutOfOrderTransfer(
            DxvkCmdBuffer             cmdBuffer,
            size_t                    accessCount,
      const DxvkResourceAccess* accessBatch);

    bool prepareOutOfOrderTransfer(
      const Rc<DxvkBuffer>&           buffer,
            VkDeviceSize              offset,
            VkDeviceSize              size,
            DxvkAccess                access);

    bool prepareOutOfOrderTransfer(
      const Rc<DxvkBufferView>&       bufferView,
            VkDeviceSize              offset,
            VkDeviceSize              size,
            DxvkAccess                access);

    bool prepareOutOfOrderTransfer(
      const Rc<DxvkImage>&            image,
            DxvkAccess                access);

    template<VkPipelineBindPoint BindPoint, typename Pred>
    bool checkResourceBarrier(
      const Pred&                     pred,
            VkAccessFlags             access) {
      // If we're only reading the resource, only pending
      // writes matter for synchronization purposes.
      bool hasPendingWrite = pred(DxvkAccess::Write);

      if (!(access & vk::AccessWriteMask))
        return hasPendingWrite;

      if (hasPendingWrite) {
        // If there is a write-after-write hazard and synchronization
        // for those is not explicitly disabled, insert a barrier.
        DxvkAccessFlags allowedHazards = getAllowedStorageHazards<BindPoint>();

        if (!allowedHazards.test(DxvkAccess::Write))
          return true;

        // Skip barrier if overlapping read-modify-write ops are allowed.
        // This includes shader atomics, but also non-atomic load-stores.
        if (allowedHazards.test(DxvkAccess::Read))
          return false;

        // Otherwise, check if there is a read-after-write hazard.
        if (access & vk::AccessReadMask)
          return true;
      }

      // Check if there are any pending reads to avoid write-after-read issues.
      return pred(DxvkAccess::Read);
    }

    DxvkPipelineLayoutType getActivePipelineLayoutType(VkPipelineBindPoint bindPoint) const {
      return (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS && m_flags.test(DxvkContextFlag::GpIndependentSets))
        ? DxvkPipelineLayoutType::Independent
        : DxvkPipelineLayoutType::Merged;
    }

    bool needsDrawBarriers();

    void beginRenderPassDebugRegion();

    template<VkPipelineBindPoint BindPoint>
    void beginBarrierControlDebugRegion();

    void pushDebugRegion(
      const VkDebugUtilsLabelEXT&       label,
            util::DxvkDebugLabelType    type);

    void popDebugRegion(
            util::DxvkDebugLabelType    type);

    bool hasDebugRegion(
            util::DxvkDebugLabelType    type);

    void beginActiveDebugRegions();

    void endActiveDebugRegions();

    template<VkPipelineBindPoint BindPoint>
    force_inline void trackUniformBufferBinding(const DxvkShaderDescriptor& binding, const DxvkBufferSlice& slice) {
      if (BindPoint == VK_PIPELINE_BIND_POINT_COMPUTE || unlikely(slice.buffer()->hasGfxStores())) {
        accessBuffer(DxvkCmdBuffer::ExecBuffer, slice,
          util::pipelineStages(binding.getStageMask()), binding.getAccess(), DxvkAccessOp::None);
      }

      m_cmd->track(slice.buffer(), DxvkAccess::Read);
    }

    template<VkPipelineBindPoint BindPoint, bool IsWritable>
    force_inline void trackBufferViewBinding(const DxvkShaderDescriptor& binding, DxvkBufferView& view) {
      DxvkAccessOp accessOp = IsWritable ? binding.getAccessOp() : DxvkAccessOp::None;

      if (BindPoint == VK_PIPELINE_BIND_POINT_COMPUTE || unlikely(view.buffer()->hasGfxStores())) {
        accessBuffer(DxvkCmdBuffer::ExecBuffer, view,
          util::pipelineStages(binding.getStageMask()), binding.getAccess(), accessOp);
      }

      DxvkAccess access = IsWritable && (binding.getAccess() & vk::AccessWriteMask)
        ? DxvkAccess::Write : DxvkAccess::Read;
      m_cmd->track(view.buffer(), access);
    }

    template<VkPipelineBindPoint BindPoint, bool IsWritable>
    force_inline void trackImageViewBinding(const DxvkShaderDescriptor& binding, DxvkImageView& view) {
      DxvkAccessOp accessOp = IsWritable ? binding.getAccessOp() : DxvkAccessOp::None;

      if (BindPoint == VK_PIPELINE_BIND_POINT_COMPUTE || unlikely(view.hasGfxStores())) {
        accessImage(DxvkCmdBuffer::ExecBuffer, view,
          util::pipelineStages(binding.getStageMask()), binding.getAccess(), accessOp);
      }

      DxvkAccess access = IsWritable && (binding.getAccess() & vk::AccessWriteMask)
        ? DxvkAccess::Write : DxvkAccess::Read;
      m_cmd->track(view.image(), access);
    }

    static uint32_t computePushDataBlockOffset(uint32_t index) {
      return index ? MaxSharedPushDataSize + MaxPerStagePushDataSize * (index - 1u) : 0u;
    }

    static VkStencilOpState convertStencilOp(
      const DxvkStencilOp&            op,
            bool                      writable);

    static bool formatsAreCopyCompatible(
            VkFormat                  imageFormat,
            VkFormat                  bufferFormat);

    static bool formatsAreResolveCompatible(
            VkFormat                  resolveFormat,
            VkFormat                  viewFormat);

    static VkFormat sanitizeTexelBufferFormat(
            VkFormat                  srcFormat);
 
 };
 
} // End of namespace
