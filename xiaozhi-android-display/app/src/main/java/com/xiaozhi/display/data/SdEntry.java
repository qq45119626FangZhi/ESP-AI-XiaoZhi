package com.xiaozhi.display.data;

/**
 * SD 卡里的一个条目（目录或文件）。path 为相对 /sdcard 的路径，不带前导斜杠。
 */
public class SdEntry {

    private final String name;
    private final String path;
    private final boolean dir;
    private final long size;

    public SdEntry(String name, String path, boolean dir, long size) {
        this.name = name;
        this.path = path;
        this.dir = dir;
        this.size = size;
    }

    public String getName() {
        return name;
    }

    public String getPath() {
        return path;
    }

    public boolean isDir() {
        return dir;
    }

    public long getSize() {
        return size;
    }

    /** 人类可读大小：1.2 MB */
    public String getSizeText() {
        if (dir) {
            return "";
        }
        double v = size;
        String[] unit = {"B", "KB", "MB", "GB"};
        int i = 0;
        while (v >= 1024.0 && i < unit.length - 1) {
            v /= 1024.0;
            i++;
        }
        if (i == 0) {
            return size + " B";
        }
        return String.format(java.util.Locale.US, "%.1f %s", v, unit[i]);
    }
}
