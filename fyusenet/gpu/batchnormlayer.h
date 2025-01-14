//--------------------------------------------------------------------------------------------------
// FyuseNet                                                               (c) Fyusion Inc. 2016-2022
//--------------------------------------------------------------------------------------------------
// Explicit BatchNorm Layer (Header)
// Creator: Martin Wawro
// SPDX-License-Identifier: MIT
//--------------------------------------------------------------------------------------------------

#pragma once

//--------------------------------------------------------------------------------------------------

//--------------------------------------- System Headers -------------------------------------------

#include <vector>

//-------------------------------------- Project  Headers ------------------------------------------

#include "../gl/gl_sys.h"
#include "../gl/uniformstate.h"
#include "../gl/fbo.h"
#include "../gl/shaderprogram.h"
#include "gfxcontextlink.h"
#include "functionlayer.h"

//------------------------------------- Public Declarations ----------------------------------------

namespace fyusion::fyusenet::gpu {

/**
 * @brief Batch-norm layer for shallow tensors
 *
 * This layer implements the batch-norm operator which basically scales and shifts the input
 * data using channel-individual scale + bias values.
 *
 * This layer should only be used in exceptional circumstances, since most layer types support
 * a fused/implicit batchnorm which is more efficient than doing it explicitly.
 *
 * @note This layer does not track any batches (our batch size is always 1 anyway), but uses
 *       fixed values obtained and stored during training (running means and variances)
 *
 * @see https://en.wikipedia.org/wiki/Batch_normalization
 */
class BatchNormLayer : public FunctionLayer {
 public:
    // ------------------------------------------------------------------------
    // Constructor / Destructor
    // ------------------------------------------------------------------------
    BatchNormLayer(const GPULayerBuilder & builder, int layerNumber);
    ~BatchNormLayer() override;

    // ------------------------------------------------------------------------
    // Public methods
    // ------------------------------------------------------------------------
    void cleanup() override;
    void loadParameters(const ParameterProvider * source) override;

 protected:
    /**
     * @brief Representation of single bias/scale block to be pushed into a shader uniform
     */
    struct BiasScaleBlock {
        explicit BiasScaleBlock(int channels) {
            biasScale_ = new float[2*channels];
            memset(biasScale_, 0, 2*channels*sizeof(float));
            paddedChannels_ = channels;
        }
        ~BiasScaleBlock() {
            delete [] biasScale_;
            biasScale_ = nullptr;
        }
        void fill(const float *bias, const float *scale,int channels) {
            assert(bias);
            assert(scale);
            for (int i=0; i < channels; i += PIXEL_PACKING) {
                int lim = PIXEL_PACKING;
                if (i+PIXEL_PACKING > channels) lim = channels-i;
                for (int j=0; j < lim; j++) {
                    biasScale_[2*i+j] = bias[i+j];
                    biasScale_[2*i+PIXEL_PACKING+j] = scale[i+j];
                }
            }
        }
        int paddedChannels_;
        float *biasScale_ = nullptr;
    };
    // ------------------------------------------------------------------------
    // Non-public methods
    // ------------------------------------------------------------------------
    void renderChannelBatch(int outPass,int numRenderTargets,int texOffset) override;
    void setupShaders() override;
    void beforeRender() override;
    void afterRender() override;

    // ------------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------------
    programptr shaders_[FBO::MAX_DRAWBUFFERS];          //!< Shader instances (shared) pointers (different shaders for different number of render targets)
    unistateptr shaderStates_[FBO::MAX_DRAWBUFFERS];    //!< Shader states that memorize the shader states of the #shaders_
    ShaderProgram *currentShader_ = nullptr;            //!< Raw pointer to currently active/in-use shader
    std::vector<BiasScaleBlock *> blocks_;              //!< List of blocks that contain the bias/scale information to be set into the shader uniforms

    constexpr static int UNIFORM_BIASSCALE = 1;         //!< Index of the bias/scale variable in the shader uniforms
};

} // fyusion::fyusenet::gpu namespace

// vim: set expandtab ts=4 sw=4:
