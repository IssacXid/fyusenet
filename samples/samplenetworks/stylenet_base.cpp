//--------------------------------------------------------------------------------------------------
// FyuseNet Samples                                                            (c) Fyusion Inc. 2022
//--------------------------------------------------------------------------------------------------
// Style-Transfer Network Base Class
// Creator: Martin Wawro
// SPDX-License-Identifier: MIT
//--------------------------------------------------------------------------------------------------


//--------------------------------------- System Headers -------------------------------------------

#include <cassert>
#include <cstring>
#include <memory>

//-------------------------------------- Project  Headers ------------------------------------------

#include "stylenet3x3.h"
#include <fyusenet/gl/fbo.h>
#include "../helpers/stylenet_provider.h"

//-------------------------------------- Global Variables ------------------------------------------

//-------------------------------------- Local Definitions -----------------------------------------



/*##################################################################################################
#                                   P U B L I C  F U N C T I O N S                                 #
##################################################################################################*/

/**
 * @brief Construct style-transfer network
 *
 * @param width Width of image data to process
 * @param height Height of image data to process
 * @param upload Create an upload layer in the network (will be named "upload")
 * @param download Create a download layer in the network (will be named "download")
 * @param ctx Link to GL context that the network should use
 */
StyleNetBase::StyleNetBase(int width, int height, bool upload, bool download, const fyusion::fyusenet::GfxContextLink& ctx) :
    fyusion::fyusenet::NeuralNetwork(ctx), upload_(upload), download_(download) {
    width_ = width;
    height_ = height;
    for (int i=0; i < ASYNC_BUFFERS; i++) inBuffers_[i] = nullptr;
}


/**
 * @brief Destructor
 *
 * Deallocates resources
 */
StyleNetBase::~StyleNetBase() {
    for (int i=0; i < ASYNC_BUFFERS; i++) {
        delete inBuffers_[i];
        inBuffers_[i] = nullptr;
        delete asyncDLBuffers_[i];
        asyncDLBuffers_[i] = nullptr;
    }
    delete parameters_;
    parameters_ = nullptr;
}



/**
 * @copydoc fyusion::fyusenet::NeuralNetwork::forward()
 */
fyusion::fyusenet::NeuralNetwork::execstate StyleNetBase::forward(fyusion::fyusenet::StateToken * token) {
#ifdef FYUSENET_MULTITHREADING
    if (async_) {
        std::unique_lock<std::mutex> lck(downloadBufferLock_);
        downloadBufferAvail_.wait(lck, [this]() { return (usedDownloadBuffers_ < ASYNC_BUFFERS);});
        usedDownloadBuffers_++;
        lck.unlock();
        return fyusion::fyusenet::NeuralNetwork::forward(token);
    } else {
        return fyusion::fyusenet::NeuralNetwork::forward(token);
    }
#else
    return fyusion::fyusenet::NeuralNetwork::forward(token);
#endif
}



/**
 * @brief Try to set input CPU buffer to network by copying the supplied buffer contents
 *
 * @param data Pointer to 32-bit single-precision FP buffer. Must be the same dimensions as the
 *             network processing size and must be 3-channel RGB floats in [0,1] in shallow GPU
 *             order (i.e. triplets of RGB)
 *
 * @pre Network has been set up already
 *
 * @retval true if input buffer was successfully set
 * @retval false if there was no free slot to set the buffer, only happens in asynchronous operation
 *
 * Set an input RGB image to the network. If the network is busy, this function will wait until an
 * upload slot becomes available.
 *
 * @note No ownership over the supplied data is taken, caller is responsible for deallocation. It
 *       is safe to delete and/or overwrite the supplied \p data after this call, because a
 *       deep-copy is made.
 *
 * @warning This function is not re-entrant and must be used from the same thread as forward().
 *          In asynchronous implementations, a call to forward() must be executed after setting
 *          the input buffer to push the buffer through the pipeline, otherwise deadlocks will
 *          occur.
 */
