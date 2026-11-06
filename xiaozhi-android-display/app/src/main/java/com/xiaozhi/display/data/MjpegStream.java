package com.xiaozhi.display.data;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * MJPEG 拉流客户端：整个读流/解 JPEG 过程都在独立子线程，
 * 解码出的 Bitmap 通过主线程 Handler 回调，绝不阻塞 UI 线程。
 *
 * 协议：GET /stream -> multipart/x-mixed-replace; boundary=frame
 * 每帧带 Content-Length，直接按长度读满再 decode。
 */
public class MjpegStream {

    public interface Listener {
        /** 主线程回调 */
        void onFrame(Bitmap bitmap);

        /** 主线程回调：是否已连上流 */
        void onStreaming(boolean streaming);

        /** 主线程回调：断流/异常提示 */
        void onError(String message);
    }

    private static final int CONNECT_TIMEOUT_MS = 4000;
    private static final int READ_TIMEOUT_MS = 10000;
    private static final int MAX_FRAME_BYTES = 4 * 1024 * 1024;
    private static final int RECONNECT_DELAY_MS = 1500;
    private static final String TAG = "MjpegStream";

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private volatile boolean running;
    private Thread worker;

    /* 解码缓冲复用（双 Bitmap 乒乓）。
     *
     * 为什么是两张而不是一张：ImageView.setImageBitmap() 之后，这张 Bitmap 仍被
     * ImageView 持有并在绘制。解码线程若把下一帧直接解进同一块像素内存，UI 线程
     * 可能正好在画它 -> 撕裂 / 半帧 / 花屏。交替两张后，某张要隔 2 帧才会被再次复用，
     * 那时 ImageView 早已换成另一张（LiveData 也只保留最新值），因此是安全的。
     *
     * 复用条件（BitmapFactory 的 inBitmap 规则）：Bitmap 必须 mutable，
     * 且新图所需字节数不超过复用图。分辨率切换时若不满足会抛 IllegalArgumentException，
     * 已在 decodeAndPost 里兜底退回普通解码。 */
    private final Bitmap[] framePool = new Bitmap[2];
    private int framePoolIdx = 0;

    public boolean isRunning() {
        return running;
    }

    public void start(final String baseUrl, final Listener listener) {
        stop();
        running = true;
        worker = new Thread(() -> loop(baseUrl, listener), "mjpeg-stream");
        worker.setDaemon(true);
        worker.start();
    }

    public void stop() {
        running = false;
        Thread t = worker;
        worker = null;
        if (t != null) {
            t.interrupt();
        }
    }

    private void loop(String baseUrl, Listener listener) {
        while (running) {
            HttpURLConnection conn = null;
            try {
                conn = (HttpURLConnection) new URL(baseUrl + "/stream").openConnection();
                conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
                conn.setReadTimeout(READ_TIMEOUT_MS);
                conn.setRequestProperty("Connection", "close");
                int code = conn.getResponseCode();
                Log.i(TAG, "GET /stream -> " + code);
                if (code == 503) {
                    // 语音/门铃模式下板子不推流，属于正常状态，静默重试
                    postError(listener, "设备不在监控模式");
                    postStreaming(listener, false);
                } else if (code != HttpURLConnection.HTTP_OK) {
                    throw new IOException("HTTP " + code);
                } else {
                    String delimiter = parseBoundary(conn.getContentType());
                    Log.i(TAG, "connected, contentType=" + conn.getContentType() + ", delim=" + delimiter);
                    postStreaming(listener, true);
                    try (InputStream in = new BufferedInputStream(conn.getInputStream(), 64 * 1024)) {
                        readFrames(in, delimiter, listener);
                    }
                    postStreaming(listener, false);
                }
            } catch (Exception e) {
                if (running) {
                    postError(listener, "拉流失败：" + e.getMessage());
                }
                postStreaming(listener, false);
            } finally {
                if (conn != null) {
                    conn.disconnect();
                }
            }
            if (!sleep(RECONNECT_DELAY_MS)) {
                return;
            }
        }
    }

    /** 从 Content-Type 取 boundary，拼成 "--frame" 形式 */
    private String parseBoundary(String contentType) {
        String boundary = "frame";
        if (contentType != null) {
            int idx = contentType.toLowerCase().indexOf("boundary=");
            if (idx >= 0) {
                String v = contentType.substring(idx + "boundary=".length()).trim();
                int semi = v.indexOf(';');
                if (semi >= 0) {
                    v = v.substring(0, semi);
                }
                v = v.replace("\"", "").trim();
                if (!v.isEmpty()) {
                    boundary = v;
                }
            }
        }
        return "--" + boundary;
    }

