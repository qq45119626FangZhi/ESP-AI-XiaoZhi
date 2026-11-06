# ai_xiaozhi
ai小智智能聊天机器人，芯片ESP32-S3，基于大神虾哥提供的小智管理后台，使用deepseek大模型实现语言聊天

参考文档:https://xiaozhi.me/docs
<img width="674" height="308" alt="image" src="https://github.com/user-attachments/assets/3e6eea12-43cc-4f9b-9b21-acbbb01aa117" />


本项目使用
主控选择、频率
ESP32-S3：最高主频240M，RAM：512KB，Flash：8MB。提供蓝牙，WIFI功能。

1音频编解码器
型号：ES8311
控制信号通信：I2C
音频数据通信：I2S
音频编解码，播放交互图如下
<img width="1204" height="462" alt="image" src="https://github.com/user-attachments/assets/58506a15-c9b8-47a1-b9c4-95f5d6d1c299" />



音频流的格式，采样率、精度
格式：PCM，采样率：16000Hz，采样精度：16位。

功率放大器
型号：NS4150B。

IPS屏幕
型号：ST7789V
分辨率：320x240
调用库：lvgl移植



