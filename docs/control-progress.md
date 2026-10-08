# 控制面实现进度

控制服务已接入 daemon，默认关闭。凭证、审计、Worker、配置热加载、
8 连接上限和 OpenSSL 3 HTTPS 已实现。安装和使用见 control.md / control.en.md。

## 已实现

- control.hpp / control.cpp：Token 文件读取、认证、来源限流与审计回调。
- 文件须为当前服务用户拥有的普通文件，权限严格为 0640。
- O_NOFOLLOW 拒绝符号链接，令牌长度限制 32..512 字符，匹配 HTTP 单头部限制。
- 认证比较不根据令牌内容提前返回；失败不输出令牌。
- GET /v1/config/generation 的业务处理。
- POST /v1/actions/fan、POST /v1/actions/inspect 的业务处理。
- 请求体严格为 sensor/action 两个字符串字段；禁止路径、重复键和其他字段。
- 动作路径取自加锁配置快照，提交 Worker，再调用 RecoveryPolicyEngine。
- 返回 202 只表示排队成功，执行结果由异步审计回调记录。
- 同一动作的恢复冷却仍由恢复引擎负责。
- C++ 单测覆盖文件权限、符号链接、认证限流、路径拒绝和恢复冷却。

## 本阶段完成的集成

- control-port / control-token-file 命令行与 fail-closed 监听入口。
- 控制角色并发上限 8，与只读角色分离。
- peer 必须取来源 IP，不包含临时源端口，否则限流可被重连绕过。
- 审计回调连接 SEL important=true 与 Logger；解析错误和超时也须审计。
- 原子配置快照随成功热加载更新。
- 可选 OpenSSL 3 TLS 构建及非阻塞握手。
- 非回环控制绑定必须启用 TLS；证书验证不得有跳过选项。
- 部署和中英文 README 的控制面使用说明。

TLS 模式为服务器证书加 Bearer 认证；客户端验证证书链与 URL 主机名/IP。
集成测试覆盖错误信任根和错误主机名被拒绝。mTLS 未纳入当前单令牌方案。
上行遥测、网络指标和实例心跳均已实现。心跳包含验证 CA/主机名的出站 TLS 客户端，见 peer.md / peer.en.md。令牌和证书轮换需重启服务。
