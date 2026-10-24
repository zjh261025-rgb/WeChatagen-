# 微信 4.1.10.27 发送真「语音条」——实测可用的最小方案

微信 PC 端的语音条不是普通文件：它是 `msgtype 34` + Tencent **silk** 编码，必须交给微信自己的发送调度。
现成 hook 一般只开 `/SendTextMsg`、`/SendStickerMsg`、`/SendFileMsg`，**没有发语音的接口**，所以这里给的是
「自己构建 hook DLL + 调微信内部 ABI」的做法。

> 本仓库是实测记录与可复用代码，**只针对 4.1.10.27**。换版本必须重新定位 RVA 并重新校验特征码。

## 结论（一句话）

```
语音对象 = 工厂 0x619A190(out, targetStr, silkStr, &durationMs, &extra)
         → body+0xD8 写 34
         → BuildSendParam1(owner)     # 任务包装
         → BuildSendParam2(second,…)  # 第二参数
         → send_message 0x1677A30(first, second)
```

实测：接口返回 `nativeReturn=0x1`，微信不崩，「文件传输助手」里出现可播放的语音条；连发两条正常。

## 复刻前提清单（动手前先自检）

> 这套方案能跑通，是有几个**硬前提**同时成立的。先花 5 分钟把下面勾一遍，
> 不满足的项先解决，否则会在半路卡住（而且大概率卡在「编译过了但发不出去」这种没有报错的地方）。

### A. 客户端

- [ ] **微信 PC 版必须是 4.1.10.27**（所有 RVA 都基于这个版本）
- [ ] 微信能正常登录（换 DLL 需要完整退出客户端，可能要求重新登录/手机确认）
- [ ] 知道**实际生效的 DLL 位置**：在 `Weixin.exe` 同目录，不是安装包的 vendor 模板

```powershell
# 查版本（应是 4.1.10.27）
(Get-Item '<客户端目录>\Weixin.exe').VersionInfo.FileVersion
# 查目录里有没有被替换的 hook DLL
Get-ChildItem '<客户端目录>' -Filter version.dll | Select-Object Length, LastWriteTime
```

### B. 宿主（把 DLL 加载起来的那个东西）

- [ ] 有一个 hook 宿主：WeChatAgent 0.4.0，或同源的 `aixed/WeChat-Hook` 定制构建
- [ ] 宿主能正常起服务，`/WeChatAgent/health` 返回 `startupState=ready`
- [ ] 宿主源码里能找到「发送文本 / 发送文件」的实现（本仓库的 patches 基于这套覆盖层）

```powershell
# 宿主在跑的话，这个会返回 JSON
Invoke-RestMethod 'http://127.0.0.1:30001/WeChatAgent/health' -Headers @{ Authorization = 'Bearer <native-token>' }
```

> 没有宿主 → 本仓库的 patches 用不了；但核心只有「工厂 + 构造 + send_message」三处调用，
> 可以自己写一个独立注入模块（DLL 注入 + 自建 HTTP 端口），工作量约半天到一天。

### C. 工具链

- [ ] Visual Studio（含 MSVC C++ 与 **MASM x64**）
- [ ] Windows SDK
- [ ] `PlatformToolset` 与本机 VS 匹配（本项目实测 **VS2026 / MSVC 14.50 / v145**；旧脚本写死 v143 会失败）
- [ ] 能编译出与现装版本功能等价的 DLL（**先验证这一步，再谈语音**）

### D. 权限与环境

- [ ] 对客户端目录有写权限；若有 ACL 防篡改（Deny Write），需要先解锁（本项目用备份 SDDL 的脚本，可一键恢复）
- [ ] 能完整退出/启动微信客户端（不是关窗口）
- [ ] 有原始 DLL 备份和回滚方案（**必须**）

### E. 建议的第一步（别直接实现）

1. 先编一个**只读探针**版本，挂到 `0x619B3B0`；
2. 自己录一条语音发出去，确认日志出现 `hook_armed` 和一次 `hit`；
3. 命中说明 RVA 还对得上、版本没跑偏 —— 再开始写 live 发送。

