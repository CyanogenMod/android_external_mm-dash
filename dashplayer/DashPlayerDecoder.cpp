/*
 * Copyright (C) 2015 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "DashPlayerDecoder"

#include "DashPlayerDecoder.h"
#include <media/ICrypto.h>
#include "ESDS.h"
#include "QCMediaDefs.h"
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include "QCMetaData.h"
#include <cutils/properties.h>
#include <utils/Log.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <avc_utils.h>
#include <utils/KeyedVector.h>



//Smooth streaming settings,
//Max resolution 1080p
#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080

#define DPD_MSG_ERROR(...) ALOGE(__VA_ARGS__)
#define DPD_MSG_HIGH(...) if(mLogLevel >= 1){ALOGE(__VA_ARGS__);}
#define DPD_MSG_MEDIUM(...) if(mLogLevel >= 2){ALOGE(__VA_ARGS__);}
#define DPD_MSG_LOW(...) if(mLogLevel >= 3){ALOGE(__VA_ARGS__);}

namespace android {

DashPlayer::Decoder::Decoder(
        const sp<AMessage> &notify,
        const sp<Surface> &nativeWindow)
    : mNotify(notify),
      mNativeWindow(nativeWindow),
      mLogLevel(0),
      mBufferGeneration(0),
      mComponentName("decoder") {
    // Every decoder has its own looper because MediaCodec operations
    // are blocking, but DashPlayer needs asynchronous operations.
    mDecoderLooper = new ALooper;
    mDecoderLooper->setName("DashPlayerDecoder");
    mDecoderLooper->start(false, false, ANDROID_PRIORITY_AUDIO);

    mCodecLooper = new ALooper;
    mCodecLooper->setName("DashPlayerDecoder-MC");
    mCodecLooper->start(false, false, ANDROID_PRIORITY_AUDIO);

    char property_value[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.dash.debug.level", property_value, NULL);

    if(*property_value) {
        mLogLevel = atoi(property_value);
    }
}

DashPlayer::Decoder::~Decoder() {
}

/** @brief: configure mediacodec
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::onConfigure(const sp<AMessage> &format) {
    CHECK(mCodec == NULL);

    ++mBufferGeneration;

    AString mime;
    CHECK(format->findString("mime", &mime));

    /*
    sp<Surface> surface = NULL;
    if (mNativeWindow != NULL) {
        surface = mNativeWindow->getSurfaceTextureClient();
        if (surface.get() == NULL) {
            DPD_MSG_ERROR("Failed to create surface");
            handleError(UNKNOWN_ERROR);
            return;
        }
    }
    */

    mComponentName = mime;
    mComponentName.append(" decoder");
    DPD_MSG_HIGH("[%s] onConfigure (surface=%p)", mComponentName.c_str(), mNativeWindow.get());

    mCodec = MediaCodec::CreateByType(mCodecLooper, mime.c_str(), false /* encoder */);
    if (mCodec == NULL) {
        DPD_MSG_ERROR("Failed to create %s decoder", mime.c_str());
        handleError(UNKNOWN_ERROR);
        return;
    }

    mCodec->getName(&mComponentName);

    status_t err;
    if (mNativeWindow != NULL) {
        // disconnect from surface as MediaCodec will reconnect
        err = native_window_api_disconnect(
                mNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
        // We treat this as a warning, as this is a preparatory step.
        // Codec will try to connect to the surface, which is where
        // any error signaling will occur.
        ALOGW_IF(err != OK, "failed to disconnect from surface: %d", err);
    }
    err = mCodec->configure(
            format, mNativeWindow, NULL /* crypto */, 0 /* flags */);
    if (err != OK) {
        DPD_MSG_ERROR("Failed to configure %s decoder (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        return;
    }
    // the following should work in configured state
    CHECK_EQ((status_t)OK, mCodec->getOutputFormat(&mOutputFormat));
    CHECK_EQ((status_t)OK, mCodec->getInputFormat(&mInputFormat));

    err = mCodec->start();
    if (err != OK) {
        DPD_MSG_ERROR("Failed to start %s decoder (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        return;
    }

    // the following should work after start
    CHECK_EQ((status_t)OK, mCodec->getInputBuffers(&mInputBuffers));
    CHECK_EQ((status_t)OK, mCodec->getOutputBuffers(&mOutputBuffers));
    DPD_MSG_HIGH("[%s] got %zu input and %zu output buffers",
            mComponentName.c_str(),
            mInputBuffers.size(),
            mOutputBuffers.size());

    requestCodecNotification();
}

/** @brief:  Register activity notification to mediacodec
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::requestCodecNotification() {
    if (mCodec != NULL) {
        sp<AMessage> reply = new AMessage(kWhatCodecNotify, this);
        reply->setInt32("generation", mBufferGeneration);
        mCodec->requestActivityNotification(reply);
    }
        }

bool DashPlayer::Decoder::isStaleReply(const sp<AMessage> &msg) {
    bool bStale = false;
    int32_t generation = -1;
    if (!(msg->findInt32("generation", &generation)) || (generation != mBufferGeneration))
    {
      DPD_MSG_ERROR( "isStaleReply: generation %d mBufferGeneration %d",
                     generation, mBufferGeneration );
      bStale = true;
    }
    return bStale;
}

void DashPlayer::Decoder::init() {
    mDecoderLooper->registerHandler(this);
}

/** @brief: configure decoder
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::configure(const sp<MetaData> &meta) {
    sp<AMessage> msg = new AMessage(kWhatConfigure, this);
    sp<AMessage> format = makeFormat(meta);
    msg->setMessage("format", format);
    msg->post();
}

/** @brief: notify decoder error to dashplayer
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::handleError(int32_t err)
{
    DPD_MSG_HIGH("[%s] handleError : %d", mComponentName.c_str() , err);
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}

/** @brief: send input buffer from codec to  dashplayer
 *
 *  @return: true if valid buffer found
 *
 */
bool DashPlayer::Decoder::handleAnInputBuffer() {
    size_t bufferIx = -1;
    status_t res = mCodec->dequeueInputBuffer(&bufferIx);
    DPD_MSG_HIGH("[%s] dequeued input: %d",
            mComponentName.c_str(), res == OK ? (int)bufferIx : res);
    if (res != OK) {
        if (res != -EAGAIN) {
            handleError(res);
        }
        return false;
    }

    CHECK_LT(bufferIx, mInputBuffers.size());

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, this);
    reply->setSize("buffer-ix", bufferIx);
    reply->setInt32("generation", mBufferGeneration);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFillThisBuffer);
    notify->setBuffer("buffer", mInputBuffers[bufferIx]);
    notify->setMessage("reply", reply);
    notify->post();
    return true;
}

