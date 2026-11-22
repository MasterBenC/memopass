# MemoPass

FoloToy AI Passport 上的会议纪要采集器。开机后扫屏幕二维码配网，按确认开始录音，屏幕滚动中文转写，分段推到飞书。

## 按键

| 页面 | 上 | 确认 | 下 |
| --- | --- | --- | --- |
| 待机 | 设置 | 开始录音 | 记录 |
| 录音 | 标重点 | 短按暂停，长按 1.2 秒结束 | 静音 |
| 设置 | 上移 | 执行 | 下移 |
| 推送失败 | 放弃 | 重试 | 详情 |

待机时长按确认进入扫码配网。默认没有提示音。

## 配网

热点名 `MemoPass-XXXX`，页面 `192.168.4.1`。必填 Wi-Fi、火山 `APP ID` 与 `Access Token`、飞书 Webhook。地址里含 `bot/v2/hook` 时走群机器人，否则走多维表格。密钥只进设备 NVS，不进固件。

识别地址固定为 `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`，默认资源 `volc.bigasr.sauc.duration`。需要在同一应用下开通大模型流式语音识别。

## 构建

```bash
./tools/validate.sh --static
./tools/validate.sh --firmware
```

合并镜像是 `build/FoloToy-AI-Passport-full.bin`，从 `0x0` 刷写。刷写会清空 NVS，需要重新配网。未确认前不烧录。
