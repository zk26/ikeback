# ikeback {VER}

本版本包含 Android 移动端、Windows 教师端/学生端以及发布流程的安全性与可靠性修复。

| 产物 | 文件 | 说明 |
|---|---|---|
| 教师端安装程序 | `ikeback-teacher-{VER}-win64-setup.exe` | Windows + MSYS2 UCRT64 + Qt6/MinGW；包含服务端与教师控制台 |
| 学生端安装程序 | `ikeback-student-{VER}-win64-setup.exe` | 同一套运行时，仅安装学生端服务 |
| Android APP | `ikeback-Mobile-{VER}.apk` | 正式签名、不可调试；`versionName={VER}`、`versionCode={CODE}` |
| 校验文件 | `SHA256SUMS.txt` | 覆盖上述三个产物 |

## 重要变更

### 手机课堂连接与响应校验

- 所有请求使用一次性 nonce，服务端响应增加 nonce、时间戳和 HMAC-SHA256 签名。
- Android 在 UTF-8 解码前验证原始响应字节，拒绝缺失签名、nonce 不匹配、时间偏差和篡改内容。
- 响应大小、请求体、连接数量和重试次数均受到限制。
- 名单同步失败时保留已有缓存；有效空名单仍可正常替换缓存。
- 课表增加深色主题、键盘操作和 TalkBack 自定义动作。

### 积分与商城持久化

- 商城变更使用文件锁和变更前重载，避免并发覆盖。
- 购买、拍卖结算和退款使用持久化操作日志、固定积分流水 ID 与恢复流程。
- 保存失败后立即尝试回滚或前滚恢复，重复操作不会重复扣分或退款。
- 损坏 JSON、非法字段结构和已持久化文件消失时保留最后有效内存状态，不静默重置数据。
- 成绩册加载、切换班级、导出报告和 SVG 改为事务化或原子写入。

### Windows 安全与部署

- 可写文件通过 Windows 句柄校验最终路径，拒绝符号链接和重解析点路径替换。
- 可写文件应用受保护 DACL，私有密钥先写入受保护临时文件再原子改名。
- 默认日志和部署校验元数据迁移到当前用户私有 AppData。
- 便携应用使用 UUID 暂存目录，并在激活失败时执行可检查的回滚。
- 学生端卸载保护在 CLI 缺失、崩溃或返回异常时默认中止卸载，不再失败放行。

### 可访问性与发布

- 座位卡片支持键盘换座、取消和焦点恢复，并补充无障碍描述。
- 主题颜色增加对比度校验与自动调整。
- CI Action 固定到完整提交 SHA，测试目标先构建再执行，发布版本统一从 Android 版本文件解析。
- Windows 安装包通过真实编译、递归 DLL 依赖收集、CLI 版本/插件检查和 SHA-256 校验。

## 安装

1. 学生机：以管理员运行学生端安装程序，保持服务端组件勾选。
2. 教师机：以管理员运行教师端安装程序，保持教师控制台组件勾选。
3. 手机：安装 APK，输入本堂课密钥与教师机 IP/端口。

安装目录内的 `README.TXT` 提供中文说明；完整部署流程见 `desktop/docs/IKEBACK-DEPLOY.md`。

## 校验

```powershell
Get-Content SHA256SUMS.txt | ForEach-Object {
    $hash, $file = $_ -split '\s+', 2
    if ((Get-FileHash $file -Algorithm SHA256).Hash.ToLower() -ne $hash) {
        throw "校验失败: $file"
    }
}
```

## 已知限制

- 课堂 Bridge 使用明文 HTTP；HMAC 提供完整性和身份验证，不提供传输加密。
- Windows ACL、重解析点和安装/卸载行为仍需在目标学生机上完成实机验证。
- UWF 仅 Windows 10/11 企业版、教育版和 IoT 企业版支持。
- 上游 `plugins/filetransfer` 当前未包含在 Windows 安装包中。

源码：<https://github.com/zk26/ikeback>