/** @brief: Send input buffer to  decoder
 *
 *  @return: void
 *
 */
void android::DashPlayer::Decoder::onInputBufferFilled(const sp<AMessage> &msg) {
    size_t bufferIx;
    CHECK(msg->findSize("buffer-ix", &bufferIx));
    CHECK_LT(bufferIx, mInputBuffers.size());
    sp<ABuffer> codecBuffer = mInputBuffers[bufferIx];

    sp<ABuffer> buffer;
    bool hasBuffer = msg->findBuffer("buffer", &buffer);
    if (buffer == NULL /* includes !hasBuffer */) {
        int32_t streamErr = ERROR_END_OF_STREAM;
        CHECK(msg->findInt32("err", &streamErr) || !hasBuffer);

        if (streamErr == OK) {
            /* buffers are returned to hold on to */
            return;
        }

        // attempt to queue EOS
        status_t err = mCodec->queueInputBuffer(
                bufferIx,
                0,
                0,
                0,
                MediaCodec::BUFFER_FLAG_EOS);
        if (streamErr == ERROR_END_OF_STREAM && err != OK) {
            streamErr = err;
            // err will not be ERROR_END_OF_STREAM
        }

        if (streamErr != ERROR_END_OF_STREAM) {
            handleError(streamErr);
        }
    } else {
        int64_t timeUs = 0;
        uint32_t flags = 0;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

        int32_t eos;
        // we do not expect CODECCONFIG or SYNCFRAME for decoder
        if (buffer->meta()->findInt32("eos", &eos) && eos) {
            flags |= MediaCodec::BUFFER_FLAG_EOS;
        }

        DPD_MSG_MEDIUM("Input buffer:[%s]: %p", mComponentName.c_str(),  buffer->data());

        // copy into codec buffer
        if (buffer != codecBuffer) {
            CHECK_LE(buffer->size(), codecBuffer->capacity());
            codecBuffer->setRange(0, buffer->size());
            memcpy(codecBuffer->data(), buffer->data(), buffer->size());
        }

        status_t err = mCodec->queueInputBuffer(
                        bufferIx,
                        codecBuffer->offset(),
                        codecBuffer->size(),
                        timeUs,
                        flags);
        if (err != OK) {
            DPD_MSG_ERROR("Failed to queue input buffer for %s (err=%d)",
                    mComponentName.c_str(), err);
            handleError(err);
        }
    }
}