void StyleNetBase::setInputBuffer(const float *data) {
    using namespace fyusion::fyusenet;
    assert(setup_);
#ifdef FYUSENET_MULTITHREADING
    int numbuffers = (async_) ? ASYNC_BUFFERS : 1;
#else
    int numbuffers = 1;
#endif
    // -------------------------------------------------------
    // Make sure that we have the necessary amount of buffers
    // allocated...
    // -------------------------------------------------------
    for (int i=0; i < numbuffers; i++) {
        if (!inBuffers_[i]) {
            inBuffers_[i] = new cpu::CPUBuffer(BufferShape(height_, width_, 3, 0, BufferShape::type::FLOAT32, BufferSpec::order::GPU_SHALLOW));
        }
    }
    gpu::UploadLayer * upload = dynamic_cast<gpu::UploadLayer *>(engine_->getLayers()["upload"]);
    assert(upload);
    CPUBuffer * buf = nullptr;
    {
#ifdef FYUSENET_MULTITHREADING
        assert(usedUploadBuffers_ <= ASYNC_BUFFERS);
        // -------------------------------------------------------
        // For async uploads, we employ multiple upload buffers
        // which we just cycle through.
        // -------------------------------------------------------
        std::unique_lock<std::mutex> lck(uploadBufferLock_);
        uploadBufferAvail_.wait(lck, [this]() { return (uploadBusy_ == false) && (usedUploadBuffers_ < ASYNC_BUFFERS); });
        if (upload->getCPUInputBuffer()) {
            buf = (upload->getCPUInputBuffer() == inBuffers_[0]) ? inBuffers_[1] : inBuffers_[0];
        } else buf = inBuffers_[0];
        usedUploadBuffers_++;
        uploadBusy_ = true;
#else
        buf = inBuffers_[0];
#endif
    }
    // one deep-copy operation too many
    float * tgt = buf->map<float>();
    assert(tgt);
    // NOTE (mw) it would be cleaner to perform a RGB -> RGBA conversion here and upload 4-channel data
    memcpy(tgt, data, buf->shape().bytes(BufferShape::order::CHANNELWISE));
    buf->unmap();
    upload->setCPUInputBuffer(buf, 0);
}



/**
 * @brief Get pointer to output buffer for download-enabled networks
 *
 * @return Pointer to CPUBuffer that is written to by the download layer
 *
 * In case download was activated for this network, this function will return a pointer to the
 * CPUBuffer instance that is being written by the download layer. In case download is not
 * activated, this function will return a \c nullptr .
 *
 * @warning If asynchronous operation is enabled, the output buffer is subject to change and
 *          must be queried inside the download callback every time it is invoked. Obtaining
 *          the output buffer by any other means is prone to errors.
 */
fyusion::fyusenet::cpu::CPUBuffer * StyleNetBase::getOutputBuffer() {
    using namespace fyusion::fyusenet;
    if ((!download_) || (!setup_)) return nullptr;
    gpu::DownloadLayer * dwn = dynamic_cast<gpu::DownloadLayer *>(engine_->getLayers()["download"]);
    return dwn->getCPUOutputBuffer(0);
}

/**
 * @brief Set input GPU buffer (GL texture) for style net
 *
 * @param buffer Pointer to GPU input buffer to set
 *
 * @note No ownership over the input buffer is taken. It is up to the caller to delete the buffer
 *       from memory.
 */
void StyleNetBase::setInputGPUBuffer(fyusion::fyusenet::gpu::GPUBuffer * buffer) {
    using namespace fyusion::fyusenet;
    using layer_ids = StyleNetProvider::layer_ids;
    if (buffer) {
        int idx = (oesInput_) ? layer_ids::UNPACK : layer_ids::CONV1;
        auto * layer = dynamic_cast<gpu::GPULayerBase *>(engine_->getLayers()[idx]);
        layer->setGPUInputBuffer(buffer, 0);
    }
}



/**
 * @brief Get (RGB) output texture
 *
 * @return GL handle of output texture (the texture the last layer writes data to)
 *
 * This function returns the texture handle that is written to by the last layer. The texture
 * contains RGB data (0..1 per channel) with undefined alpha value.
 */
