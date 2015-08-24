/*
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "DashPlayerRenderer"

#include "DashPlayerRenderer.h"
#include <cutils/properties.h>
#include <utils/Log.h>

#define DPR_MSG_ERROR(...) ALOGE(__VA_ARGS__)
#define DPR_MSG_HIGH(...) if(mLogLevel >= 1){ALOGE(__VA_ARGS__);}
#define DPR_MSG_MEDIUM(...) if(mLogLevel >= 2){ALOGE(__VA_ARGS__);}
#define DPR_MSG_LOW(...) if(mLogLevel >= 3){ALOGE(__VA_ARGS__);}

namespace android {

// static
const int64_t DashPlayer::Renderer::kMinPositionUpdateDelayUs = 100000ll;

DashPlayer::Renderer::Renderer(
        const sp<MediaPlayerBase::AudioSink> &sink,
        const sp<AMessage> &notify)
    : mAudioSink(sink),
      mNotify(notify),
      mNumFramesWritten(0),
      mDrainAudioQueuePending(false),
      mDrainVideoQueuePending(false),
      mDrainVideoQueuePendingUntilFirstAudio(false),
      mAudioQueueGeneration(0),
      mVideoQueueGeneration(0),
      mAnchorTimeMediaUs(-1),
      mAnchorTimeRealUs(-1),
      mSeekTimeUs(0),
      mFlushingAudio(false),
      mFlushingVideo(false),
      mHasAudio(false),
      mHasVideo(false),
      mSyncQueues(false),
      mPaused(false),
      mWasPaused(false),
      mLastPositionUpdateUs(-1ll),
      mVideoLateByUs(0ll),
      mStats(NULL),
      mLogLevel(0),
      mLastReceivedVideoSampleUs(-1),
      mDelayPending(false),
      mDelayToQueueUs(0),
      mDelayToQueueTimeRealUs(0),
      mIsLiveStream(false),
      mStartUpLatencyBeginUs(-1),
      mStartUpLatencyUs(0),
      mDiscFromAnchorRealTimeRefresh(false) {

      mAVSyncDelayWindowUs = 40000;

      char avSyncDelayMsec[PROPERTY_VALUE_MAX] = {0};
      property_get("persist.dash.avsync.window.msec", avSyncDelayMsec, NULL);

      if(*avSyncDelayMsec) {
          int64_t avSyncDelayWindowUs = atoi(avSyncDelayMsec) * 1000;

          if(avSyncDelayWindowUs > 0) {
             mAVSyncDelayWindowUs = avSyncDelayWindowUs;
          }
      }

      DPR_MSG_LOW("AVsync window in Us %lld", mAVSyncDelayWindowUs);

      char property_value[PROPERTY_VALUE_MAX] = {0};
      property_get("persist.dash.debug.level", property_value, NULL);

      if(*property_value) {
          mLogLevel = atoi(property_value);
      }
}

DashPlayer::Renderer::~Renderer() {
    if(mStats != NULL) {
        mStats->logStatistics();
        mStats->logSyncLoss();
        mStats = NULL;
    }
}

void DashPlayer::Renderer::queueBuffer(
        bool audio,
        const sp<ABuffer> &buffer,
        const sp<AMessage> &notifyConsumed) {
    sp<AMessage> msg = new AMessage(kWhatQueueBuffer, this);
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->setBuffer("buffer", buffer);
    msg->setMessage("notifyConsumed", notifyConsumed);
    msg->post();
}

void DashPlayer::Renderer::queueEOS(bool audio, status_t finalResult) {
    CHECK_NE(finalResult, (status_t)OK);

    if(mSyncQueues)
      syncQueuesDone();

    sp<AMessage> msg = new AMessage(kWhatQueueEOS, this);
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->setInt32("finalResult", finalResult);
    msg->post();
}

void DashPlayer::Renderer::queueDelay(int64_t delayUs) {
    Mutex::Autolock autoLock(mDelayLock);
    if (mDelayPending) {
        // Earlier posted delay still processing.
        mDelayToQueueUs = delayUs;
        mDelayToQueueTimeRealUs = ALooper::GetNowUs();
        DPR_MSG_HIGH("queueDelay Delay already queued earlier. Cache this delay %lld msecs and queue later",
                               mDelayToQueueUs/1000);
        return;
    }

    // Pause audio sink
    if (mHasAudio) {
        mAudioSink->pause();
    }

    DPR_MSG_ERROR("queueDelay delay introduced in rendering %lld msecs", delayUs/1000);

    (new AMessage(kWhatDelayQueued, this))->post(delayUs);
    mDelayPending = true;
    mDelayToQueueUs = 0;
}

void DashPlayer::Renderer::flush(bool audio) {
    {
        Mutex::Autolock autoLock(mFlushLock);
        if (audio) {
            CHECK(!mFlushingAudio);
            mFlushingAudio = true;
        } else {
            CHECK(!mFlushingVideo);
            mFlushingVideo = true;
        }
    }

    sp<AMessage> msg = new AMessage(kWhatFlush, this);
    msg->setInt32("audio", static_cast<int32_t>(audio));
    msg->post();
}

void DashPlayer::Renderer::signalTimeDiscontinuity() {
    CHECK(mAudioQueue.empty());
    CHECK(mVideoQueue.empty());
    mAnchorTimeMediaUs = -1;
    mAnchorTimeRealUs = -1;
    mRealTimeOffsetUs = 0;
    mWasPaused = false;
    mSeekTimeUs = 0;
    mSyncQueues = mHasAudio && mHasVideo;
    mHasAudio = false;
    mHasVideo = false;
    mLastReceivedVideoSampleUs = -1;
    mDrainVideoQueuePendingUntilFirstAudio = false;
    mStartUpLatencyBeginUs = -1;
    mStartUpLatencyUs = 0;
    mDiscFromAnchorRealTimeRefresh = false;
    DPR_MSG_HIGH("signalTimeDiscontinuity mHasAudio %d mHasVideo %d mSyncQueues %d",mHasAudio,mHasVideo,mSyncQueues);
}

void DashPlayer::Renderer::pause() {
    (new AMessage(kWhatPause, this))->post();
}

void DashPlayer::Renderer::resume() {
    (new AMessage(kWhatResume, this))->post();
}


void DashPlayer::Renderer::signalRefreshAnchorRealTime(bool bAddStartUpLatency) {
    Mutex::Autolock autoLock(mRefreshAnchorTimeLock);

    if (mAnchorTimeMediaUs > -1 && mAnchorTimeRealUs > -1) {
        int64_t oldAnchorTimeRealUs = mAnchorTimeRealUs;

        mAnchorTimeMediaUs = mLastRenderedTimeMediaUs;
        mAnchorTimeRealUs = ALooper::GetNowUs() + mRealTimeOffsetUs;
        if (bAddStartUpLatency) {
            mAnchorTimeRealUs += mStartUpLatencyUs;
        }

        mDiscFromAnchorRealTimeRefresh = true;

        DPR_MSG_HIGH("signalRefreshAnchorRealTime mAnchorTimeMediaUs=%.3f "
              "OLD mAnchorTimeRealUs=%.3f NEW mAnchorTimeRealUs=%.3f "
              "mRealTimeOffsetUs=%.3f mStartUpLatencyUs=%.3f",
                (double)mAnchorTimeMediaUs/1E6,
                (double)oldAnchorTimeRealUs/1E6,
                (double)mAnchorTimeRealUs/1E6,
                (double)mRealTimeOffsetUs/1E6,
                (double)mStartUpLatencyUs/1E6);
    }
}


void DashPlayer::Renderer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatDrainAudioQueue:
        {
            mDrainAudioQueuePending = false;

            int32_t generation = -1;
            if (!(msg->findInt32("generation", &generation)) || (generation != mAudioQueueGeneration)) {
                DPR_MSG_ERROR( "onMessageReceived - kWhatDrainAudioQueue: generation %d mAudioQueueGeneration %d",
                               generation, mAudioQueueGeneration );
                break;
            }

            bool postAudio = onDrainAudioQueue();

            if (mDrainVideoQueuePendingUntilFirstAudio) {
                mDrainVideoQueuePendingUntilFirstAudio = false;
                postDrainVideoQueue();
            }

            if (postAudio) {
                uint32_t numFramesPlayed;
                CHECK_EQ(mAudioSink->getPosition(&numFramesPlayed),
                         (status_t)OK);

                uint32_t numFramesPendingPlayout =
                    mNumFramesWritten - numFramesPlayed;

                // This is how long the audio sink will have data to
                // play back.
                int64_t delayUs =
                    (int64_t)(mAudioSink->msecsPerFrame()
                        * (float)(numFramesPendingPlayout * 1000ll));

                // Let's give it more data after about half that time
                // has elapsed.
                postDrainAudioQueue(delayUs / 2);
            }

            break;
        }

        case kWhatDrainVideoQueue:
        {
            mDrainVideoQueuePending = false;

            int32_t generation = -1;
            if (!(msg->findInt32("generation", &generation)) || (generation != mVideoQueueGeneration)) {
                DPR_MSG_ERROR( "onMessageReceived - kWhatDrainVideoQueue: generation %d mVideoQueueGeneration %d",
                               generation, mVideoQueueGeneration );
                break;
            }

            onDrainVideoQueue();

            postDrainVideoQueue();
            break;
        }

        case kWhatQueueBuffer:
        {
            onQueueBuffer(msg);
            break;
        }

        case kWhatQueueEOS:
        {
            onQueueEOS(msg);
            break;
        }

        case kWhatFlush:
        {
            onFlush(msg);
            break;
        }

        case kWhatAudioSinkChanged:
        {
            onAudioSinkChanged();
            break;
        }

        case kWhatPause:
        {
            onPause();
            break;
        }

        case kWhatResume:
        {
            onResume();
            break;
        }

        case kWhatDelayQueued:
        {
            onDelayQueued();
            break;
        }

        default:
            TRESPASS();
            break;
    }
}


void DashPlayer::Renderer::onDelayQueued() {
    DPR_MSG_HIGH("onDelayQueued resume rendering");

    int64_t delayToQueueUs;
    int64_t delayToQueueTimeRealUs;
    {
        Mutex::Autolock autoLock(mDelayLock);
        mDelayPending = false;
        delayToQueueUs = mDelayToQueueUs;
        delayToQueueTimeRealUs = mDelayToQueueTimeRealUs;
    }

    if (delayToQueueUs > 0) {
        // This is to handle back to delay delays queued which first delay
        // is already in progress. Compute the net elapsed time from when
        // the second delay was posted and queueDelay with the elapsed time
        int64_t delayUs = delayToQueueUs - (ALooper::GetNowUs() - delayToQueueTimeRealUs);
        if (delayUs > 0) {
            DPR_MSG_HIGH("onDelayQueued delay was posted again mDelayToQueueUs %lld msecs. Calling queueDelay()",
                               delayToQueueUs/1000);
            queueDelay(delayUs);
            return;
        }
    }

    if(mHasAudio && !mPaused) {
        mAudioSink->start();
    }
    postDrainAudioQueue();
    postDrainVideoQueue();
}

void DashPlayer::Renderer::postDrainAudioQueue(int64_t delayUs) {
    if (mDelayPending || mDrainAudioQueuePending || mSyncQueues || mPaused) {
        return;
    }

    if (mAudioQueue.empty()) {
        return;
    }

    mDrainAudioQueuePending = true;
    sp<AMessage> msg = new AMessage(kWhatDrainAudioQueue, this);
    msg->setInt32("generation", mAudioQueueGeneration);
    msg->post(delayUs);
}

void DashPlayer::Renderer::signalAudioSinkChanged() {
    (new AMessage(kWhatAudioSinkChanged, this))->post();
}

bool DashPlayer::Renderer::onDrainAudioQueue() {
    if(mDelayPending) {
        return false;
    }

    if (mStartUpLatencyUs == 0 && mStartUpLatencyBeginUs >= 0) {
        mStartUpLatencyUs = ALooper::GetNowUs() - mStartUpLatencyBeginUs;
        DPR_MSG_HIGH("mStartUpLatencyUs computed %lld msecs", (int64_t)mStartUpLatencyUs/1000);
    }

    uint32_t numFramesPlayed;

    // Check if first frame is EOS, process EOS and return
    if(1 == mAudioQueue.size())
    {
       QueueEntry *entry = &*mAudioQueue.begin();
       if (entry->mBuffer == NULL) {
        DPR_MSG_ERROR("onDrainAudioQueue process EOS");
        notifyEOS(true /* audio */, entry->mFinalResult);

        mAudioQueue.erase(mAudioQueue.begin());
        entry = NULL;
        return false;
      }
    }

    if (mAudioSink->getPosition(&numFramesPlayed) != OK) {
        return false;
    }

    ssize_t numFramesAvailableToWrite =
        mAudioSink->frameCount() - (mNumFramesWritten - numFramesPlayed);

    size_t numBytesAvailableToWrite =
        numFramesAvailableToWrite * mAudioSink->frameSize();

    while (numBytesAvailableToWrite > 0 && !mAudioQueue.empty()) {
        QueueEntry *entry = &*mAudioQueue.begin();

        if (entry->mBuffer == NULL) {
            // EOS

            notifyEOS(true /* audio */, entry->mFinalResult);

            mAudioQueue.erase(mAudioQueue.begin());
            entry = NULL;
            return false;
        }

        if (entry->mOffset == 0) {
            int64_t mediaTimeUs;
            CHECK(entry->mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));

            if (mIsLiveStream && mHasVideo) {
                if (mAnchorTimeRealUs < 0) {
                    // This is first audio sample at start (or) after seek.
                    // If audio mediaTimeUs does not match mAnchorTimeMediaUs
                    // it means video mediaTimeUs matches. Return on onDrainAudioQueue
                    // so that video will set mAnchorTimeRealUs
                    if (mAnchorTimeMediaUs != mediaTimeUs) {
                        return true;
                    }
                }

                if (mediaTimeUs < mLastReceivedVideoSampleUs) {
                    DPR_MSG_ERROR("dropping late by audio. "
                      "media time %.2f secs < last received video media time %.2f secs",
                      (double)mediaTimeUs/1E6, (double)mLastReceivedVideoSampleUs/1E6);
                    entry->mNotifyConsumed->post();
                    mAudioQueue.erase(mAudioQueue.begin());
                    entry = NULL;
                    continue;
                }

                int32_t disc;
                if (((mDiscFromAnchorRealTimeRefresh) || (entry->mBuffer->meta()->findInt32("disc", &disc) && disc == 1))
                        && (mAnchorTimeMediaUs > 0) && (mAnchorTimeRealUs > 0)) {
                    int64_t realTimeUs = (mediaTimeUs - mAnchorTimeMediaUs) + mAnchorTimeRealUs;
                    int64_t delayUs = realTimeUs - ALooper::GetNowUs();

                    delayUs -= (mAudioSink->latency()*1000/2);

                    DPR_MSG_ERROR("onDrainAudioQueue SAMPLE EARLY CHECK. mediaTimeUs=%.3f mAnchorTimeMediaUs=%.3f"
                              " realTimeUs=%.3f mAnchorTimeRealUs=%.3f sinkLatency=%.3f delayUs=%.3f",
                                (double)mediaTimeUs/1E6,
                                (double)mAnchorTimeMediaUs/1E6,
                                (double)ALooper::GetNowUs()/1E6,
                                (double)mAnchorTimeRealUs/1E6,
                                (double)mAudioSink->latency(),
                                (double)delayUs/1E6);

                    mDiscFromAnchorRealTimeRefresh = false;

                    if (delayUs > 0) {
                        entry->mBuffer->meta()->setInt32("disc", 0);
                        mAnchorTimeMediaUs = mediaTimeUs;
                        mAnchorTimeRealUs = realTimeUs;
                        postDrainAudioQueue(delayUs);
                        return false;
                    }
                }
            }

            DPR_MSG_ERROR("rendering audio at media time %.2f secs", (double)mediaTimeUs / 1E6);

            mAnchorTimeMediaUs = mediaTimeUs;
            mLastRenderedTimeMediaUs = mAnchorTimeMediaUs;

            uint32_t numFramesPlayed;
            CHECK_EQ(mAudioSink->getPosition(&numFramesPlayed), (status_t)OK);

            uint32_t numFramesPendingPlayout =
                mNumFramesWritten - numFramesPlayed;

            mRealTimeOffsetUs =
                (int64_t)(((float)mAudioSink->latency() / 2
                    + (float)numFramesPendingPlayout
                        * mAudioSink->msecsPerFrame()) * 1000ll);

            mAnchorTimeRealUs =
                ALooper::GetNowUs() + mRealTimeOffsetUs;

            DPR_MSG_HIGH("onDrainAudioQueue mediaTimeUs %lld us mAnchorTimeMediaUs %lld us mAnchorTimeRealUs %lld us",
             mediaTimeUs, mAnchorTimeMediaUs, mAnchorTimeRealUs);
        }

        size_t copy = entry->mBuffer->size() - entry->mOffset;
        if (copy > numBytesAvailableToWrite) {
            copy = numBytesAvailableToWrite;
        }

        CHECK_EQ(mAudioSink->write(
                    entry->mBuffer->data() + entry->mOffset, copy),
                 (ssize_t)copy);

        entry->mOffset += copy;
        if (entry->mOffset == entry->mBuffer->size()) {
            entry->mNotifyConsumed->post();
            mAudioQueue.erase(mAudioQueue.begin());

            entry = NULL;
        }

        numBytesAvailableToWrite -= copy;
        size_t copiedFrames = copy / mAudioSink->frameSize();
        mNumFramesWritten += (uint32_t)copiedFrames;
    }

    notifyPosition();

    return !mAudioQueue.empty();
}