> 探针里已经有「首 21 字节特征码 + body 虚表 + `VS_FIXEDFILEINFO` 版本号」三重校验，
> 版本不对会直接拒绝挂载，不会瞎挂钩子把客户端搞崩。

### 不满足时怎么办

| 缺什么 | 处理方式 |
|---|---|
| 版本不是 4.1.10.27 | 先想办法拿到该版本安装包；或按《方法总览》重新定位 RVA（1~3 天） |
| 没有宿主 | 自己写独立注入模块（DLL 注入 + HTTP 端口），核心只调三个函数 |
| 没有 MASM | 安装 VS 的「C++ 生成工具」组件；或把入口汇编改成 C++（风险更高） |
| 目录被锁 | 先备份 SDDL，再临时解锁；装完立刻恢复 |
| 只想验证可行性 | 只做探针，不做 live —— 命中一次就足以证明这套路子在你的版本上成立 |

## 目录

```
docs/方法总览.md        完整方法：RVA 表、对象布局、探针方法论、live 序列、踩坑
src/voice_probe.*       只读探针（MinHook 挂 0x619B3B0，全寄存器保存 + 尾跳 trampoline）
src/voice_sender.*      live 实现（dry-run 默认、live 开关、日志、互斥+冷却+上限）
tools/voice-send.py     音频 → ffmpeg → pysilk → HTTP 调 hook 的一键脚本
patches/                接进既有 hook 工程（aixed/WeChat-Hook 定制构建）的补丁
logs/                   真实命中样本与发送链日志（wxid 已脱敏）
```

## 关键 RVA（4.1.10.27，ImageBase 0x180000000）

| RVA | 用途 |
|---|---|
| `0x619A190` | 语音对象工厂，返回 `{body, owner}`，`body = owner + 0x10` |
| `0x619B3B0` | 就地构造函数（自 body 起写派生虚表 0x8DD0028） |
| `0x6199D65` | `mov dword ptr [rax+0D8h], 22h` —— 写 msgtype=34 |
| `0x1677A30` | 通用 send_message 调度（文本/文件/表情/语音共用） |
| `0x84EC9C8` | 发送任务虚表，`+0x10` 槽 = `0x1677A30` |
| `0xDF40` / `0xA0CE0B0` | BuildSendParam2 依赖的辅助函数 / 全局值 |

## body 字段（实测）

| 偏移 | 含义 |
|---|---|
| `+0xB0` | 目标 wxid（MSVC std::string，size@+0x10 / cap@+0x18） |
| `+0xD8` | msgtype = 34 |
| `+0x708` | silk 音频字节（`0x02#!SILK_V3` 开头） |
| `+0x728` | 时长（毫秒） |
| Extra（`[RSP+0x28]`） | 64 字节，实测 `{4, 3, 30000, 0…}` |

## 音频转 silk

```bash
ffmpeg -y -i in.mp3 -ac 1 -ar 24000 -f s16le out.pcm
python -c "import pysilk;open('a.silk','wb').write(pysilk.encode(open('out.pcm','rb').read(),sample_rate=24000))"
# pysilk 输出自带 0x02 前缀
```

## 已知限制 / Known issues

### 1. 只在「助手模式」下可用；「聊天模式」下发不出语音（2026-10-23 发现，尚未修复）

**现象**：把聊天切到「聊天模式（social）」后，让她发语音没有效果，只能回文字。

**根因**：发语音需要两步——① 本地 TTS 生成音频；② 调 `/SendVoiceMsg` 把音频发成语音条。
本方案里这两步是靠工具/脚本完成的（`tts_speak` 生成音频 + `scripts/voice-send.py` 转 silk 并调接口），
而**聊天模式下工具会被策略过滤**：

| | 聊天模式（social） | 助手模式（assistant） |
|---|---|---|
| 可用工具 | 仅授权白名单（如 `fortune_cast`、`meme-search__search`） | DSH 原生全套（`pwsh`、`tts_speak`、`wechat_file_send`…） |
| 能否跑 `voice-send.py` | ❌ | ✅ |

**注意：这不是 hook 的问题**——`/SendVoiceMsg` 接口本身在两种模式下都能正常返回
（`nativeReturn=0x1`），聊天模式下也一样能发。真正缺的是「谁来生成音频、谁来调它」。

