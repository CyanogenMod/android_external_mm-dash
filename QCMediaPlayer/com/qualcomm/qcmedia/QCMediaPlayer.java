/*
 * Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 *  File: QCMediaPlayer.java
 *  Description: Snapdragon SDK for Android support class.
 *
 *
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
 *
 * Copyright (C) 2006 The Android Open Source Project
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

package com.qualcomm.qcmedia;

import android.media.MediaPlayer;
import android.util.Log;
import android.media.TimedText;
import java.lang.ref.WeakReference;
import com.qualcomm.qcmedia.QCTimedText;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.Parcel;
import java.util.HashSet;
import java.io.IOException;
import android.content.Context;
import android.net.Uri;
import java.util.Map;

/**
 * QCMediaPlayer extends MediaPlayer from package android.media
 * and provides extended APIs and interfaces to get and set MPD
 * attributes for DASH protocol in compatible Snapdragon builds.
 * {@hide}
 */

public class QCMediaPlayer extends MediaPlayer {
    private final static String TAG = "QCMediaPlayer";
    private QCMediaEventHandler mEventHandler;

    /**
     * Default constructor. Calls base class constructor.
     */
    public QCMediaPlayer() {
        super();
        Log.d(TAG, "constructor");
        Looper looper;
        if ((looper = Looper.myLooper()) != null) {
            mEventHandler = new QCMediaEventHandler(this, looper);
        } else if ((looper = Looper.getMainLooper()) != null) {
            mEventHandler = new QCMediaEventHandler(this, looper);
        } else {
            mEventHandler = null;
        }
    }

    /**
     * Sets listeners registered with qcmediaplayer to null. Calls
     * release on base class
     */
    public void release() {
        Log.d(TAG, "release");
        mQCOnPreparedListener = null;
        mOnMPDAttributeListener = null;
        mOnQCTimedTextListener = null;
        mOnQOEEventListener = null;
        super.release();
    }

    /* Do not change these values without updating their counterparts
     * in include/media/mediaplayer.h!
     */
    private static final int MEDIA_NOP = 0; // interface test message
    private static final int MEDIA_PREPARED = 1;
    private static final int MEDIA_PLAYBACK_COMPLETE = 2;
    private static final int MEDIA_BUFFERING_UPDATE = 3;
    private static final int MEDIA_SEEK_COMPLETE = 4;
    private static final int MEDIA_SET_VIDEO_SIZE = 5;
    private static final int MEDIA_TIMED_TEXT = 99;
    private static final int MEDIA_ERROR = 100;
    private static final int MEDIA_INFO = 200;
    private static final int MEDIA_QOE = 300;

    /* Do not change these values without updating their counterparts
     * in DashPlayer.h!
     */
    /**
     * Key to get MPD attributes
     */
    public static final int INVOKE_ID_GET_MPD_PROPERTIES = 8010;

    /**
     * Key to set MPD attributes
     */
    public static final int INVOKE_ID_SET_MPD_PROPERTIES = 8011;

    /**
     * Key to get whole MPD
     */
    public static final int INVOKE_ID_GET_MPD = 8003;

    /**
     * Key to identify type of QOE Event
     */
    public static final int ATTRIBUTES_QOE_EVENT_REG       = 8004;
    public static final int ATTRIBUTES_QOE_EVENT_PLAY      = 8005;
    public static final int ATTRIBUTES_QOE_EVENT_STOP      = 8006;
    public static final int ATTRIBUTES_QOE_EVENT_SWITCH    = 8007;
    public static final int ATTRIBUTES_QOE_EVENT_PERIODIC  = 8008;

    /**
     * Private keys for QOE events
     */
    private static final int QOEPlay = 1;
    private static final int QOEStop = 2;
    private static final int QOESwitch = 3;
    private static final int QOEPeriodic = 4;

    /**
     * Key to query timeshiftbuffer boundaries.
     */
    public static final int KEY_DASH_REPOSITION_RANGE = 9000;
    /**
     * Interal key to push blank frame to native window.
     */
    private static final int INVOKE_ID_PUSH_BLANK_FRAME = 9001;

    /**
     * Keys for dash playback modes used within timeshiftbuffer
     */
    public static final int KEY_DASH_SEEK_EVENT = 7001;
    public static final int KEY_DASH_PAUSE_EVENT = 7002;
    public static final int KEY_DASH_RESUME_EVENT = 7003;

    /**
     * Interface definition for a callback to be invoked when the media
     * source is ready for MPD attribute retrieval
     */
    public interface OnMPDAttributeListener
    {
        /**
        * Called when attributes are available.
        *
        * @param value the value for the attribute
        * @param mp the MediaPlayer to which MPD attribute is applicable
        *
        */
        public void onMPDAttribute(String value, QCMediaPlayer mp);
    }

    /**
     * Register a callback to be invoked when MPD attributes are avaialble
     *
     * @param listener the callback that will be run
     */
    public void setOnMPDAttributeListener(OnMPDAttributeListener listener)
    {
        mOnMPDAttributeListener = listener;
    }

    private OnMPDAttributeListener mOnMPDAttributeListener;

    /**
     * Register a callback to be invoked when the media source is
     * ready for playback.
     *
     * @param listener the callback that will be run
     */
    public void setOnPreparedListener(OnPreparedListener listener)
    {
        Log.d(TAG, "setOnPreparedListener");
        mQCOnPreparedListener = listener;
    }

    private OnPreparedListener mQCOnPreparedListener;

    /**
     * Interface definition of a callback to be invoked when a
     * QCtimedtext is available for display.
     */
    public interface OnQCTimedTextListener
    {
        /**
         * Called to indicate an avaliable timed text
         *
         * @param mp the QCMediaPlayer associated with this callback
         * @param text the Qtimed text sample which contains the text
         * needed to be displayed and the display format.
         */
        public void onQCTimedText(QCMediaPlayer mp, QCTimedText text);
    }

    /**
     * Register a callback to be invoked when a Qtimed text is available
     * for display.
     *
     * @param listener the callback that will be run
     */
    public void setOnQCTimedTextListener(OnQCTimedTextListener listener)
    {
        Log.d(TAG, "setOnQCTimedTextListener");
        mOnQCTimedTextListener = listener;
    }

    private OnQCTimedTextListener mOnQCTimedTextListener;

    /**
     * Interface definition for a callback to be invoked when the media
     * source is ready for QOE data retrieval.
     */
    public interface OnQOEEventListener
    {
        /**
         * Called when attributes are available.
         *
         * @param attributekey the key identifying the type of
         *        attributes available
         * @param value the value for the attribute
         * @param mp the MediaPlayer to which QOE event is applicable
         *
         */
        public void onQOEAttribute(int key, Parcel value,QCMediaPlayer mp);
    }

    /**
     * Register a callback to be invoked when QOE event happens
     * @param listener  the callback that will be run
     */
    public void setOnQOEEventListener(OnQOEEventListener listener)
    {
        mOnQOEEventListener = listener;
    }

    private OnQOEEventListener mOnQOEEventListener;

    private class QCMediaEventHandler extends Handler {
        private QCMediaPlayer mQCMediaPlayer;

        public QCMediaEventHandler(QCMediaPlayer mp, Looper looper) {
            super(looper);
            Log.d(TAG, "QCMediaEventHandler calling mp.mEventHandler.sendMessage()m");
            mQCMediaPlayer = mp;
        }