void DashPlayer::Renderer::postDrainVideoQueue() {
    if (mDelayPending || mDrainVideoQueuePending || mSyncQueues || mPaused || mDrainVideoQueuePendingUntilFirstAudio) {
        return;
    }

    if (mVideoQueue.empty()) {
        return;
    }

    QueueEntry &entry = *mVideoQueue.begin();

    sp<AMessage> msg = new AMessage(kWhatDrainVideoQueue, this);
    msg->setInt32("generation", mVideoQueueGeneration);

    int64_t delayUs;

    if (entry.mBuffer == NULL) {
        // EOS doesn't carry a timestamp.
        delayUs = 0;
    } else {
        int64_t mediaTimeUs;
        CHECK(entry.mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));

        if (mAnchorTimeMediaUs < 0 || mAnchorTimeRealUs < 0) {
            delayUs = 0;
            mAnchorTimeMediaUs = mediaTimeUs;
            mAnchorTimeRealUs = ALooper::GetNowUs();
        } else {
            if (mWasPaused) {
                mWasPaused = false;
                if (!mHasAudio) {
                    mAnchorTimeMediaUs = mediaTimeUs;
                    mAnchorTimeRealUs = ALooper::GetNowUs();
                } else if (!mAudioQueue.empty()) {
                    if (!mDrainVideoQueuePendingUntilFirstAudio) {
                        mDrainVideoQueuePendingUntilFirstAudio = true;
                    }
                    return;
                }
            }

            int64_t realTimeUs =
                (mediaTimeUs - mAnchorTimeMediaUs) + mAnchorTimeRealUs;

            delayUs = realTimeUs - ALooper::GetNowUs();
            if (delayUs > 0) {
                DPR_MSG_HIGH("postDrainVideoQueue video early by %.2f secs", (double)delayUs / 1E6);
            }
        }
    }

    msg->post(delayUs);

    mDrainVideoQueuePending = true;
}

