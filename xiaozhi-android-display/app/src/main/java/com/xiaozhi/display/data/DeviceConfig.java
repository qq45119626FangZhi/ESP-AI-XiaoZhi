package com.xiaozhi.display.data;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * 板子地址配置（长按首页标题可修改，默认与网页版一致）。
 */
public final class DeviceConfig {

    private static final String PREF_NAME = "xiaozhi_display";
    private static final String KEY_BASE_URL = "base_url";

    /** 出厂默认地址（板子 IP 由路由器 DHCP 分配，换网络后需要改这里） */
    public static final String DEFAULT_BASE_URL = "http://10.62.113.139";

    private DeviceConfig() {
    }

    public static String getBaseUrl(Context context) {
        return normalize(prefs(context).getString(KEY_BASE_URL, DEFAULT_BASE_URL));
    }

    public static void setBaseUrl(Context context, String url) {
        prefs(context).edit().putString(KEY_BASE_URL, normalize(url)).apply();
    }

    /** 统一成 scheme://host[:port]，去掉末尾斜杠 */
    public static String normalize(String url) {
        if (url == null) {
            return DEFAULT_BASE_URL;
        }
        String s = url.trim();
        if (s.isEmpty()) {
            return DEFAULT_BASE_URL;
        }
        if (!s.startsWith("http://") && !s.startsWith("https://")) {
            s = "http://" + s;
        }
        while (s.endsWith("/")) {
            s = s.substring(0, s.length() - 1);
        }
        return s;
    }

    private static SharedPreferences prefs(Context context) {
        return context.getApplicationContext().getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
    }
}