        @Override
        public void handleMessage(Message msg) {
            switch(msg.what) {
            case MEDIA_PREPARED:
                Log.d(TAG, "QCMediaEventHandler::handleMessage::MEDIA_PREPARED");
                if (mOnMPDAttributeListener != null) {
                    if (msg.obj instanceof Parcel) {
                        Parcel parcel = (Parcel)msg.obj;
                        mOnMPDAttributeListener.onMPDAttribute(parcel.readString(), mQCMediaPlayer);
                    }
                }
                if (mQCOnPreparedListener != null) {
                    mQCOnPreparedListener.onPrepared(mQCMediaPlayer);
                }
                return;

            case MEDIA_TIMED_TEXT:
                Log.d(TAG, "QCMediaEventHandler::handleMessage::MEDIA_TIMED_TEXT");
                if(mOnQCTimedTextListener != null) {
                    if (msg.obj instanceof Parcel) {
                        Parcel parcel = (Parcel)msg.obj;
                        QCTimedText text = new QCTimedText(parcel);
                        mOnQCTimedTextListener.onQCTimedText(mQCMediaPlayer, text);
                    }
                }
                return;

            case MEDIA_QOE:
                Log.d(TAG, "QCMediaEventHandler::handleMessage::MEDIA_QOE Received " + msg.arg2);
                if(mOnQOEEventListener != null) {
                    if (msg.obj instanceof Parcel) {
                        int key = 0;
                        Parcel parcel = (Parcel)msg.obj;
                        if(msg.arg2 == /*(int)QOEEvent.*/QOEPlay) {
                            key = ATTRIBUTES_QOE_EVENT_PLAY;
                        } else if(msg.arg2 == /*(int)QOEEvent.*/QOEPeriodic) {
                            key = ATTRIBUTES_QOE_EVENT_PERIODIC;
                        } else if(msg.arg2 == /*(int)QOEEvent.*/QOESwitch) {
                            key = ATTRIBUTES_QOE_EVENT_SWITCH;
                        } else if(msg.arg2 == /*(int)QOEEvent.*/QOEStop) {
                            key = ATTRIBUTES_QOE_EVENT_STOP;
                        }
                        mOnQOEEventListener.onQOEAttribute(key,parcel,mQCMediaPlayer);
                    }
                }
                return;

            default:
                Log.d(TAG, "QCMediaEventHandler::handleMessage unknown message type " + msg.what);
                return;
            }
        }
    }

    /**
     * Called from native code when an interesting event happens.  This method
     * just uses the EventHandler system to post the event back to the main app thread.
     * We use a weak reference to the original QCMediaPlayer object so that the native
     * code is safe from the object disappearing from underneath it.  (This is
     * the cookie passed to native_setup().)
     */
    private static void QCMediaPlayerNativeEventHandler(Object mediaplayer_ref,
                                                      int what, int arg1, int arg2, Object obj)
    {
        QCMediaPlayer mp = (QCMediaPlayer)((WeakReference)mediaplayer_ref).get();
        if (mp == null) {
           Log.d(TAG, "QCMediaPlayerNativeEventHandler mp == null");
           return;
        }
        if (mp.mEventHandler != null) {
           Message m = mp.mEventHandler.obtainMessage(what, arg1, arg2, obj);
           Log.d(TAG, "QCMediaPlayerNativeEventHandler calling mp.mEventHandler.sendMessage()");
           mp.mEventHandler.sendMessage(m);
        }
    }


    /**
     * Class for QCMediaPlayer to return TSB left, right boundaries and depth.
     *
     */
    static public class RepositionRangeInfo {
        public RepositionRangeInfo(Parcel in) {
            mLowerBoundary = in.readLong();
            mUpperBoundary = in.readLong();
            mDepth = in.readLong();
        }

        public RepositionRangeInfo() {
            mLowerBoundary = 0;
            mUpperBoundary = 0;
            mDepth = 0;
        }

        final long mLowerBoundary;
        final long mUpperBoundary;
        final long mDepth;

        /**
         * Gets the lower boundary.
         * @return lower boundary of the reposition range.
         */
        public long getLowerBoundary() {
            return mLowerBoundary;
        }

        /**
         * Gets the upper boundary.
         * @return upper boundary of the reposition range.
         */
        public long getUpperBoundary() {
            return mUpperBoundary;
        }

        /**
         * Gets the depth.
         * @return depth of the reposition range (i.e. upper boundary - lower boundary).
         */
        public long getDepth() {
            return mDepth;
        }
    };

    /**
     * Returns the version of QCMediaPlayer
     *
     * @return QCMediaPlayer version as a double
     */
    public double getPlayerVersion() {
        return 1.1;
    }