GLuint StyleNetBase::getOutputTexture() const {
    using namespace fyusion::fyusenet;
    assert(engine_);
    assert(engine_->getLayers()["sigmoid"]);
    gpu::GPULayerBase * layer = static_cast<gpu::GPULayerBase *>(engine_->getLayers()["sigmoid"]);
    assert(layer);
    auto * buf = layer->getGPUOutputBuffer(0);
    assert(buf);
    return buf->getSlice(0).getHandle();
}


/*##################################################################################################
#                               N O N -  P U B L I C  F U N C T I O N S                            #
##################################################################################################*/

/**
 * @copydoc NeuralNetwork::initializeWeights
 */
void StyleNetBase::initializeWeights(fyusion::fyusenet::CompiledLayers & layers) {
    using namespace fyusion::fyusenet;
    assert(parameters_);
    for (auto it = layers.begin(); it != layers.end(); ++it) {
        it.second->loadParameters(parameters_);
    }
}



#ifdef FYUSENET_MULTITHREADING
/**
 * @brief Callback that is invoked when the pipeline completes an asynchronous download
 *
 * @param seqNo Sequence number of the upload operation
 * @param buffer Pointer to CPUBuffer that was used as target for the download operation
 * @param state State of the download (should be complete)
 *
 * This callback is invoked by an asynchronous download layer when the download operation has
 * finished and the buffer is ready to use.
 */
void StyleNetBase::internalDLCallback(uint64_t seqNo, fyusion::fyusenet::cpu::CPUBuffer *buffer, fyusion::fyusenet::AsyncLayer::state state) {
    using namespace fyusion::fyusenet;
    assert(engine_);
    gpu::DownloadLayer * down = dynamic_cast<gpu::DownloadLayer *>(engine_->getLayers()["download"]);
    assert(down);
    // --------------------------------------------
    // Perform buffer swap...
    // ---------------------------------------------
    if (state == fyusion::fyusenet::AsyncLayer::state::DOWNLOAD_COMMENCED) {
        down->updateOutputBuffer((down->getCPUOutputBuffer() == asyncDLBuffers_[0]) ? asyncDLBuffers_[1] : asyncDLBuffers_[0]);
    }
    // ---------------------------------------------
    // Run external callback if download is done
    // and also adjust the number of used DL buffers
    // ---------------------------------------------
    if (state == fyusion::fyusenet::AsyncLayer::state::DOWNLOAD_DONE) {
        if (asyncCallbacks_.downReady_) asyncCallbacks_.downReady_(down->getName(), seqNo, buffer);
        downloadBufferLock_.lock();
        usedDownloadBuffers_--;
        downloadBufferAvail_.notify_one();
        downloadBufferLock_.unlock();
    }
}
#endif


#ifdef FYUSENET_MULTITHREADING
/**
 * @brief Callback that is invoked when the pipeline commences or completes an asynchronous upload
 *
 * @param seqNo Sequence number of the upload operation
 * @param buffer Pointer to CPUBuffer that was used as source for the upload operation
 * @param state State that the upload is in (either upload is commenced or complete)
 *
 * This updates the available upload buffers for the network, once a previous upload was completed.
 */
void StyleNetBase::internalULCallback(uint64_t seqNo, fyusion::fyusenet::cpu::CPUBuffer *buffer, fyusion::fyusenet::AsyncLayer::state state) {
    using namespace fyusion::fyusenet;
    assert(engine_);
    assert(usedUploadBuffers_ <= ASYNC_BUFFERS);
    uploadBufferLock_.lock();
    gpu::UploadLayer * up = dynamic_cast<gpu::UploadLayer *>(engine_->getLayers()["upload"]);
    assert(up);
    if (state == AsyncLayer::UPLOAD_COMMENCED) {
        uploadBusy_ = false;
    }
    if (state == AsyncLayer::UPLOAD_DONE) {
        usedUploadBuffers_--;
    }
    uploadBufferAvail_.notify_one();
    uploadBufferLock_.unlock();
    if (state == AsyncLayer::UPLOAD_COMMENCED) {
        if (asyncCallbacks_.upReady_) asyncCallbacks_.upReady_(up->getName(), seqNo);
    }
}
#endif
