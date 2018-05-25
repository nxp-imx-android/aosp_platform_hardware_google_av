/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "C2SoftG711"
#include <utils/Log.h>

#include "C2SoftG711.h"

#include <C2PlatformSupport.h>
#include <SimpleInterfaceCommon.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/MediaDefs.h>

namespace android {

#ifdef ALAW
constexpr char COMPONENT_NAME[] = "c2.android.g711.alaw.decoder";
#else
constexpr char COMPONENT_NAME[] = "c2.android.g711.mlaw.decoder";
#endif

class C2SoftG711::IntfImpl : public C2InterfaceHelper {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : C2InterfaceHelper(helper) {

        setDerivedInstance(this);

        addParameter(
                DefineParam(mInputFormat, C2_NAME_INPUT_STREAM_FORMAT_SETTING)
                .withConstValue(new C2StreamFormatConfig::input(0u, C2FormatCompressed))
                .build());

        addParameter(
                DefineParam(mOutputFormat, C2_NAME_OUTPUT_STREAM_FORMAT_SETTING)
                .withConstValue(new C2StreamFormatConfig::output(0u, C2FormatAudio))
                .build());

        addParameter(
                DefineParam(mInputMediaType, C2_NAME_INPUT_PORT_MIME_SETTING)
                .withConstValue(AllocSharedString<C2PortMimeConfig::input>(
#ifdef ALAW
                        MEDIA_MIMETYPE_AUDIO_G711_ALAW
#else
                        MEDIA_MIMETYPE_AUDIO_G711_MLAW
#endif
                )).build());

        addParameter(
                DefineParam(mOutputMediaType, C2_NAME_OUTPUT_PORT_MIME_SETTING)
                .withConstValue(AllocSharedString<C2PortMimeConfig::output>(
                        MEDIA_MIMETYPE_AUDIO_RAW))
                .build());

        addParameter(
                DefineParam(mSampleRate, C2_NAME_STREAM_SAMPLE_RATE_SETTING)
                .withDefault(new C2StreamSampleRateInfo::output(0u, 8000))
                .withFields({C2F(mSampleRate, value).inRange(8000, 48000)})
                .withSetter((Setter<decltype(*mSampleRate)>::StrictValueWithNoDeps))
                .build());

        addParameter(
                DefineParam(mChannelCount, C2_NAME_STREAM_CHANNEL_COUNT_SETTING)
                .withDefault(new C2StreamChannelCountInfo::output(0u, 1))
                .withFields({C2F(mChannelCount, value).equalTo(1)})
                .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrate, C2_NAME_STREAM_BITRATE_SETTING)
                .withDefault(new C2BitrateTuning::input(0u, 64000))
                .withFields({C2F(mBitrate, value).equalTo(64000)})
                .withSetter(Setter<decltype(*mBitrate)>::NonStrictValueWithNoDeps)
                .build());
    }

private:
    std::shared_ptr<C2StreamFormatConfig::input> mInputFormat;
    std::shared_ptr<C2StreamFormatConfig::output> mOutputFormat;
    std::shared_ptr<C2PortMimeConfig::input> mInputMediaType;
    std::shared_ptr<C2PortMimeConfig::output> mOutputMediaType;
    std::shared_ptr<C2StreamSampleRateInfo::output> mSampleRate;
    std::shared_ptr<C2StreamChannelCountInfo::output> mChannelCount;
    std::shared_ptr<C2BitrateTuning::input> mBitrate;
};

C2SoftG711::C2SoftG711(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl) {
}

C2SoftG711::~C2SoftG711() {
    onRelease();
}

c2_status_t C2SoftG711::onInit() {
    mSignalledOutputEos = false;
    return C2_OK;
}

c2_status_t C2SoftG711::onStop() {
    mSignalledOutputEos = false;
    return C2_OK;
}

void C2SoftG711::onReset() {
    (void)onStop();
}

