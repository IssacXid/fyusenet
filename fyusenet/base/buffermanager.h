//--------------------------------------------------------------------------------------------------
// FyuseNet                                                               (c) Fyusion Inc. 2016-2022
//--------------------------------------------------------------------------------------------------
// Buffer Manager (GPU and CPU Buffers)
// Creator: Martin Wawro
// SPDX-License-Identifier: MIT
//--------------------------------------------------------------------------------------------------

#pragma once

//--------------------------------------------------------------------------------------------------

//--------------------------------------- System Headers -------------------------------------------

#include <vector>
#include <algorithm>

//-------------------------------------- Project  Headers ------------------------------------------

#include "../gl/gl_sys.h"
#include "../gpu/gfxcontexttracker.h"
#include "layerbase.h"
#include "../cpu/cpulayerbase.h"
#include "../gpu/gpulayerbase.h"
#include "bufferspec.h"
#include "../cpu/cpubuffer.h"

using fyusion::fyusenet::cpu::CPUBuffer;
using fyusion::fyusenet::cpu::CPULayerBase;

namespace fyusion::fyusenet {

//------------------------------------- Public Declarations ----------------------------------------


/**
 * @brief Manager class for tensor buffers and layer connectivity
 *
 * This class manages all buffers (buffers = tensors mostly) that are used to store intermediate
 * results during the computation. In addition, it also contains the code to match/connect the
 * input/output ports of interacting layers. For textures, it will connect one or more textures
 * per port and for CPU buffers, it will use single buffers/tensors for each port.
 *
 * Unfortunately the code in this class is particularly messy, and it should be refactored first
 * thing (or second or so...)
 */
class BufferManager : public GfxContextTracker {
 public:

    /**
     * @brief
     */
    struct Buffer {

        /**
         * @brief Constructor for buffer/tensor representation (does no allocation)
         *
         * @param height Height of buffer/tensor to represent
         * @param width Width of buffer/tensor to represent
         * @param channels Number of channels of buffer/tensor to represent
         * @param intFormat OpenGL-compatible sized format for interfacing with GL code
         */
        Buffer(int height, int width, int channels, BufferSpec::sizedformat intFormat) :
              buf_(nullptr), width_(width), height_(height), channels_(channels), internalFormat_(intFormat) {
        }

        [[nodiscard]] size_t size() const;

        CPUBuffer * buf_ = nullptr;   //!< Pointer to CPU buffer (is not set for OpenGL textures)
        int width_;                   //!< Buffer width
        int height_;                  //!< Buffer height
        int channels_;                //!< Channels in buffer
        int lastInputLayer_ = -1;     //!< Number of the last (highest) layer that this buffer served as input to
        bool locked_ = false;        //!< Indicator that buffer is locked against re-use
        /**
         * Sized texture format, for buffers that interact with OpenGL
         */
        BufferSpec::sizedformat internalFormat_;
    };


    /**
     * @brief Single texture representation
     */
    // FIXME (mw) unfortunate name clash with opengl::Texture
    struct Texture {

        /**
         * @brief Construct texture representation and initialize with existing texture ID
         *
         * @param id OpenGL texture ID/handle that this object should wrap
         * @param width Width of buffer/tensor to represent
         * @param height Height of buffer/tensor to represent
         * @param intFormat OpenGL-compatible sized format for the texture
         * @param interpolation Interpolation mode to use for the texture
         */
        Texture(GLuint id, int width, int height, BufferSpec::sizedformat intFormat, BufferSpec::interp interpolation) :
              id_(id), width_(width), height_(height), internalFormat_(intFormat),
              lastInputLayer_(-1), locked_(false), interpolation_(interpolation) {
        }

        GLuint id_;                               //!< Raw GL texture handle
        int width_;                               //!< Width of the texture (pixels)
        int height_;                              //!< Height of the texture (pixels)
        BufferSpec::sizedformat internalFormat_;  //!< Internal (sized) texture format for GL
        int lastInputLayer_;                      //!< Layer number of the last (highest) layer that this texture was used as input for
        bool locked_;                             //!< Indicator if texture is to be locked (blocks re-use)
        BufferSpec::interp interpolation_;        //!< Interpolation mode
    };