/** @brief: dequeue out buffer from mediacodec and send it to renderer
 *
 *  @return: void
 *
 */
bool DashPlayer::Decoder::handleAnOutputBuffer() {
    size_t bufferIx = -1;
    size_t offset;
    size_t size;
    int64_t timeUs;
    uint32_t flags;
    status_t res = mCodec->dequeueOutputBuffer(
            &bufferIx, &offset, &size, &timeUs, &flags);

    if (res != OK) {
        DPD_MSG_HIGH("[%s] dequeued output: %d", mComponentName.c_str(), res);
    } else {
        DPD_MSG_HIGH("[%s] dequeued output: %d (time=%lld flags=%u)",
                mComponentName.c_str(), (int)bufferIx, timeUs, flags);
    }

    if (res == INFO_OUTPUT_BUFFERS_CHANGED) {
        res = mCodec->getOutputBuffers(&mOutputBuffers);
        if (res != OK) {
            DPD_MSG_ERROR("Failed to get output buffers for %s after INFO event (err=%d)",
                    mComponentName.c_str(), res);
            handleError(res);
            return false;
        }
        // DashPlayer ignores this
        return true;
    } else if (res == INFO_FORMAT_CHANGED) {
        sp<AMessage> format = new AMessage();
        res = mCodec->getOutputFormat(&format);
        if (res != OK) {
            DPD_MSG_ERROR("Failed to get output format for %s after INFO event (err=%d)",
                    mComponentName.c_str(), res);
            handleError(res);
            return false;
        }

        /* Computation of dpbSize

           #dpbSize = #output buffers
                      - 2 extrabuffers allocated by firmware
                      - minUndequeuedBufs (query from native window)
                      - 3 extrabuffers allocated by codec
           If extrabuffers allocated by firmware or ACodec changes,
           above eq. needs to be updated
        */

        int dpbSize = 0;
        if (mNativeWindow != NULL) {
             ANativeWindow *nativeWindow = mNativeWindow.get();
            if (nativeWindow != NULL) {
                int minUndequeuedBufs = 0;
                status_t err = nativeWindow->query(nativeWindow,
                    NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBufs);
                if (err == NO_ERROR) {
                    dpbSize = (mOutputBuffers.size() - minUndequeuedBufs - 5) > 0 ?
                        (mOutputBuffers.size() - minUndequeuedBufs - 5) : 0;
                    DPD_MSG_ERROR("[%s] computed DPB size of video stream = %d",
                        mComponentName.c_str(), dpbSize);
                }
            }
        }

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatOutputFormatChanged);
        notify->setMessage("format", format);
        notify->setInt32("dpb-size", dpbSize);
        notify->post();
        return true;
    } else if (res == INFO_DISCONTINUITY) {
        // nothing to do
        return true;
    } else if (res != OK) {
        if (res != -EAGAIN) {
            handleError(res);
        }
        return false;
    }

    // FIXME: This should be handled after rendering is complete,
    // but Renderer needs it now
    if (flags & MediaCodec::BUFFER_FLAG_EOS) {
        DPD_MSG_ERROR("queueing eos [%s]", mComponentName.c_str());

        status_t err;
        err = mCodec->releaseOutputBuffer(bufferIx);
        if (err != OK) {
            DPD_MSG_ERROR("failed to release output buffer for %s (err=%d)",
                 mComponentName.c_str(), err);
          handleError(err);
        }

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatEOS);
        notify->setInt32("err", ERROR_END_OF_STREAM);
        notify->post();
        return true;
    }

    CHECK_LT(bufferIx, mOutputBuffers.size());
    sp<ABuffer> buffer = mOutputBuffers[bufferIx];
    buffer->setRange(offset, size);

    sp<RefBase> obj;
    sp<GraphicBuffer> graphicBuffer;
    if (buffer->meta()->findObject("graphic-buffer", &obj)) {
        graphicBuffer = static_cast<GraphicBuffer*>(obj.get());
    }

    buffer->meta()->clear();
    buffer->meta()->setInt64("timeUs", timeUs);
    if (flags & MediaCodec::BUFFER_FLAG_EOS) {
        buffer->meta()->setInt32("eos", true);
    }
    // we do not expect CODECCONFIG or SYNCFRAME for decoder

    sp<AMessage> reply = new AMessage(kWhatRenderBuffer, this);
    reply->setSize("buffer-ix", bufferIx);
    reply->setInt32("generation", mBufferGeneration);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatDrainThisBuffer);
    if(flags & MediaCodec::BUFFER_FLAG_EXTRADATA) {
       buffer->meta()->setInt32("extradata", 1);
    }
    buffer->meta()->setObject("graphic-buffer", graphicBuffer);
    notify->setBuffer("buffer", buffer);
    notify->setMessage("reply", reply);
    notify->post();

    return true;
}