void C2SoftG711::onRelease() {
}

c2_status_t C2SoftG711::onFlush_sm() {
    return onStop();
}

void C2SoftG711::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    if (mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    const C2ConstLinearBlock inBuffer =
        work->input.buffers[0]->data().linearBlocks().front();
    C2ReadView rView = inBuffer.map().get();
    size_t inOffset = inBuffer.offset();
    size_t inSize = inBuffer.size();
    int outSize =  inSize * sizeof(int16_t);
    if (inSize && rView.error()) {
        ALOGE("read view map failed %d", rView.error());
        work->result = C2_CORRUPTED;
        return;
    }
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;

    ALOGV("in buffer attr. size %zu timestamp %d frameindex %d", inSize,
          (int)work->input.ordinal.timestamp.peeku(), (int)work->input.ordinal.frameIndex.peeku());

    if (inSize == 0) {
        work->worklets.front()->output.flags = work->input.flags;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
        if (eos) {
            mSignalledOutputEos = true;
            ALOGV("signalled EOS");
        }
        return;
    }

    uint8_t *inputptr = const_cast<uint8_t *>(rView.data() + inOffset);

    std::shared_ptr<C2LinearBlock> block;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
    c2_status_t err = pool->fetchLinearBlock(outSize, usage, &block);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock for Output failed with status %d", err);
        work->result = C2_NO_MEMORY;
        return;
    }
    C2WriteView wView = block->map().get();
    if (wView.error()) {
        ALOGE("write view map failed %d", wView.error());
        work->result = C2_CORRUPTED;
        return;
    }
    int16_t *outputptr = reinterpret_cast<int16_t *>(wView.data());

#ifdef ALAW
    DecodeALaw(outputptr, inputptr, inSize);
#else
    DecodeMLaw(outputptr, inputptr, inSize);
#endif

    work->worklets.front()->output.flags = work->input.flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(createLinearBuffer(block));
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;

    if (eos) {
        mSignalledOutputEos = true;
        ALOGV("signalled EOS");
    }
}

c2_status_t C2SoftG711::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    (void) pool;
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    return C2_OK;
}

#ifdef ALAW
void C2SoftG711::DecodeALaw(
        int16_t *out, const uint8_t *in, size_t inSize) {
    while (inSize > 0) {
        inSize--;
        int32_t x = *in++;

        int32_t ix = x ^ 0x55;
        ix &= 0x7f;

        int32_t iexp = ix >> 4;
        int32_t mant = ix & 0x0f;

        if (iexp > 0) {
            mant += 16;
        }

        mant = (mant << 4) + 8;

        if (iexp > 1) {
            mant = mant << (iexp - 1);
        }

        *out++ = (x > 127) ? mant : -mant;
    }
}
#else
void C2SoftG711::DecodeMLaw(
        int16_t *out, const uint8_t *in, size_t inSize) {
    while (inSize > 0) {
        inSize--;
        int32_t x = *in++;

        int32_t mantissa = ~x;
        int32_t exponent = (mantissa >> 4) & 7;
        int32_t segment = exponent + 1;
        mantissa &= 0x0f;

        int32_t step = 4 << segment;

        int32_t abs = (0x80l << exponent) + step * mantissa + step / 2 - 4 * 33;

        *out++ = (x < 0x80) ? -abs : abs;
    }
}
#endif

class C2SoftG711DecFactory : public C2ComponentFactory {
public:
    C2SoftG711DecFactory() : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GetCodec2PlatformComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2SoftG711(COMPONENT_NAME, id,
                               std::make_shared<C2SoftG711::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2SoftG711::IntfImpl>(
                        COMPONENT_NAME, id, std::make_shared<C2SoftG711::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual ~C2SoftG711DecFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

}  // namespace android

extern "C" ::C2ComponentFactory* CreateCodec2Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2SoftG711DecFactory();
}

extern "C" void DestroyCodec2Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}