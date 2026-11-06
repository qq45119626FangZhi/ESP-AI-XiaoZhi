package com.xiaozhi.display.data;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * 板子 HTTP 接口封装（与网页版同一套）：
 *   GET /api/status              状态 JSON
 *   GET /api/mode?set=next       循环切模式（等价 SW2 单击）
 *   GET /api/list?dir=xxx        目录 JSON（新固件；旧固件回退解析 /list 的 HTML）
 *   GET /stream                  MJPEG 流
 *   GET /download?path=xxx       下载文件
 * 所有方法都是阻塞调用，必须在子线程执行。
 */
public final class XiaozhiApi {

    private static final int TIMEOUT_MS = 4000;

    private XiaozhiApi() {
    }

    public static String fetchStatus(String baseUrl) throws IOException {
        return httpGet(baseUrl + "/api/status");
    }

    /** 从 /api/status 的 JSON 里取当前模式（voice/doorbell/monitor） */
    public static String parseMode(String statusJson) {
        if (statusJson == null) {
            return "--";
        }
        try {
            return new JSONObject(statusJson).optString("mode", "--");
        } catch (Exception e) {
            return "--";
        }
    }

    public static boolean parseStreamActive(String statusJson) {
        if (statusJson == null) {
            return false;
        }
        try {
            return new JSONObject(statusJson).optBoolean("stream", false);
        } catch (Exception e) {
            return false;
        }
    }

    /** 循环切换模式：voice → doorbell → monitor → voice（与 SW2 单击一致） */
    public static void requestNextMode(String baseUrl) throws IOException {
        httpGet(baseUrl + "/api/mode?set=next");
    }

    /**
     * 读取目录列表。优先用 /api/list 的 JSON；旧固件没有该接口时回退解析 /list 的 HTML。
     */
    public static List<SdEntry> fetchList(String baseUrl, String dirRel) throws IOException {
        String dir = dirRel == null ? "" : dirRel;
        String url = baseUrl + "/api/list?dir=" + urlEncode(dir);
        String body;
        try {
            body = httpGet(url);
        } catch (IOException e) {
            // 404 说明固件还是旧版，退回 HTML 解析
            return fetchListFromHtml(baseUrl, dir);
        }
        try {
            return parseListJson(body, dir);
        } catch (Exception e) {
            return fetchListFromHtml(baseUrl, dir);
        }
    }

    private static List<SdEntry> parseListJson(String body, String dir) throws Exception {
        List<SdEntry> out = new ArrayList<>();
        JSONArray arr = new JSONArray(body);
        for (int i = 0; i < arr.length(); i++) {
            JSONObject o = arr.getJSONObject(i);
            String name = o.optString("name", "");
            if (name.isEmpty()) {
                continue;
            }
            String path = o.optString("path", joinPath(dir, name));
            boolean isDir = o.optBoolean("dir", false);
            long size = o.optLong("size", 0L);
            out.add(new SdEntry(name, path, isDir, size));
        }
        return out;
    }

    /**
     * 兼容旧固件的 HTML 目录页：只抓 <a href="/list?dir=..."> 与 <a href="/download?path=...">。
     */
    private static List<SdEntry> fetchListFromHtml(String baseUrl, String dir) throws IOException {
        String html = httpGet(baseUrl + "/list?dir=" + urlEncode(dir));
        List<SdEntry> out = new ArrayList<>();
        Pattern p = Pattern.compile("href=\"(/list\\?dir=|/download\\?path=)([^\"]*)\"[^>]*>([^<]*)<");
        Matcher m = p.matcher(html);
        while (m.find()) {
            boolean isDir = m.group(1).startsWith("/list");
            String target = urlDecode(m.group(2));
            String name = htmlUnescape(m.group(3)).trim();
            if (name.isEmpty()) {
                continue;
            }
            String path = isDir ? target : joinPath(dir, name);
            out.add(new SdEntry(name, path, isDir, 0L));
        }
        return out;
    }

    public static String downloadUrl(String baseUrl, String relPath) {
        return baseUrl + "/download?path=" + urlEncode(relPath);
    }

    private static String httpGet(String urlText) throws IOException {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(urlText).openConnection();
            conn.setConnectTimeout(TIMEOUT_MS);
            conn.setReadTimeout(TIMEOUT_MS);
            conn.setRequestMethod("GET");
            conn.setRequestProperty("Connection", "close");
            int code = conn.getResponseCode();
            if (code != HttpURLConnection.HTTP_OK) {
                throw new IOException("HTTP " + code);
            }
            try (InputStream in = conn.getInputStream()) {
                ByteArrayOutputStream bos = new ByteArrayOutputStream(8192);
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    bos.write(buf, 0, n);
                }
                return bos.toString("UTF-8");
            }
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private static String urlEncode(String s) {
        try {
            return URLEncoder.encode(s, "UTF-8");
        } catch (Exception e) {
            return s;
        }
    }

    private static String urlDecode(String s) {
        try {
            return java.net.URLDecoder.decode(s, "UTF-8");
        } catch (Exception e) {
            return s;
        }
    }

    private static String joinPath(String dir, String name) {
        if (dir == null || dir.isEmpty()) {
            return name;
        }
        return dir.endsWith("/") ? dir + name : dir + "/" + name;
    }

    private static String htmlUnescape(String s) {
        return s.replace("&amp;", "&")
                .replace("&lt;", "<")
                .replace("&gt;", ">")
                .replace("&quot;", "\"")
                .replace("&#39;", "'");
    }
}