    // ------------------------------------------------------------------------
    // Constructor / Destructor
    // ------------------------------------------------------------------------
    explicit BufferManager(const GfxContextLink& ctx = GfxContextLink());
    ~BufferManager() override;

    // ------------------------------------------------------------------------
    // Public methods
    // ------------------------------------------------------------------------
    void cleanup();
    void connectLayers(LayerBase *outputLayer,LayerBase *inputLayer,int inputIndex,bool lockOutput=false);
    void createCPUOutput(LayerBase *outputLayer, bool lock=false);
    void createGPUOutput(gpu::GPULayerBase *outputLayer,
                         BufferSpec::sizedformat internalFormat = gpu::GPULayerBase::TEXTURE_IFORMAT_4,
                         BufferSpec::genericformat pixelFormat = gpu::GPULayerBase::TEXTURE_FORMAT_4,
                         BufferSpec::dtype dataType = gpu::GPULayerBase::TEXTURE_TYPE_DEFAULT);

    /**
     * @brief Get estimate on how much texture memory is used by the network textures
     *
     * @return Estimate on how many bytes of texture memory are used by the internal network textures
     */
    [[nodiscard]] size_t estimatedTextureBytes() const {
        return estimatedTextureBytes_;
    }

    /**
     * @brief Retrieve number of bytes allocated in internal tensor buffers
     *
     * @return # of bytes allocated in internal tensor buffers (excluding textures)
     */
    [[nodiscard]] size_t bufferBytes() const {
        size_t sz = 0;
        std::for_each(bufferPool_.begin(), bufferPool_.end(),[&](const Buffer& buf) { sz += buf.buf_->bytes(); });
        return sz;
    }

 private:
    // ------------------------------------------------------------------------
    // Non-public methods
    // ------------------------------------------------------------------------
    static std::vector<std::pair<BufferSpec,BufferSpec>> checkIOMatch(LayerBase *inputLayer, const std::vector<BufferSpec>& inputs, const std::vector<BufferSpec>& outputs, int inputPort);
    void connectCPULayers(LayerBase *outlayer, LayerBase *inlayer, std::vector<std::pair<BufferSpec,BufferSpec>> & matches, int inputPort, bool lock);
    void connectGPULayers(gpu::GPULayerBase * outlayer, gpu::GPULayerBase * inlayer, std::vector<std::pair<BufferSpec,BufferSpec>> & matches, int inputIndex, bool lock);
    void updateLayerUse(int index, int layerNumber, bool lock=false);
    void updateLayerUseByBuffer(const CPUBuffer *buffer, int layerNumber, bool lock);
    void updateLayerUseByTextureID(GLuint id, int layerNumber, bool lock=false);
    [[nodiscard]] int findBuffer(int inputLayer, int outputLayer, int width, int height, int channels, BufferSpec::sizedformat internalFormat) const;
    [[nodiscard]] int findTexture(int inputLayer,int outputLayer, int width, int height, BufferSpec::sizedformat internalFormat, BufferSpec::interp interpolation) const;
    [[nodiscard]] int findTexture(GLuint handle) const;
    Buffer createBuffer(int width, int height, int channels, BufferSpec::sizedformat iFormat, BufferSpec::dtype dType, BufferShape::order order = BufferShape::order::CHANNELWISE);
    Texture createTexture(int width, int height, BufferSpec::sizedformat internalFormat, BufferSpec::genericformat format, BufferSpec::dtype type, BufferSpec::interp interpolation=BufferSpec::interp::ANY);

    // ------------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------------
    std::vector<Texture> texturePool_;          //!< Pool that contains all internally used textures for the network(s)
    std::vector<Buffer> bufferPool_;            //!< Pool that contains all internally used buffers for the network(s)
    size_t estimatedTextureBytes_ = 0;          //!< Number of bytes in the pooled textures (estimate)
};


} // fyusion::fyusenet namespace

// vim: set expandtab ts=4 sw=4:
