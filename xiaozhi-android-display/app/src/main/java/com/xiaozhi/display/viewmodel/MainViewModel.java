package com.xiaozhi.display.viewmodel;

import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

import com.xiaozhi.display.data.MjpegStream;
import com.xiaozhi.display.data.XiaozhiApi;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/**
 * 首页 ViewModel：状态轮询、循环切模式、MJPEG 帧分发。
 * 所有网络/解码动作都在子线程，UI 只通过 LiveData 拿到结果。
 */
public class MainViewModel extends ViewModel {

    private static final long POLL_INTERVAL_SEC = 2L;

    private final MutableLiveData<String> mode = new MutableLiveData<>("--");
    private final MutableLiveData<Boolean> online = new MutableLiveData<>(false);
    private final MutableLiveData<Boolean> streaming = new MutableLiveData<>(false);
    private final MutableLiveData<Bitmap> frame = new MutableLiveData<>();
    private final MutableLiveData<Event<String>> toast = new MutableLiveData<>();
    /** 状态栏文字：连接状态 + 已解码帧数，便于现场判断拉流是否真的在出帧 */
    private final MutableLiveData<String> streamInfo = new MutableLiveData<>("未连接");
    private int frameCount;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService actionExecutor = Executors.newSingleThreadExecutor();

    private MjpegStream mjpeg;
    private ScheduledExecutorService poller;
    private volatile boolean active;
    private volatile String baseUrl = "";

    public LiveData<String> getMode() {
        return mode;
    }

    public LiveData<Boolean> getOnline() {
        return online;
    }

    public LiveData<Boolean> getStreaming() {
        return streaming;
    }

    public LiveData<Bitmap> getFrame() {
        return frame;
    }

    public LiveData<Event<String>> getToast() {
        return toast;
    }

    public LiveData<String> getStreamInfo() {
        return streamInfo;
    }

    /** Activity.onStart 调用 */
    public void start(String baseUrl) {
        if (active) {
            return;
        }
        active = true;
        this.baseUrl = baseUrl;

        mjpeg = new MjpegStream();
        mjpeg.start(baseUrl, streamListener);

        poller = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "status-poll");
            t.setDaemon(true);
            return t;
        });
        poller.scheduleWithFixedDelay(this::pollStatus, 0, POLL_INTERVAL_SEC, TimeUnit.SECONDS);
    }

    /** Activity.onStop 调用：停流、停轮询（ViewModel 本身仍存活） */
    public void stop() {
        if (!active) {
            return;
        }
        active = false;
        if (poller != null) {
            poller.shutdownNow();
            poller = null;
        }
        if (mjpeg != null) {
            mjpeg.stop();
            mjpeg = null;
        }
        streaming.setValue(false);
    }

    /** 长按标题改地址后重建连接 */
    public void rebase(String newBaseUrl) {
        boolean wasActive = active;
        stop();
        baseUrl = newBaseUrl;
        if (wasActive) {
            start(newBaseUrl);
        }
    }

    /** 底部「切换模式」：和 SW2 单击一样循环切换 voice → doorbell → monitor */
    public void switchNextMode() {
        final String base = baseUrl;
        actionExecutor.execute(() -> {
            try {
                XiaozhiApi.requestNextMode(base);
                pollStatus();
            } catch (Exception e) {
                postToast("切换失败：" + e.getMessage());
            }
        });
    }

    private void pollStatus() {
        if (!active) {
            return;
        }
        try {
            String body = XiaozhiApi.fetchStatus(baseUrl);
            String m = XiaozhiApi.parseMode(body);
            mode.postValue(m);
            online.postValue(true);
        } catch (Exception e) {
            online.postValue(false);
        }
    }

    private final MjpegStream.Listener streamListener = new MjpegStream.Listener() {
        @Override
        public void onFrame(Bitmap bitmap) {
            if (active) {
                frameCount++;
                frame.setValue(bitmap);
                streamInfo.setValue("推流中 · " + frameCount + " 帧");
            }
        }

        @Override
        public void onStreaming(boolean isStreaming) {
            if (active) {
                streaming.setValue(isStreaming);
                streamInfo.setValue(isStreaming ? "已连上：等待首帧…"
                                                : (frameCount > 0 ? "已断开（共 " + frameCount + " 帧）" : "未推流"));
            }
        }

        @Override
        public void onError(String message) {
            if (active) {
                streamInfo.setValue("未推流：" + message);
            }
        }
    };

    private void postToast(String text) {
        mainHandler.post(() -> toast.setValue(new Event<>(text)));
    }

    @Override
    protected void onCleared() {
        stop();
        actionExecutor.shutdownNow();
        super.onCleared();
    }
}