void DashPlayer::Renderer::onDrainVideoQueue() {
    if(mDelayPending) {
        return;
    }

    if (mStartUpLatencyUs == 0 && mStartUpLatencyBeginUs >= 0) {
        mStartUpLatencyUs = ALooper::GetNowUs() - mStartUpLatencyBeginUs;
    }

    if (mVideoQueue.empty()) {
        return;
    }

    QueueEntry *entry = &*mVideoQueue.begin();

    if (entry->mBuffer == NULL) {
        // EOS

        notifyPosition(true);

        notifyEOS(false /* audio */, entry->mFinalResult);

        mVideoQueue.erase(mVideoQueue.begin());
        entry = NULL;

        mVideoLateByUs = 0ll;

        return;
    }

    int64_t mediaTimeUs;
    CHECK(entry->mBuffer->meta()->findInt64("timeUs", &mediaTimeUs));
    mLastReceivedVideoSampleUs = mediaTimeUs;

    int64_t realTimeUs = mediaTimeUs - mAnchorTimeMediaUs + mAnchorTimeRealUs;
    int64_t nowUs = ALooper::GetNowUs();
    mVideoLateByUs = nowUs - realTimeUs;

    DPR_MSG_HIGH("onDrainVideoQueue mediaTimeUs %lld us mAnchorTimeMediaUs %lld us mAnchorTimeRealUs %lld us",
             mediaTimeUs, mAnchorTimeMediaUs, mAnchorTimeRealUs);

    bool tooLate = (mVideoLateByUs > mAVSyncDelayWindowUs);

    if (tooLate && (!mHasAudio || (mediaTimeUs > mAnchorTimeMediaUs)))
    {
        DPR_MSG_HIGH("video only - resetting anchortime");
        mAnchorTimeMediaUs = mediaTimeUs;
        mAnchorTimeRealUs = ALooper::GetNowUs();
        tooLate = false;
    }

    if (tooLate) {
        DPR_MSG_ERROR("video late by %lld us (%.2f secs)",
             mVideoLateByUs, (double)mVideoLateByUs / 1E6);
        if(mStats != NULL) {
            mStats->recordLate(realTimeUs,nowUs,mVideoLateByUs,mAnchorTimeRealUs);
        }
    } else {
        DPR_MSG_ERROR("rendering video at media time %.2f secs", (double)mediaTimeUs / 1E6);

        if(mStats != NULL) {
            mStats->recordOnTime(realTimeUs,nowUs,mVideoLateByUs);
            mStats->incrementTotalRenderingFrames();
            mStats->logFps();
        }
        mLastRenderedTimeMediaUs = mediaTimeUs;
        mRealTimeOffsetUs = 0;
    }

    entry->mNotifyConsumed->setInt32("render", !tooLate);
    entry->mNotifyConsumed->post();
    mVideoQueue.erase(mVideoQueue.begin());
    entry = NULL;

    notifyPosition();
}