**临时绕过**：把该聊天切到助手模式即可（1 分钟）：

```jsonc
// config/wechatagent.json
"wechat": {
  "workModes": {
    "wechat:private:1056893460": "assistant"   // 把私聊切成助手模式
  }
}
```

重启桥接服务后，记忆卡里的「发语音条」指令就能执行了。

**正式修法（TODO，未做）**：在 bridge 层加原生工具，进而在两种模式下都能用：

1. `wechat_voice_say(text)`：桥接内部完成 TTS → silk → `/SendVoiceMsg`；
2. `wechat_voice_send(path)`：把已有音频发成语音条；
3. 在 `social-tool-restrict.mjs` 的策略里对社交模式放行这两个工具（不依赖 `pwsh`）。

这样做还顺带解决「聊天模式下模型没有 `pwsh`、无法跑外部脚本」的通用问题。

### 2. 版本强耦合

所有 RVA 只对 **4.1.10.27** 有效；微信一升级，`0x619A190 / 0x619B3B0 / 0x1677A30` 都要重新定位，
探针里的「首 21 字节特征码 + body 虚表 + VS_FIXEDFILEINFO 版本」三重校验就是防止误挂到别的版本上。

### 3. 未做完整回归

本方案只在单个客户端上验证过「发语音条」与「连发两条」；文本/文件/表情发送沿用原有实现，
理论上不受影响，但没有做系统性的回归测试。

## 依赖与来源

本仓库**不包含**上游源码，只包含「发送语音条」这一层的增量。要复刻，需要先准备：

| 项目 | 许可证 | 用途 | 地址 |
|---|---|---|---|
| `sourcepro5/WeChatAgent` | MIT | hook 宿主：覆盖层源码、构建脚本、DSH 插件 | https://github.com/sourcepro5/WeChatAgent |
| `Derpyu520/qq-bridge` | MIT（GitHub 标记 NOASSERTION，许可证文件内容是 MIT；随 WeChatAgent vendored 为 `components/qq-bridge`） | 桥接核心：社交模拟、消息决策与投递、安全 MCP 工具 | https://github.com/Derpyu520/qq-bridge |
| `aixed/WeChat-Hook` | **无许可证，不随本仓库分发** | hook 底座：`version.dll` 注入框架、发送调度 | https://github.com/aixed/WeChat-Hook |

本项目改动的上游文件只有 4 处，全部以 patch 形式给出（见 `patches/`）；
新增文件是 `src/voice_sender.*`、`src/voice_probe.*`、`tools/voice-send.py`。

详细对应关系（每个文件用在哪、为什么不上传上游源码）见 [`docs/依赖与来源.md`](docs/依赖与来源.md)。

## 封号风险（必读）

用 hook 方式跑机器人，**账号随时有被封的可能**，这是方案本身的性质：

- 客户端目录被改（`version.dll`）、内存里有非官方模块、内部函数入口被改写 —— 这些都属于客户端完整性问题；
- 直接调内部 ABI 发消息，调用栈/线程/入参状态都和官方路径不同；
- 「秒回、7×24 在线、固定间隔、群发」这类行为特征同样会被风控识别。

**后果**：轻则消息失败/要求重新登录/功能受限，中则临时封禁，重则永久封号（聊天记录与好友关系不可恢复）。

**降风险最低要求**：

1. **用小号，不要用主号**；小号先养一段时间再上机器人；
2. 回复间隔 ≥ 5 秒、每分钟 ≤ 3~5 条、加随机延迟、偶尔沉默；
3. 不群发、不自动加好友、不主动私聊陌生人、不碰营销内容；
4. 只开必要功能，用完卸掉调试用的探针；
5. 账号一旦出现验证/消息失败，立即停机器人、恢复官方 DLL。

详见 [`docs/封号风险.md`](docs/封号风险.md)。

## 免责声明

这是对微信客户端内部函数的调用，属于非官方行为：可能违反软件许可协议、会随版本更新失效、
有崩溃与账号风险。仅用于自建机器人的研究与个人使用，风险自负。仓库不含任何账号凭据或聊天内容。
