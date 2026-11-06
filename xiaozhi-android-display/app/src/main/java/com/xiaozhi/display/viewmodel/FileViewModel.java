package com.xiaozhi.display.viewmodel;

import android.os.Handler;
import android.os.Looper;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

import com.xiaozhi.display.data.SdEntry;
import com.xiaozhi.display.data.XiaozhiApi;

import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * SD 文件浏览 ViewModel：进入某目录时在子线程拉取列表。
 */
public class FileViewModel extends ViewModel {

    private final MutableLiveData<List<SdEntry>> entries = new MutableLiveData<>();
    private final MutableLiveData<String> currentDir = new MutableLiveData<>("");
    private final MutableLiveData<Boolean> loading = new MutableLiveData<>(false);
    private final MutableLiveData<Event<String>> toast = new MutableLiveData<>();

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private volatile int generation;

    public LiveData<List<SdEntry>> getEntries() {
        return entries;
    }

    public LiveData<String> getCurrentDir() {
        return currentDir;
    }

    public LiveData<Boolean> getLoading() {
        return loading;
    }

    public LiveData<Event<String>> getToast() {
        return toast;
    }

    public void load(String baseUrl, String dir) {
        final int gen = ++generation;
        final String target = dir == null ? "" : dir;
        loading.postValue(true);
        executor.execute(() -> {
            try {
                List<SdEntry> list = XiaozhiApi.fetchList(baseUrl, target);
                if (gen != generation) {
                    return;
                }
                currentDir.postValue(target);
                entries.postValue(list);
            } catch (Exception e) {
                if (gen != generation) {
                    return;
                }
                entries.postValue(java.util.Collections.emptyList());
                mainHandler.post(() -> toast.setValue(new Event<>("读取目录失败：" + e.getMessage())));
            } finally {
                if (gen == generation) {
                    loading.postValue(false);
                }
            }
        });
    }

    @Override
    protected void onCleared() {
        executor.shutdownNow();
        super.onCleared();
    }
}