void DashPlayer::Renderer::notifyEOS(bool audio, status_t finalResult) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatEOS);
    notify->setInt32("audio", static_cast<int32_t>(audio));
    notify->setInt32("finalResult", finalResult);
    notify->post();
}

void DashPlayer::Renderer::onQueueBuffer(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    if (audio) {
        mHasAudio = true;
    } else {
        mHasVideo = true;
    }

    if (dropBufferWhileFlushing(audio, msg)) {
        return;
    }

    sp<ABuffer> buffer;
    CHECK(msg->findBuffer("buffer", &buffer));

    sp<AMessage> notifyConsumed;
    CHECK(msg->findMessage("notifyConsumed", &notifyConsumed));

    QueueEntry entry;
    entry.mBuffer = buffer;
    entry.mNotifyConsumed = notifyConsumed;
    entry.mOffset = 0;
    entry.mFinalResult = OK;

    int64_t mediaTimeUs;
    (buffer->meta())->findInt64("timeUs", &mediaTimeUs);

    if (mStartUpLatencyBeginUs < 0) {
        mStartUpLatencyBeginUs = ALooper::GetNowUs();
    }

    if (audio) {
        mAudioQueue.push_back(entry);

        if (mHasVideo && mAnchorTimeMediaUs < 0) {
            if (mVideoQueue.size() < 2) {
                DPR_MSG_HIGH("Not rendering audio Sample with TS: %lld  until first two video frames are received", mediaTimeUs);
                return;
            }

            setStartAnchorMediaAndPostDrainQueue();
            return;
        }

        postDrainAudioQueue();
        return;
    } else {
        mVideoQueue.push_back(entry);

        if (mHasAudio && mAnchorTimeMediaUs < 0) {
            if (mAudioQueue.size() == 0) {
                DPR_MSG_HIGH("Not rendering video Sample with TS: %lld  until first audio sample is received", mediaTimeUs);
                return;
            }

            if (mVideoQueue.size() < 2) {
                DPR_MSG_HIGH("Not rendering video Sample with TS: %lld  until first two video frames are received", mediaTimeUs);
                return;
            }

            setStartAnchorMediaAndPostDrainQueue();
            return;
        }

        postDrainVideoQueue();
    }

    if (!mSyncQueues || mAudioQueue.empty() || mVideoQueue.empty()) {
        return;
    }

    sp<ABuffer> firstAudioBuffer = (*mAudioQueue.begin()).mBuffer;
    sp<ABuffer> firstVideoBuffer = (*mVideoQueue.begin()).mBuffer;

    if (firstAudioBuffer == NULL || firstVideoBuffer == NULL) {
        // EOS signalled on either queue.
        syncQueuesDone();
        return;
    }

    int64_t firstAudioTimeUs;
    int64_t firstVideoTimeUs;
    CHECK(firstAudioBuffer->meta()
            ->findInt64("timeUs", &firstAudioTimeUs));
    CHECK(firstVideoBuffer->meta()
            ->findInt64("timeUs", &firstVideoTimeUs));

    int64_t diff = firstVideoTimeUs - firstAudioTimeUs;

    DPR_MSG_LOW("queueDiff = %.2f secs", (double)diff / 1E6);

    if (diff > 100000ll) {
        // Audio data starts More than 0.1 secs before video.
        // Drop some audio.

        (*mAudioQueue.begin()).mNotifyConsumed->post();
        mAudioQueue.erase(mAudioQueue.begin());
        return;
    }

    syncQueuesDone();
}