/** @brief: Give buffer to mediacodec for rendering
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::onRenderBuffer(const sp<AMessage> &msg) {
    status_t err;
    int32_t render;
    size_t bufferIx;
    CHECK(msg->findSize("buffer-ix", &bufferIx));
    if (msg->findInt32("render", &render) && render) {
        err = mCodec->renderOutputBufferAndRelease(bufferIx);
    } else {
        err = mCodec->releaseOutputBuffer(bufferIx);
    }
    if (err != OK) {
        DPD_MSG_ERROR("failed to release output buffer for %s (err=%d)",
                mComponentName.c_str(), err);
        handleError(err);
    }
    }

/** @brief: notify decoder flush complete to dashplayer
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::onFlush() {
    status_t err = OK;
    if (mCodec != NULL) {
        err = mCodec->flush();
        ++mBufferGeneration;
    }

    if (err != OK) {
        DPD_MSG_ERROR("failed to flush %s (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        // finish with posting kWhatFlushCompleted.
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFlushCompleted);
    notify->post();
}

/** @brief: notify decoder shutdown complete to dashplayer
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::onShutdown() {
    status_t err = OK;
    if (mCodec != NULL) {
        err = mCodec->release();
        mCodec = NULL;
        ++mBufferGeneration;

        if (mNativeWindow != NULL) {
            // reconnect to surface as MediaCodec disconnected from it
            status_t error =
                    native_window_api_connect(
                            mNativeWindow.get(),
                            NATIVE_WINDOW_API_MEDIA);
            ALOGW_IF(error != NO_ERROR,
                    "[%s] failed to connect to native window, error=%d",
                    mComponentName.c_str(), error);
        }
        mComponentName = "decoder";
    }

    if (err != OK) {
        DPD_MSG_ERROR("failed to release %s (err=%d)", mComponentName.c_str(), err);
        handleError(err);
        // finish with posting kWhatShutdownCompleted.
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatShutdownCompleted);
    notify->post();
}

/** @brief: message handler to handle dashplayer/mediacodec messages
 *
 *  @return: void
 *
 */
