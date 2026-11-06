package com.xiaozhi.display.ui;

import android.Manifest;
import android.app.DownloadManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.view.View;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.lifecycle.ViewModelProvider;
import androidx.recyclerview.widget.LinearLayoutManager;

import com.xiaozhi.display.R;
import com.xiaozhi.display.data.DeviceConfig;
import com.xiaozhi.display.data.SdEntry;
import com.xiaozhi.display.data.XiaozhiApi;
import com.xiaozhi.display.databinding.ActivityFileBrowserBinding;
import com.xiaozhi.display.viewmodel.Event;
import com.xiaozhi.display.viewmodel.FileViewModel;

import java.util.List;

/**
 * 二级页面：浏览板子 SD 卡目录。
 * 目录可点击进入下一级，「上一级」或系统返回键回到父目录；文件点击用系统下载器保存到下载目录。
 */
public class FileBrowserActivity extends AppCompatActivity {

    private static final int REQ_STORAGE = 1001;

    private ActivityFileBrowserBinding binding;
    private FileViewModel viewModel;
    private FileAdapter adapter;
    private String baseUrl;
    private SdEntry pendingDownload;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        binding = ActivityFileBrowserBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        baseUrl = DeviceConfig.getBaseUrl(this);
        viewModel = new ViewModelProvider(this).get(FileViewModel.class);

        adapter = new FileAdapter(this::onEntryClick);
        binding.rvFiles.setLayoutManager(new LinearLayoutManager(this));
        binding.rvFiles.setAdapter(adapter);

        binding.btnUp.setOnClickListener(v -> goUp());
        binding.tvFooter.setText("来源：" + baseUrl + " · 长按首页标题可改地址");

        observeViewModel();
        viewModel.load(baseUrl, "");
    }

    private void observeViewModel() {
        viewModel.getEntries().observe(this, this::renderEntries);

        viewModel.getCurrentDir().observe(this, dir -> {
            String shown = getString(R.string.sd_root) + (dir == null || dir.isEmpty() ? "" : "/" + dir);
            binding.tvPath.setText(shown);
            binding.btnUp.setVisibility(dir == null || dir.isEmpty() ? View.INVISIBLE : View.VISIBLE);
        });

        viewModel.getLoading().observe(this, loading -> {
            boolean busy = Boolean.TRUE.equals(loading);
            binding.tvEmpty.setVisibility(busy || adapter.getItemCount() == 0 ? View.VISIBLE : View.GONE);
            if (busy) {
                binding.tvEmpty.setText(R.string.loading);
            }
        });

        viewModel.getToast().observe(this, event -> {
            String text = consume(event);
            if (text != null) {
                Toast.makeText(this, text, Toast.LENGTH_SHORT).show();
            }
        });
    }

    private void renderEntries(List<SdEntry> entries) {
        adapter.submit(entries);
        boolean empty = entries == null || entries.isEmpty();
        binding.tvEmpty.setVisibility(empty ? View.VISIBLE : View.GONE);
        if (empty) {
            binding.tvEmpty.setText(R.string.empty_dir);
        }
    }

    private String consume(Event<String> event) {
        return event == null ? null : event.getContentIfNotHandled();
    }

    private void onEntryClick(SdEntry entry) {
        if (entry.isDir()) {
            viewModel.load(baseUrl, entry.getPath());
        } else {
            download(entry);
        }
    }

    private void goUp() {
        String dir = viewModel.getCurrentDir().getValue();
        if (dir == null || dir.isEmpty()) {
            finish();
            return;
        }
        int idx = dir.lastIndexOf('/');
        viewModel.load(baseUrl, idx < 0 ? "" : dir.substring(0, idx));
    }

    @Override
    public void onBackPressed() {
        String dir = viewModel.getCurrentDir().getValue();
        if (dir != null && !dir.isEmpty()) {
            goUp();
        } else {
            super.onBackPressed();
        }
    }

    /** 下载文件：API 28 及以下先申请存储权限（使用时申请） */
    private void download(SdEntry entry) {
        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.P
                && ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            pendingDownload = entry;
            ActivityCompat.requestPermissions(this,
                    new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_STORAGE);
            return;
        }
        enqueueDownload(entry);
    }

    private void enqueueDownload(SdEntry entry) {
        try {
            String url = XiaozhiApi.downloadUrl(baseUrl, entry.getPath());
            DownloadManager.Request request = new DownloadManager.Request(Uri.parse(url));
            request.setTitle(entry.getName());
            request.setDescription("来自 AI小智 SD 卡");
            request.setNotificationVisibility(
                    DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
            request.setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, entry.getName());

            DownloadManager manager = (DownloadManager) getSystemService(Context.DOWNLOAD_SERVICE);
            if (manager == null) {
                Toast.makeText(this, "系统下载器不可用", Toast.LENGTH_SHORT).show();
                return;
            }
            manager.enqueue(request);
            Toast.makeText(this, getString(R.string.download_started, entry.getName()),
                    Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(this, "下载失败：" + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQ_STORAGE) {
            return;
        }
        boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        if (granted && pendingDownload != null) {
            SdEntry entry = pendingDownload;
            pendingDownload = null;
            enqueueDownload(entry);
        } else {
            pendingDownload = null;
            Toast.makeText(this, R.string.permission_storage_denied, Toast.LENGTH_SHORT).show();
        }
    }
}