void DashPlayer::Renderer::setStartAnchorMediaAndPostDrainQueue() {
    int64_t firstVideoTimeUs = -1;
    sp<ABuffer> firstVideoBuffer = (*mVideoQueue.begin()).mBuffer;
    firstVideoBuffer->meta()->findInt64("timeUs", &firstVideoTimeUs);

    int64_t firstAudioTimeUs = -1;
    sp<ABuffer> firstAudioBuffer = (*mAudioQueue.begin()).mBuffer;
    firstAudioBuffer->meta()->findInt64("timeUs", &firstAudioTimeUs);

    if (firstAudioTimeUs >= 0 && firstVideoTimeUs >= 0) {
        mAnchorTimeMediaUs = (firstAudioTimeUs <= firstVideoTimeUs) ? firstAudioTimeUs : firstVideoTimeUs;
        DPR_MSG_HIGH("Both audio and video received. Start rendering");
    } else if (firstAudioTimeUs >= 0) {
        mAnchorTimeMediaUs = firstAudioTimeUs;
    } else if(firstVideoTimeUs >= 0) {
        mAnchorTimeMediaUs = firstVideoTimeUs;
    }

    mDrainVideoQueuePendingUntilFirstAudio = true;
    postDrainAudioQueue();
}

void DashPlayer::Renderer::syncQueuesDone() {
    if (!mSyncQueues) {
        return;
    }

    mSyncQueues = false;

    if (!mAudioQueue.empty()) {
        postDrainAudioQueue();
    }

    if (!mVideoQueue.empty()) {
        postDrainVideoQueue();
    }
}

