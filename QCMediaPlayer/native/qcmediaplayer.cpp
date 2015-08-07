/*Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "NativeQCMediaPlayer"

#include "qcmediaplayer.h"

namespace android {

extern "C" MediaPlayer* CreateQCMediaPlayer() {
    return new QCMediaPlayer();
}

QCMediaPlayer::QCMediaPlayer() {
    ALOGV("constructor");
    mDashPlayback = false;
}

QCMediaPlayer::~QCMediaPlayer() {
    ALOGV("destructor");
}

void QCMediaPlayer::notify(int msg, int ext1, int ext2, const Parcel *obj)
{
    ALOGV("message received msg=%d, ext1=%d, ext2=%d", msg, ext1, ext2);
    bool locked = false;
    if (mLockThreadId != getThreadId()) {
        mLock.lock();
        locked = true;
    }

    // Allows calls from JNI in idle state to notify errors
    if (!((msg == MEDIA_ERROR || msg == MEDIA_QOE) && mCurrentState == MEDIA_PLAYER_IDLE) && mPlayer == 0) {
        ALOGV("notify(%d, %d, %d) callback on disconnected mediaplayer", msg, ext1, ext2);
        if (locked) mLock.unlock();   // release the lock when done.
        return;
    }

    if (locked) mLock.unlock();
    MediaPlayer::notify(msg, ext1, ext2, obj);
}



status_t QCMediaPlayer::setDataSource(
        const sp<IMediaHTTPService> &httpService,
        const char *url, const KeyedVector<String8, String8> *headers) {
    ALOGV("setDataSource(%s)", url);
    if (url != NULL) {
        if (!strncasecmp("http://", url, 7)) {
            size_t len = strlen(url);
            if (len >= 5 && !strncasecmp(".mpd", &url[len - 4], 4)) {
                mDashPlayback = true;
            }
        }
    }

    return MediaPlayer::setDataSource(httpService, url, headers);
}


status_t QCMediaPlayer::pause() {
    ALOGV("pause");
    if(mDashPlayback) {
        Mutex::Autolock _l(mLock);
        if (mCurrentState & MEDIA_PLAYER_PAUSED)
            return NO_ERROR;
        if ((mPlayer != 0) && (mCurrentState &
                    (MEDIA_PLAYER_STARTED | MEDIA_PLAYER_PLAYBACK_COMPLETE))) {
            status_t ret = mPlayer->pause();
            if (ret != NO_ERROR) {
                mCurrentState = MEDIA_PLAYER_STATE_ERROR;
            } else {
                mCurrentState = MEDIA_PLAYER_PAUSED;
            }
            return ret;
        }
        ALOGE("pause called in state %d", mCurrentState);
        return INVALID_OPERATION;
    } else {
        return MediaPlayer::pause();
    }
}


status_t QCMediaPlayer::seekTo(int msec) {
    ALOGV("seek");
    if(mDashPlayback) {
        mLockThreadId = getThreadId();
        Mutex::Autolock _l(mLock);

        status_t result = NO_ERROR;

        if ((mPlayer != 0) && ( mCurrentState & ( MEDIA_PLAYER_STARTED |
                    MEDIA_PLAYER_PREPARED | MEDIA_PLAYER_PAUSED |
                    MEDIA_PLAYER_PLAYBACK_COMPLETE) ) ) {
            if ( msec < 0 ) {
                ALOGW("Attempt to seek to invalid position: %d", msec);
                msec = 0;
            }

            int durationMs;
            result = mPlayer->getDuration(&durationMs);

            if (result != OK) {
                ALOGW("Stream has no duration and is therefore not seekable.");
            } else {
                //When timeshiftbuffer(tsb) is present for a live dash clip
                //seek is allowed with the tsb boundaries. Since getDuration()
                //returns 0 for live clip below check valid only if duration > 0
                if (durationMs > 0 && msec > durationMs) {
                    ALOGW("Attempt to seek to past end of file: request = %d, "
                          "durationMs = %d",
                          msec,
                          durationMs);

                    msec = durationMs;
                }

                // cache duration
                mCurrentPosition = msec;
                if (mSeekPosition < 0) {
                    mSeekPosition = msec;
                    result = mPlayer->seekTo(msec);
                } else {
                    ALOGV("Seek in progress - queue up seekTo[%d]", msec);
                    result = NO_ERROR;
                }
            }
        } else {
            ALOGE("Attempt to perform seekTo in wrong state: mPlayer=%p,"
                  "mCurrentState=%u", mPlayer.get(), mCurrentState);
            result = INVALID_OPERATION;
        }

        mLockThreadId = 0;
        return result;

    } else {
        return MediaPlayer::seekTo(msec);
    }
}

}; // namespace android
