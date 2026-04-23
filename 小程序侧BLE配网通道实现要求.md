# 小程序侧 BLE 配网通道实现要求（WATER_PROV）

适用工程：`water`  
固件侧模块：`components/blufi_custom`（当前为“自定义 BLE GATT + JSON”配网通道）  

本文档只描述“小程序侧必须怎么做才能稳定完成配网与配置”，用于前端/小程序开发对齐实现细节与边界条件。

---

## 1. 关键结论（必须遵守）

1. 连接后必须先订阅 TX Notify（写 CCCD=0x0001），否则收不到任何回包 JSON。
2. 必须请求更大 MTU（建议 256），避免 MQTT 配置等 JSON 超过 20 字节导致写入失败/截断。
3. 必须按步骤顺序发送 JSON（步骤门禁），乱序/重复会被设备忽略，并在串口提示 step mismatch。
4. 配网窗口仅上电/重启后 120 秒内有效；超时后设备停止广播并拒绝写入（可能直接断开）。
5. 单连接：同一时间只允许 1 个 BLE 连接，避免并发干扰。

---

## 2. 设备发现与连接

### 2.1 设备名与广播
- 广播名（Device Name）：`WATER_PROV`
- 广播内容：包含自定义 Service UUID（128-bit），建议优先用 UUID 过滤/匹配而不是只靠名字。

### 2.2 GATT 服务与特征值（128-bit UUID）

| 类型 | 名称 | UUID | 属性 | 用途 |
|---|---|---|---|---|
| Service | Provision Service | `9E3C0001-2C6B-4F9B-8B5A-6F686E3D110A` | Primary | 自定义配网服务 |
| Characteristic | RX | `9E3C0002-2C6B-4F9B-8B5A-6F686E3D110A` | Write / WriteNoRsp | 小程序写入 JSON 到设备 |
| Characteristic | TX | `9E3C0003-2C6B-4F9B-8B5A-6F686E3D110A` | Notify | 设备通知回包 JSON 给小程序 |

### 2.3 连接后的必做动作
1. 发现服务与特征值。
2. 订阅 TX Notify：写 TX 的 CCCD 为 `0x0001`。
3. 请求 MTU：建议 `256`（或更大，取决于平台限制）。

---

## 3. 写入要求（避免“写了但设备不处理/不回包”）

### 3.1 编码与数据格式
- 编码：UTF-8
- 载荷：JSON 对象文本（Object）
- 不要求固定字段顺序

### 3.2 写入方式建议
- 调试阶段优先使用 `Write（带响应）`，更容易定位失败原因。
- 量产可使用 `WriteNoRsp` 提升吞吐，但仍需做失败重试与超时处理。

### 3.3 长度与 MTU
- 未请求 MTU 时，ATT 默认有效载荷通常很小（常见 20 字节级别），长 JSON 容易失败。
- 建议策略：
  - 连接成功后立刻请求 MTU=256
  - 单条 JSON 控制在 200 字节级别以内（为不同平台预留空间）

---

## 4. 步骤门禁与推荐时序

设备侧有严格步骤门禁，必须按以下步骤推进。

### 4.1 步骤定义（强制）

| Step | 小程序 -> 设备 | 设备 -> 小程序 | 说明 |
|---|---|---|---|
| 1 | `{"statusBLE":0}` | `{"statusBLE":0}` | 握手开始 |
| 2 | `{"statusNet":0/1}` | `{"statusNet":1}` | 选择 Wi‑Fi(0) / 4G(1)，设备回确认 |
| 3（Wi‑Fi） | `{"ssid":"...","password":"..."}` | `{"statusWiFi":0}` | 设备开始连接 Wi‑Fi |
| 3（Wi‑Fi） | （无） | `{"statusWiFi":1/2}` | 成功（拿到 IP）回 1；失败回 2 |
| 4 | `{"mqttserver":"ip:port","username":"...","password":"..."}` | `{"statusMQTT":0/1}` | MQTT 成功回 0；失败/超时回 1 |
| 5 | `{"statusBLE":1}` | `{"statusBLE":1}` | 结束流程；设备关闭窗口并主动断开 |

### 4.2 推荐完整时序（Wi‑Fi 模式）
1. 连接 BLE -> 订阅 TX Notify -> 请求 MTU=256
2. 发送 `{"statusBLE":0}`，等待回 `{"statusBLE":0}`
3. 发送 `{"statusNet":0}`，等待回 `{"statusNet":1}`
4. 发送 Wi‑Fi JSON（必须包含 `ssid` 字段），等待回 `{"statusWiFi":0}`
5. 等待设备回 `{"statusWiFi":1}` 或 `{"statusWiFi":2}`
6. 发送 MQTT JSON，等待设备回 `{"statusMQTT":0}` 或 `{"statusMQTT":1}`
7. 发送 `{"statusBLE":1}`，等待回 `{"statusBLE":1}`，随后设备会断开连接

---

## 5. 字段识别注意事项（避免误判）

1. MQTT 配置也会包含 `password` 字段，因此“仅出现 password”不代表 Wi‑Fi 配置。
2. 设备侧以 `ssid` 字段作为 Wi‑Fi 配置识别条件：Wi‑Fi JSON 必须包含 `ssid`。

---

## 6. 超时与重试建议（小程序侧）

### 6.1 必要超时
- 握手回包（statusBLE/statusNet）：建议 3 秒超时
- Wi‑Fi 结果（statusWiFi=1/2）：建议 30~60 秒超时（取决于网络环境）
- MQTT 结果（statusMQTT=0/1）：建议 25 秒超时（设备侧默认 20 秒超时）

### 6.2 重试建议
- 任一步骤超时：优先断开重连，从 Step 1 重新走流程（更稳）
- 若发现设备不再广播：可能已超出 120 秒配网窗口，需要设备重启/触发“网络重置”后再试

---

## 7. 典型失败表现与排查

1. 小程序写入成功但收不到回包  
   - 大概率未订阅 TX Notify（未写 CCCD=0x0001）。
2. 写入偶发失败或长 JSON 被截断/不生效  
   - 大概率未请求 MTU 或 MTU 太小；确保 MTU=256，并控制单条 JSON 长度。
3. 连接后立刻被断开  
   - 可能已超出 120 秒配网窗口；设备侧会拒绝会话并终止连接。
4. 发送乱序消息无反应  
   - 设备侧步骤门禁会忽略；请按 Step 顺序发送。

---

## 8. 示例报文

### 8.1 Wi‑Fi 配置
```json
{"ssid":"PURESUN-1F","password":"404NotFound"}
```

### 8.2 MQTT 配置
```json
{"mqttserver":"mqtts://mqtt.gredicer.top:8883","username":"backend_core_api","password":"********"}
```

### 8.3 结束
```json
{"statusBLE":1}
```