void DashPlayer::Renderer::onQueueEOS(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    if (dropBufferWhileFlushing(audio, msg)) {
        return;
    }

    int32_t finalResult;
    CHECK(msg->findInt32("finalResult", &finalResult));

    QueueEntry entry;
    entry.mOffset = 0;
    entry.mFinalResult = finalResult;

    if (audio) {
        mAudioQueue.push_back(entry);
        postDrainAudioQueue();
    } else {
        mVideoQueue.push_back(entry);
        postDrainVideoQueue();
    }
}

void DashPlayer::Renderer::onFlush(const sp<AMessage> &msg) {
    int32_t audio;
    CHECK(msg->findInt32("audio", &audio));

    // If we're currently syncing the queues, i.e. dropping audio while
    // aligning the first audio/video buffer times and only one of the
    // two queues has data, we may starve that queue by not requesting
    // more buffers from the decoder. If the other source then encounters
    // a discontinuity that leads to flushing, we'll never find the
    // corresponding discontinuity on the other queue.
    // Therefore we'll stop syncing the queues if at least one of them
    // is flushed.
    syncQueuesDone();

    if (audio) {
        flushQueue(&mAudioQueue);

        Mutex::Autolock autoLock(mFlushLock);
        mFlushingAudio = false;

        mDrainAudioQueuePending = false;
        ++mAudioQueueGeneration;
    } else {
        flushQueue(&mVideoQueue);

        Mutex::Autolock autoLock(mFlushLock);
        mFlushingVideo = false;

        mDrainVideoQueuePending = false;
        ++mVideoQueueGeneration;
        if(mStats != NULL) {
            mStats->setVeryFirstFrame(true);
        }
    }

    notifyFlushComplete(audio);
}