    /**
     * 直接从字节流里按 JPEG 帧头帧尾（FFD8 … FFD9）切帧，不再依赖 multipart 的结构。
     *
     * 为什么不用边界 + Content-Length 解析：
     *   板子用 esp_http_server 的 httpd_resp_send_chunk() 推流，该接口在 IDF v5.3 里
     *   硬编码 "Transfer-Encoding: chunked"（httpd_txrx.c:323），无法改成 Content-Length。
     *   不同客户端对 "chunked + multipart + 每帧 Content-Length" 的处理差异很大，
     *   按文本边界解析时一旦多/少吃掉一个 CRLF 就整帧报废（表现为连上了但永远等不到首帧），
     *   而浏览器容错强所以网页看起来正常。
     *
     * 为什么 FFD9 可以作为帧尾：JPEG 熵编码段里出现的 0xFF 都做过字节填充（FF00），
     *   FFD9 只会出现在帧的真正结尾，所以按 SOI/EOI 切分是可靠的。
     *
     * @param delimiter 仅用于日志/兼容旧签名，切帧不依赖它
     */
    private void readFrames(InputStream in, String delimiter, Listener listener) throws IOException {
        final int STATE_IDLE = 0;
        final int STATE_IN_FRAME = 1;
        final int MAX_FRAME = MAX_FRAME_BYTES;

        int state = STATE_IDLE;
        int prev = -1;
        int frameCount = 0;
        ByteArrayOutputStream bos = new ByteArrayOutputStream(64 * 1024);
        int b;

        Log.i(TAG, "start parsing jpeg stream (delim=" + delimiter + ")");

        while (running && (b = in.read()) != -1) {
            if (state == STATE_IDLE) {
                /* 找 SOI：FF D8（其后必须紧跟 FF，避免误判） */
                if (prev == 0xFF && b == 0xD8) {
                    bos.reset();
                    bos.write(0xFF);
                    bos.write(0xD8);
                    state = STATE_IN_FRAME;
                    prev = -1;
                } else {
                    prev = b;
                }
                continue;
            }

            bos.write(b);
            if (bos.size() > MAX_FRAME) {
                Log.w(TAG, "frame too large, drop and resync");
                bos.reset();
                state = STATE_IDLE;
                prev = -1;
                continue;
            }

            /* 找 EOI：FF D9 */
            if (prev == 0xFF && b == 0xD9) {
                byte[] jpeg = bos.toByteArray();
                bos.reset();
                state = STATE_IDLE;
                prev = -1;
                if (jpeg.length > 4) {
                    frameCount++;
                    if (frameCount <= 3) {
                        Log.i(TAG, "frame #" + frameCount + " len=" + jpeg.length);
                    }
                    decodeAndPost(jpeg, listener);
                }
                continue;
            }
            prev = b;
        }

        Log.i(TAG, "stream eof after frames=" + frameCount + " (bytes parsed)");
    }

    private void decodeAndPost(byte[] jpeg, Listener listener) {
        /* 兜底：从 JPEG 的 SOI(FFD8) 开始解，跳过前面可能的垃圾字节 */
        int start = indexOfSoi(jpeg);
        if (start < 0) {
            Log.w(TAG, "no JPEG SOI in frame, len=" + jpeg.length);
            return;
        }

        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inMutable = true; /* inBitmap 生效的前提 */
        Bitmap reuse = framePool[framePoolIdx];
        if (reuse != null && !reuse.isRecycled()) {
            opts.inBitmap = reuse;
        }

        Bitmap decoded;
        try {
            decoded = BitmapFactory.decodeByteArray(jpeg, start, jpeg.length - start, opts);
        } catch (IllegalArgumentException e) {
            /* 分辨率/配置变了，inBitmap 不兼容：退回普通解码，这一帧会重新分配 */
            Log.w(TAG, "inBitmap rejected (" + e.getMessage() + "), decode without reuse");
            opts.inBitmap = null;
            decoded = BitmapFactory.decodeByteArray(jpeg, start, jpeg.length - start, opts);
        }
        if (decoded == null) {
            Log.w(TAG, "decode failed, len=" + jpeg.length + ", soi=" + start);
            return;
        }

        /* 存回池子并切到另一张：本次这张交给 UI，下一帧解到另一张 */
        final Bitmap out = decoded;
        boolean reused = (opts.inBitmap != null);
        framePool[framePoolIdx] = out;
        framePoolIdx ^= 1;
        if (!reused) {
            Log.i(TAG, "frame buffer allocated new (" + out.getWidth() + "x" + out.getHeight() + ")");
        }

        mainHandler.post(() -> {
            if (running) {
                listener.onFrame(out);
            }
        });
    }

    /** 返回 JPEG 起始标记 SOI 的下标，找不到返回 -1。
     *  注意字节顺序是 0xFF,0xD8（FF D8），不能写反，否则会定位到 D8 FF 上导致解码失败。 */
    private static int indexOfSoi(byte[] data) {
        for (int i = 0; i + 1 < data.length; i++) {
            if ((data[i] & 0xFF) == 0xFF && (data[i + 1] & 0xFF) == 0xD8) {
                return i;
            }
        }
        return -1;
    }

    private boolean sleep(long ms) {
        try {
            Thread.sleep(ms);
            return true;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    private void postStreaming(Listener listener, boolean streaming) {
        mainHandler.post(() -> {
            if (running) {
                listener.onStreaming(streaming);
            }
        });
    }

    private void postError(Listener listener, String message) {
        mainHandler.post(() -> {
            if (running) {
                listener.onError(message);
            }
        });
    }
}
