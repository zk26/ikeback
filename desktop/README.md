# Ikeback - Virtual Eye On Networks

> ## ikeback — 桌面端总入口
>
> 本目录是仓库 <https://github.com/zk26/ikeback> 的**桌面端子目录** `desktop/`。
> 仓库根（总 README、LICENSE、NOTICE、Android 工程）在上一级，见 [`../README.md`](../README.md)。
>
> 本分支是 **Ikeback 4.x 的教学管理定制分支**，在保留 Ikeback 原有监控/控屏/演示能力的基础上，
> 增加了班级与 IP 绑定、课堂密钥防退出、全量行为日志与兴趣词云、积分制与期末报表、
> UWF 还原与远程装软件、本地 AI 助手、Android 教室端 App 等能力。
>
> ### 目录结构
>
> | 路径 | 内容 |
> |---|---|
> | `plugins/classroom/` | ikeback 插件：教师端控制台、学生端悬浮客户端、日志/积分/词云、Bridge 服务 |
> | `mobile/` | Android 教室端 App（Kotlin，零第三方网络库） |
> | `docs/` | 协议、部署、发布、许可证全套说明 |
> | `.github/workflows/` | 发布构建流水线 |
> | `scripts/` | 发布包打包脚本 |
> | `core/`、`master/`、`server/` | 对上游的最小扩展点 |
>
> ### 三类发布产物
>
> | 产物 | 运行位置 |
> |---|---|
> | **教师端** | 教师机 Windows（Master + Configurator + ikeback 插件 + Bridge） |
> | **学生端** | 学生机 Windows（Service/Server/Worker + ikeback 插件，UWF 还原环境） |
> | **APP** | Android 11+ 教室端 |
>
> 产物清单、命名、校验与安装见 [docs/IKEBACK-RELEASE.md](docs/IKEBACK-RELEASE.md)。
> 当前仓库**不含预编译二进制**，三类产物由发布工作流在推送 `v*` tag 时自动构建并上传到 Releases。
>
> ### 文档索引
>
> | 文档 | 内容 |
> |---|---|
> | [`../README.md`](../README.md) | **仓库总入口**（整个项目的目录结构与全部说明索引） |
> | [`../NOTICE.md`](../NOTICE.md) | 版权归属、修改声明、商标、隐私与无担保声明 |
> | [`../LICENSE`](../LICENSE) | GNU GPL v2 全文 |
> | [docs/IKEBACK-FEATURES.md](docs/IKEBACK-FEATURES.md) | **全部功能验收总表**（逐条对照需求 / 实现位置 / 入口 / 未完成项） |
> | [docs/IKEBACK.md](docs/IKEBACK.md) | 功能总览、架构、目录结构与构建 |
> | [docs/IKEBACK-BUILD.md](docs/IKEBACK-BUILD.md) | **真实编译方案**（GitHub Actions / MinGW / 排错） |
> | [docs/IKEBACK-PROTOCOL.md](docs/IKEBACK-PROTOCOL.md) | FeatureMessage 命令表、Bridge HTTP/UDP 协议、认证算法、数据文件格式 |
> | [docs/IKEBACK-DEPLOY.md](docs/IKEBACK-DEPLOY.md) | 教师机/学生机/手机端部署、UWF 与 Defender 注意事项 |
> | [docs/IKEBACK-RELEASE.md](docs/IKEBACK-RELEASE.md) | 发布产物清单与打包规范 |
> | [docs/IKEBACK-UI.md](docs/IKEBACK-UI.md) | APP 交互与视觉规范、平板适配验收清单 |
> | [docs/IKEBACK-PRIVACY.md](docs/IKEBACK-PRIVACY.md) | **数据采集范围与合规义务**（部署前必读） |
> | [docs/IKEBACK-THIRD-PARTY.md](docs/IKEBACK-THIRD-PARTY.md) | 第三方组件与许可清单 |
> | [docs/IKEBACK-LICENSE.md](docs/IKEBACK-LICENSE.md) | 许可证与衍生作品声明 |
> | [plugins/classroom/README.md](plugins/classroom/README.md) | ikeback 插件源码结构 |
> | [scripts/README.md](scripts/README.md) | 依赖闭包收集、NSIS 安装器与图标生成脚本 |
> | [`../mobile/README.md`](../mobile/README.md) | Android 教室端工程说明 |
> | [`../.github/workflows/`](../.github/workflows/) | 编译验证 / Windows 构建 / 发布流水线 |
>
> 本项目继承上游 **GPL-2.0-or-later**（见 `COPYING` 与 `../LICENSE`）。
> 上游项目主页：https://github.com/zk26/ikeback/ — 本分支与上游 Ikeback 项目无隶属、背书或合作关系。
>
> Android 工程在仓库根 [`../mobile/`](../mobile/)（唯一一份）；桌面端图标由根目录
> [`../ikeback_logo.png`](../ikeback_logo.png) 生成，产品名统一为「ikeback」。