void DashPlayer::Renderer::flushQueue(List<QueueEntry> *queue) {
    while (!queue->empty()) {
        QueueEntry *entry = &*queue->begin();

        if (entry->mBuffer != NULL) {
            entry->mNotifyConsumed->post();
        }

        queue->erase(queue->begin());
        entry = NULL;
    }
}

void DashPlayer::Renderer::notifyFlushComplete(bool audio) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFlushComplete);
    notify->setInt32("audio", static_cast<int32_t>(audio));
    notify->post();
}

bool DashPlayer::Renderer::dropBufferWhileFlushing(
        bool audio, const sp<AMessage> &msg) {
    bool flushing = false;

    {
        Mutex::Autolock autoLock(mFlushLock);
        if (audio) {
            flushing = mFlushingAudio;
        } else {
            flushing = mFlushingVideo;
        }
    }

    if (!flushing) {
        return false;
    }

    sp<AMessage> notifyConsumed;
    if (msg->findMessage("notifyConsumed", &notifyConsumed)) {
        notifyConsumed->post();
    }

    return true;
}

void DashPlayer::Renderer::onAudioSinkChanged() {
    CHECK(!mDrainAudioQueuePending);
    mNumFramesWritten = 0;
    uint32_t written;
    if (mAudioSink->getFramesWritten(&written) == OK) {
        mNumFramesWritten = written;
    }
}

