package com.xiaozhi.display.ui;

import android.view.LayoutInflater;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.xiaozhi.display.data.SdEntry;
import com.xiaozhi.display.databinding.ItemSdEntryBinding;

import java.util.ArrayList;
import java.util.List;

/** SD 文件列表适配器：目录追加 "/"，文件右侧显示大小。 */
public class FileAdapter extends RecyclerView.Adapter<FileAdapter.Holder> {

    public interface OnEntryClick {
        void onClick(SdEntry entry);
    }

    private final List<SdEntry> items = new ArrayList<>();
    private final OnEntryClick listener;

    public FileAdapter(OnEntryClick listener) {
        this.listener = listener;
    }

    public void submit(List<SdEntry> list) {
        items.clear();
        if (list != null) {
            items.addAll(list);
        }
        notifyDataSetChanged();
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        ItemSdEntryBinding binding = ItemSdEntryBinding.inflate(
                LayoutInflater.from(parent.getContext()), parent, false);
        return new Holder(binding);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder holder, int position) {
        SdEntry entry = items.get(position);
        holder.binding.tvName.setText(entry.isDir() ? entry.getName() + "/" : entry.getName());
        holder.binding.tvName.setTextColor(holder.binding.getRoot().getContext().getColor(
                entry.isDir() ? com.xiaozhi.display.R.color.accent : com.xiaozhi.display.R.color.textMain));
        holder.binding.tvSize.setText(entry.getSizeText());
        holder.binding.getRoot().setOnClickListener(v -> listener.onClick(entry));
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static class Holder extends RecyclerView.ViewHolder {
        final ItemSdEntryBinding binding;

        Holder(ItemSdEntryBinding binding) {
            super(binding.getRoot());
            this.binding = binding;
        }
    }
}
