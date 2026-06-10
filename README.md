# ESP32S3_PPP_V1

ESP32-S3 + EC200A 的 4G 转 Wi-Fi 路由工程，当前以 PPP 拨号模式作为基础稳定版本。

## 当前版本

- 项目版本：`V1.1`
- 目标芯片：`ESP32-S3`
- 4G 模组：`EC200A`
- 工作模式：`PPP`

## 当前已完成目标

- ESP32-S3 作为 USB Host 接入 EC200A
- 使用 AT + PPP 完成 4G 拨号
- 开启 Wi-Fi SoftAP
- SoftAP 客户端通过 4G + NAPT 访问外网
- 提供本地 WebUI 做状态查看和基础配置

## 默认参数

- SSID：`EC200A`
- 密码：`12345678`
- 信道：`1`
- 默认 APN：`中国移动 -> cmnet`

## 目录说明

- `main/main.c`：启动协调
- `main/app_config.*`：NVS 配置与默认参数
- `main/app_state.*`：运行时共享状态
- `main/wifi_ap.*`：SoftAP 初始化与重配置
- `main/webui.*`：本地管理页面
- `main/led_status.*`：WS2812 状态指示
- `main/system_monitor.*`：温度与任务分布监控
- `main/cellular_ppp.*`：USB Host、AT、PPP、NAPT
- `功能文档.md`：当前版本功能说明
- `记忆文档.md`：项目长期记录
- `版本记录.md`：版本变更记录
- `GitHub上传规范.md`：上传与提交规范

## 构建方式

```powershell
. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
cd D:\HSP\esp_projects\ESP32S3_PPP_V1
idf.py set-target esp32s3
idf.py build
idf.py -p COM25 flash monitor
```

## 维护要求

- 重要变更同步更新 `功能文档.md`
- 版本发布同步更新 `版本记录.md`
- 有新的硬件/调试经验同步更新 `记忆文档.md`
- 保持模块拆分，不把业务逻辑堆进 `main.c`
