package com.xiaozhi.display.viewmodel;

/**
 * 一次性事件包装（Toast 等），避免旋转屏幕/重订阅时重复消费。
 */
public class Event<T> {

    private final T content;
    private boolean handled;

    public Event(T content) {
        this.content = content;
    }

    public T getContentIfNotHandled() {
        if (handled) {
            return null;
        }
        handled = true;
        return content;
    }

    public T peek() {
        return content;
    }
}