void DashPlayer::Decoder::onMessageReceived(const sp<AMessage> &msg) {
    DPD_MSG_HIGH("[%s] onMessage: %s", mComponentName.c_str(), msg->debugString().c_str());

    switch (msg->what()) {
        case kWhatConfigure:
        {
            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));
            onConfigure(format);
            break;
        }

        case kWhatCodecNotify:
        {
            if (!isStaleReply(msg)) {
                while (handleAnInputBuffer()) {
                }

                while (handleAnOutputBuffer()) {
                }
            }

            requestCodecNotification();
            break;
        }

        case kWhatInputBufferFilled:
        {
            if (!isStaleReply(msg)) {
                onInputBufferFilled(msg);
            }
            break;
        }

        case kWhatRenderBuffer:
        {
            if (!isStaleReply(msg)) {
                onRenderBuffer(msg);
            }
            break;
        }

        case kWhatFlush:
        {
            onFlush();
            break;
            }

        case kWhatShutdown:
        {
            onShutdown();
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

void DashPlayer::Decoder::signalFlush() {
    (new AMessage(kWhatFlush, this))->post();
}

void DashPlayer::Decoder::signalResume() {
    // nothing to do
}

void DashPlayer::Decoder::initiateShutdown() {
    (new AMessage(kWhatShutdown,this))->post();
}


/** @brief: convert input metadat into AMessage format
 *
 *  @return: input format value in AMessage
 *
 */
sp<AMessage> DashPlayer::Decoder::makeFormat(const sp<MetaData> &meta) {
    sp<AMessage> msg;
    CHECK_EQ(convertMetaDataToMessage(meta, &msg), (status_t)OK);
    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    if(!strncasecmp(mime, "video/", strlen("video/"))){
       msg->setInt32("max-height", MAX_HEIGHT);
       msg->setInt32("max-width", MAX_WIDTH);
       msg->setInt32("enable-extradata-user", 1);

       // Below property requie to set to prefer adaptive playback
       // msg->setInt32("prefer-adaptive-playback", 1);
    }

    return msg;
}
struct CCData
{
  CCData(uint8_t type, uint8_t data1, uint8_t data2)
      : mType(type), mData1(data1), mData2(data2)
  {}
  bool getChannel(size_t *channel) const
  {
    //The CL group contains the 32 addressable codes from 0x00 to 0x1F
    //Unused codes within the range of 0x00 to 0x0F shall be skipped
    //hence it 0x10 to 0x1F
    if (mData1 >= 0x10 && mData1 <= 0x1f)
    {
      *channel = (mData1 >= 0x18 ? 1 : 0) + (mType ? 2 : 0);
      return true;
    }
    return false;
  }
  uint8_t mType;
  uint8_t mData1;
  uint8_t mData2;
};


DashPlayer::CCDecoder::CCDecoder(const sp<AMessage> &notify)
    : mNotify(notify),
      mCurrentChannel(0),
      mSelectedTrack(-1)
{
  for (size_t i = 0; i < sizeof(mTrackIndices)/sizeof(mTrackIndices[0]); ++i)
  {
    mTrackIndices[i] = -1;
  }

  char property_value[PROPERTY_VALUE_MAX] = {0};
  property_get("persist.dash.debug.level", property_value, NULL);

  if(*property_value) {
      mLogLevel = atoi(property_value);
  }
}


static bool isNullPad(CCData *cc)
{
  //It is recommended that the padding byte pairs use values cc_data_1 = 0x00, cc_data_2 = 0x00
  return cc->mData1 < 0x10 && cc->mData2 < 0x10;
}

static void dumpBytePair(const sp<ABuffer> &ccBuf)
{
  size_t offset = 0;
  AString out;

  while (offset < ccBuf->size())
  {
    char tmp[128];

    CCData *cc = (CCData *) (ccBuf->data() + offset);

    if (isNullPad(cc)) {
        // 1 null pad or XDS metadata, ignore
        offset += sizeof(CCData);
        continue;
    }
    //The GL group contains the 96 addressable codes from 0x20 to 0x7F
    if (cc->mData1 >= 0x20 && cc->mData1 <= 0x7f)
    {
      // 2 basic chars
      snprintf(tmp, sizeof(tmp), "[%d]Basic: %c %c", cc->mType, cc->mData1, cc->mData2);
    }
    else if ((cc->mData1 == 0x11 || cc->mData1 == 0x19)
            && cc->mData2 >= 0x30 && cc->mData2 <= 0x3f)
    {
      // 1 special char
      snprintf(tmp, sizeof(tmp), "[%d]Special: %02x %02x", cc->mType, cc->mData1, cc->mData2);
    }
    else if ((cc->mData1 == 0x12 || cc->mData1 == 0x1A)
             && cc->mData2 >= 0x20 && cc->mData2 <= 0x3f)
    {
      // 1 Spanish/French char
      snprintf(tmp, sizeof(tmp), "[%d]Spanish: %02x %02x", cc->mType, cc->mData1, cc->mData2);
    }
    else if ((cc->mData1 == 0x13 || cc->mData1 == 0x1B)
             && cc->mData2 >= 0x20 && cc->mData2 <= 0x3f)
    {
      // 1 Portuguese/German/Danish char
      snprintf(tmp, sizeof(tmp), "[%d]German: %02x %02x", cc->mType, cc->mData1, cc->mData2);
    }
    else if ((cc->mData1 == 0x11 || cc->mData1 == 0x19)
             && cc->mData2 >= 0x20 && cc->mData2 <= 0x2f)
    {
      // Mid-Row Codes (Table 69)
      snprintf(tmp, sizeof(tmp), "[%d]Mid-row: %02x %02x", cc->mType, cc->mData1, cc->mData2);
    }
    else if (((cc->mData1 == 0x14 || cc->mData1 == 0x1c)
              && cc->mData2 >= 0x20 && cc->mData2 <= 0x2f)
              ||
               ((cc->mData1 == 0x17 || cc->mData1 == 0x1f)
              && cc->mData2 >= 0x21 && cc->mData2 <= 0x23))
    {
      // Misc Control Codes (Table 70)
      snprintf(tmp, sizeof(tmp), "[%d]Ctrl: %02x %02x", cc->mType, cc->mData1, cc->mData2);
    }
    else if ((cc->mData1 & 0x70) == 0x10
            && (cc->mData2 & 0x40) == 0x40
            && ((cc->mData1 & 0x07) || !(cc->mData2 & 0x20)) )
    {
      // Preamble Address Codes (Table 71)
      snprintf(tmp, sizeof(tmp), "[%d]PAC: %02x %02x", cc->mType, cc->mData1, cc->mData2);
    }
    else
    {
      snprintf(tmp, sizeof(tmp), "[%d]Invalid: %02x %02x", cc->mType, cc->mData1, cc->mData2);
    }
    if (out.size() > 0)
    {
      out.append(", ");
    }
    out.append(tmp);
    offset += sizeof(CCData);
  }

  ALOGV("%s", out.c_str());
}


size_t DashPlayer::CCDecoder::getTrackCount() const
{
  return mFoundChannels.size();
}

sp<AMessage> DashPlayer::CCDecoder::getTrackInfo(size_t index) const
{
  if (!isTrackValid(index))
  {
    DPD_MSG_ERROR("CCDecoder: getTrackInfo - NotValid track");
    return NULL;
  }

  sp<AMessage> format = new AMessage();


  format->setInt32("type", MEDIA_TRACK_TYPE_SUBTITLE);
  AString lang = "und";
  AString mimeType = MEDIA_MIMETYPE_TEXT_CEA_608;
  format->setString("language", lang);
  format->setString("mime", mimeType);
  //CC1, field 0 channel 0
  bool isDefaultAuto = (mFoundChannels[index] == 0);
  format->setInt32("auto", isDefaultAuto);
  format->setInt32("default", isDefaultAuto);
  format->setInt32("forced", 0);
  return format;
}

status_t DashPlayer::CCDecoder::selectTrack(size_t index, bool select)
{
  if (!isTrackValid(index))
  {
    DPD_MSG_ERROR("CCDecoder: selectTrack - NotValid track");
    return BAD_VALUE;
  }
  if (select)
  {
    if (mSelectedTrack == (ssize_t)index)
    {
      DPD_MSG_HIGH("CCDecoder: track %zu already selected", index);
      return BAD_VALUE;
    }
    DPD_MSG_HIGH("CCDecoder: selected track %zu", index);
    mSelectedTrack = index;
  }
  else
  {
    if (mSelectedTrack != (ssize_t)index)
    {
      DPD_MSG_ERROR("CCDecoder: track %zu is not selected", index);
      return BAD_VALUE;
    }
    DPD_MSG_HIGH("CCDecoder: unselected track %zu", index);
    mSelectedTrack = -1;
  }
  return OK;
}

int DashPlayer::CCDecoder::getSelectedTrack()
{
  return mSelectedTrack;
}

bool DashPlayer::CCDecoder::isSelected() const
{
  return mSelectedTrack >= 0 && mSelectedTrack < (int32_t) getTrackCount();
}

bool DashPlayer::CCDecoder::isTrackValid(size_t index) const
{
  return index < getTrackCount();
}

int32_t DashPlayer::CCDecoder::getTrackIndex(size_t channel) const
{
  if (channel < sizeof(mTrackIndices)/sizeof(mTrackIndices[0]))
  {
    return mTrackIndices[channel];
  }
  return -1;
}

bool  DashPlayer::CCDecoder::extractPictureUserData
  (OMX_U8 *pictureUserData, OMX_U32 pictureUserDataSize, int64_t mediaTimeUs)
{
  bool trackAdded = false;
  NALBitReader br(pictureUserData, pictureUserDataSize);
  uint8_t itu_t_t35_country_code = br.getBits(8);
  uint16_t itu_t_t35_provider_code = br.getBits(16);
  uint32_t user_identifier = br.getBits(32);
  uint8_t user_data_type_code = br.getBits(8);


  if (itu_t_t35_country_code == 0xB5
               && itu_t_t35_provider_code == 0x0031
               && user_identifier == 'GA94'
               && user_data_type_code == 0x3)
  {
    // MPEG_cc_data()
    // ATSC A/53 Part 4: 6.2.3.1
     br.skipBits(1); //process_em_data_flag
     bool process_cc_data_flag = br.getBits(1);
     br.skipBits(1); //additional_data_flag
     size_t cc_count = br.getBits(5);
     DPD_MSG_HIGH("CCDecoder: CEA CC cc_count : %d",cc_count);
     br.skipBits(8); // em_data;

     if (process_cc_data_flag)
     {
       sp<ABuffer> ccBuf = new ABuffer(cc_count * sizeof(CCData));
       ccBuf->setRange(0, 0);

       for (size_t i = 0; i < cc_count; i++)
       {
         uint8_t marker = br.getBits(5);
         CHECK_EQ(marker, 0x1f);
         bool cc_valid = br.getBits(1);
         uint8_t cc_type = br.getBits(2);
         //PrintCCTypeCombo(cc_valid, cc_type);
         uint8_t cc_data_1 = br.getBits(8) & 0x7f;
         uint8_t cc_data_2 = br.getBits(8) & 0x7f;
         DPD_MSG_HIGH("CCDecoder: Processing cc_data_pkt #: %d cc_data_1 0x%x cc_data_2 0x%x", i, cc_data_1, cc_data_2);
         //If field “x” Buffer is empty at the transmit time of NTSC field “x”, a CEA-608 waveform should be
         //generated for that field with cc_data_1 = 0x80 and cc_data_2 = 0x80
         //in CEA-608 notation, two 0x00s with odd parity,remove odd parity bit

         if (cc_valid && (cc_type == 0 || cc_type == 1))
         {
           CCData cc(cc_type, cc_data_1, cc_data_2);
           if (!isNullPad(&cc))
           {
             size_t channel; //setting to 1 as getChannel returns 0
             if (cc.getChannel(&channel) && getTrackIndex(channel) < 0)
             {
                  mTrackIndices[channel] = mFoundChannels.size();
                  mFoundChannels.push_back(channel);
                  trackAdded = true;
                  DPD_MSG_HIGH("CCDecoder: CEA TrackAdded successfully - channel %d index %d", channel,  mTrackIndices[channel]);
             }
             memcpy(ccBuf->data() + ccBuf->size(),(void *)&cc, sizeof(cc));
             ccBuf->setRange(0, ccBuf->size() + sizeof(CCData));
             mCCMap.add(mediaTimeUs, ccBuf);
           }
           else
           {
             DPD_MSG_HIGH("CCDecoder: CEA null pad %d", i);
           }
         }
         else
         {
            DPD_MSG_HIGH("CCDecoder: CEA-708 cc_valid %d cc_type %d", cc_valid, cc_type);
         }
       }
       DPD_MSG_HIGH("CCDecoder: mCCMap.add timeUs %lld ccBuf.size() %d", mediaTimeUs , ccBuf->size());
       //printmCCMap();
     }
  }
  else
  {
    DPD_MSG_ERROR("CCDecoder: Malformed SEI payload type 4");
  }
  return trackAdded;
}

sp<ABuffer> DashPlayer::CCDecoder::filterCCBuf(
        const sp<ABuffer> &ccBuf, size_t index)
{
  sp<ABuffer> filteredCCBuf = new ABuffer(ccBuf->size());
  filteredCCBuf->setRange(0, 0);
  size_t cc_count = ccBuf->size() / sizeof(CCData);
  const CCData* cc_data = (const CCData*)ccBuf->data();
  for (size_t i = 0; i < cc_count; ++i)
  {
    size_t channel;
    if (cc_data[i].getChannel(&channel))
    {
      mCurrentChannel = channel;
    }
    if (mCurrentChannel == mFoundChannels[index])
    {
      memcpy(filteredCCBuf->data() + filteredCCBuf->size(),
                  (void *)&cc_data[i], sizeof(CCData));
      filteredCCBuf->setRange(0, filteredCCBuf->size() + sizeof(CCData));
    }
  }
  return filteredCCBuf;
}

void DashPlayer::CCDecoder::decode(OMX_U8 *pictureUserData,
             OMX_U32 pictureUserDataSize, int64_t mediaTimeUs)
{
  if (extractPictureUserData(pictureUserData, pictureUserDataSize, mediaTimeUs))
  {
    DPD_MSG_ERROR("CCDecoder: Found CEA-608 track");
    if (mNotify != NULL)
    {
      sp<AMessage> msg = mNotify->dup();
      msg->setInt32("what", kWhatTrackAdded);
      msg->post();
    }
  }
}

void DashPlayer::CCDecoder::display(int64_t timeUs)
{
  if (!isTrackValid(mSelectedTrack))
  {
    DPD_MSG_ERROR("CCDecoder: display Could not find current track(index=%d)", mSelectedTrack);
    return;
  }

  ssize_t index = mCCMap.indexOfKey(timeUs);
  if (index < 0)
  {
    DPD_MSG_ERROR("CCDecoder: display cc for timestamp %lld not found", timeUs);
    return;
  }
  DPD_MSG_ERROR("CCDecoder: display found TS %lld at index %d", timeUs, index);
  //printmCCMap();
  sp<ABuffer> ccBuf = filterCCBuf(mCCMap.valueAt(index), mSelectedTrack);

  if (ccBuf != NULL && ccBuf->size() > 0)
  {
    dumpBytePair(ccBuf);
    ccBuf->meta()->setInt32("trackIndex", mSelectedTrack);
    ccBuf->meta()->setInt64("timeUs", timeUs);
    ccBuf->meta()->setInt64("durationUs", 0ll);
    if (mNotify != NULL)
    {
      sp<AMessage> msg = mNotify->dup();
      msg->setInt32("what", kWhatClosedCaptionData);
      msg->setBuffer("buffer", ccBuf);
      msg->post();
    }
  }
  else
  {
    DPD_MSG_ERROR("CCDecoder: ccBuf->size() is zero");
  }
  // remove all entries before timeUs
  mCCMap.removeItemsAt(0, index + 1);
}

void DashPlayer::CCDecoder::flush() {
    mCCMap.clear();
}

void DashPlayer::CCDecoder::PrintCCTypeCombo(bool cc_valid, uint8_t cc_type)
{
  DPD_MSG_MEDIUM("CCDecoder: cc_valid %d and cc_type %d",cc_valid, cc_type);
  if (cc_valid)
  {
    if(cc_type == 0 )
    {
      DPD_MSG_MEDIUM("CEA-608 line 21 field 1 CC bytes");
    }
    else if(cc_type == 1 )
    {
      DPD_MSG_MEDIUM("CEA-608 line 21 field 2 CC bytes");
    }
    else if(cc_type == 2 )
    {
      DPD_MSG_MEDIUM("Continuing CCP: cc_data_1/cc_data_2 CCP data");
    }
    else if(cc_type == 3 )
    {
      DPD_MSG_MEDIUM("start CCP: cc_data_1 CCP Header and cc_data_2 CCP data ");
    }
  }
  else
  {
    if(cc_type == 0 )
    {
      DPD_MSG_MEDIUM("CEA-608 line 21 field 1 - DTVCC padding bytes");
    }
    else if(cc_type == 1 )
    {
      DPD_MSG_MEDIUM("CEA-608 line 21 field 2 - DTVCC padding bytes");
    }
    else if(cc_type == 2 )
    {
      DPD_MSG_MEDIUM("DTV CC padding bytes");
    }
    else if(cc_type == 3 )
    {
      DPD_MSG_MEDIUM("DTV CC padding bytes");
    }
  }
}

void DashPlayer::CCDecoder::printmCCMap()
{
  DPD_MSG_HIGH("CCDecoder: printmCCMap size %d ", mCCMap.size());
  for (size_t i = 0; i < mCCMap.size(); ++i)
  {
    const sp<ABuffer> ccBuf =  mCCMap.valueAt(i);
    const int64_t timeUs = mCCMap.keyAt(i);
    DPD_MSG_HIGH("CCDecoder: CCMap[%d] ccBuf size %d and timeTs %lld", i, ccBuf->size(),timeUs );
  }
}


}  // namespace android

