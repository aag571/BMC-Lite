# QEMU 集成环境

`run-qemu.sh` 启动带 KVM 的 x86_64 Ubuntu guest，并把 guest 的 8000/22 映射到宿主机 8000/2222。`guest-init.sh` 安装最小运行依赖；发布包需通过 scp 复制进 guest。

```sh
./run-qemu.sh
scp -P 2222 bmc-lite-release.tar.gz ubuntu@127.0.0.1:/tmp/
ssh -p 2222 ubuntu@127.0.0.1
tar -xzf /tmp/bmc-lite-release.tar.gz
cd bmc-lite-release
sudo bash deploy/install.sh
```

QEMU 通用 machine 不保证存在真实 `/dev/i2c-*` 或 `/dev/gpiochip*`。mock/sysfs、网络、systemd、SEL、HTTP 和故障注入可以在 guest 验证；I2C/GPIO 电气行为需要设备模型或开发板。