void DashPlayer::Renderer::notifyPosition(bool isEOS) {
    if (mAnchorTimeRealUs < 0 || mAnchorTimeMediaUs < 0) {
        return;
    }

    int64_t nowUs = ALooper::GetNowUs();

    if ((!isEOS) && (mLastPositionUpdateUs >= 0
            && nowUs < mLastPositionUpdateUs + kMinPositionUpdateDelayUs)) {
        return;
    }
    mLastPositionUpdateUs = nowUs;

    int64_t positionUs = (mSeekTimeUs != 0) ? mSeekTimeUs : ((nowUs - mAnchorTimeRealUs) + mAnchorTimeMediaUs);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatPosition);
    notify->setInt64("positionUs", positionUs);
    notify->setInt64("videoLateByUs", mVideoLateByUs);
    notify->post();
}

void DashPlayer::Renderer::notifySeekPosition(int64_t seekTime){
  mSeekTimeUs = seekTime;
  int64_t nowUs = ALooper::GetNowUs();
  mLastPositionUpdateUs = nowUs;
  sp<AMessage> notify = mNotify->dup();
  notify->setInt32("what", kWhatPosition);
  notify->setInt64("positionUs", seekTime);
  notify->setInt64("videoLateByUs", mVideoLateByUs);
  notify->post();

}


void DashPlayer::Renderer::onPause() {
    CHECK(!mPaused);

    mDrainAudioQueuePending = false;
    ++mAudioQueueGeneration;

    mDrainVideoQueuePending = false;
    ++mVideoQueueGeneration;

    if (mHasAudio) {
        mAudioSink->pause();
    }

    DPR_MSG_LOW("now paused audio queue has %d entries, video has %d entries",
          mAudioQueue.size(), mVideoQueue.size());

    mPaused = true;
    mWasPaused = true;

    if(mStats != NULL) {
        int64_t positionUs;
        if(mAnchorTimeRealUs < 0 || mAnchorTimeMediaUs < 0) {
            positionUs = -1000;
        } else {
            positionUs = (ALooper::GetNowUs() - mAnchorTimeRealUs) + mAnchorTimeMediaUs;
        }

        mStats->logPause(positionUs);
    }
}

void DashPlayer::Renderer::onResume() {
    if (!mPaused) {
        return;
    }

    if (mHasAudio && !mDelayPending) {
        mAudioSink->start();
    }

    mPaused = false;

    if (mIsLiveStream) {
        signalRefreshAnchorRealTime(false);
    }

    if (!mAudioQueue.empty()) {
        postDrainAudioQueue();
    }

    if (!mVideoQueue.empty()) {
        postDrainVideoQueue();
    }
}

void DashPlayer::Renderer::registerStats(sp<DashPlayerStats> stats) {
    if(mStats != NULL) {
        mStats = NULL;
    }
    mStats = stats;
}

status_t DashPlayer::Renderer::setMediaPresence(bool audio, bool bValue)
{
   if (audio)
   {
      DPR_MSG_LOW("mHasAudio set to %d from %d",bValue,mHasAudio);
      mHasAudio = bValue;
   }
   else
   {
     DPR_MSG_LOW("mHasVideo set to %d from %d",bValue,mHasVideo);
     mHasVideo = bValue;
   }
   return OK;
}

void DashPlayer::Renderer::setLiveStream(bool bLiveStream) {
    DPR_MSG_HIGH("mIsLiveStream set to %d", bLiveStream);
    mIsLiveStream = bLiveStream;
}

}  // namespace android