    /**
     * Returns the MPD properties xml as a string
     *
     * @return MPD properties xml as string
     */
    public String getMPDProperties() {
        Log.d(TAG, "getMPDProperties");
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(INVOKE_ID_GET_MPD_PROPERTIES);
            invoke(request, reply);
            String retval = reply.readString();
            return retval;
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    /**
     * Sets the selected MPD properties xml as a string
     *
     * @param selected MPD properties xml as a string
     * @return true if call was a success else false
     */
    public boolean setMPDProperties(String value) {
        Log.d(TAG, "setMPDProperties");
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(INVOKE_ID_SET_MPD_PROPERTIES);
            request.writeString(value);
            invoke(request, reply);
            boolean retval = reply.readInt() > 0 ? true : false;
            return retval;
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    /**
     * Returns the MPD as a string
     *
     * @return MPD as string
     */
    public String getMPD() {
        Log.d(TAG, "getMPD");
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(INVOKE_ID_GET_MPD);
            invoke(request, reply);
            String retval = reply.readString();
            return retval;
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    /**
     * Returns the timeshiftBuffer boundaries
     *
     * @return RepositionRangeInfo object
     */
    public RepositionRangeInfo getTSBRepositionRange() {
        Log.d(TAG, "getTSBRepositionRange");
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(KEY_DASH_REPOSITION_RANGE);
            invoke(request, reply);
            RepositionRangeInfo rangeInfo = new RepositionRangeInfo(reply);
            return rangeInfo;
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    /**
     * Tells mediaplayer to push blank frame to native window
     *
     * @return true if call was a success else false
     */
    public boolean pushBlankFrametoDisplay() {
        Log.d(TAG, "pushBlankFrametoDisplay");
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(INVOKE_ID_PUSH_BLANK_FRAME);
            invoke(request, reply);
            boolean retval = reply.readInt() > 0 ? true : false;
            return retval;
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    /**
     * Register for QOE events
     *
     * @return true if call was a success else false
     */
    public boolean regQOEEvents(boolean value) {
        Log.d(TAG, "regQOEEvents: " + value);
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(ATTRIBUTES_QOE_EVENT_REG);
            int regEvent = (value == true) ? 1 : 0;
            request.writeInt(regEvent);
            invoke(request, reply);
            boolean retval = reply.readInt() > 0 ? true : false;
            return retval;
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    /**
     * Returns QOE parameters
     *
     * @return parcel with QOE param values
     */
    public Parcel getQOEPeriodicParameter() {
        Log.d(TAG, "getQOEPeriodicParameter");
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(ATTRIBUTES_QOE_EVENT_PERIODIC);
            invoke(request, reply);
            return reply;
        } finally {
            request.recycle();
        }
    }

    /**
     * Sets the value for the corr. key on mediaplayer
     * Currently used only for pause/resume/seek events within
     * timeshiftbuffer boundaries. Caller recommended to use
     * pause(), resume() or seek() calls directly instead of
     * QCsetParameter. This api will be deprecated later.
     *
     * @param integer key
     * @return true if call was a success else false
     */
    public boolean QCsetParameter(int key, int value) {
        if(KEY_DASH_PAUSE_EVENT != key && KEY_DASH_RESUME_EVENT != key &&
                  KEY_DASH_SEEK_EVENT !=key) {
            Log.d(TAG, "QCsetParameter unsupported key "+key);
            return false;
        }
        Log.d(TAG, "QCsetParameter key " + key);
        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(key);
            request.writeInt(value);
            invoke(request, reply);
            boolean retval = reply.readInt() > 0 ? true : false;
            return retval;
        } finally {
            request.recycle();
            reply.recycle();
        }
    }

    /**
     * Returns parcel value for the corr. key.
     * Currently used only to get timeshiftbuffer boundaries. Caller
     * recommended to use getTSBRepositionRange() call instead. This
     * api will be deprecated later.
     *
     * @param integer key
     * @return true if call was a success else false
     */
    public Parcel QCgetParcelParameter(int key) {
        if(KEY_DASH_REPOSITION_RANGE != key) {
            Log.d(TAG, "QCgetParcelParameter unsupported key "+key);
            return null;
        }
        Log.d(TAG, "QCgetParcelParameter key " + key);

        Parcel request = newRequest();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInt(key);
            invoke(request, reply);
            return reply;
        } finally {
            request.recycle();
        }
    }
}
