# 专用芯片驱动与标定

本文记录 `src/chips.cpp` 中各驱动的寄存器语义来源、换算公式与已知限制。
数据手册页码与链接见文末；凡未能在数据手册中确认的结论都单独标注。

## 1. 配置语法

后端字段由 `i2c` 扩展为 `i2c:<chip>[@<feature>]`：

```text
# 原始 SMBus word 读（原有行为，未改动）
i2c_word    i2c          /dev/i2c-1,0x48,0x00  1 high 70 90 3 3 3 -
# 专用驱动：path 只接受 device,address；@ 后选择测量量，省略则取该型号的第一个
cpu_temp    i2c:lm75b    /dev/i2c-1,0x48       1 high 70 90 3 3 3 -
fan_rpm     i2c:emc2103@rpm /dev/i2c-1,0x2E    1 low 500 200 20 3 3 /sys/class/hwmon/hwmon1/pwm1
vout        i2c:adm1275@vout /dev/i2c-1,0x10   1 high 12 13 0.5 3 3 -
```

支持的型号（`chip_names()`）：`lm75`、`lm75b`、`emc2103`、`adm1275`、`ina219`、`ina226`。
型号后的 `@feature` 必须是该型号确实提供的测量量，否则在**构造时**即报
`std::invalid_argument`，不会退化成运行期静默返回空值。

## 2. 标定（第 12 个配置字段，可选）

```text
# 语法：gain[:offset][;raw=value;raw=value...]
cpu_temp i2c:lm75b /dev/i2c-1,0x48 1 high 70 90 3 3 3 - 1.5:-2;0=10;100=210
```

换算顺序是**先线性、后分段插值**：`corrected = raw * gain + offset`，
再在 `points` 上做分段线性插值；区间外按端点钳制，**不外推**。
`points` 必须按 raw 严格递增，乱序或非有限值在 `validate()` 与加载期都会被拒绝。
旧格式（11 字段）不受影响，缺省标定为恒等。

## 3. 各驱动

### LM75 / LM75B（温度）

核验状态：已列出手册链接，但尚未记录具体章节/表号，以下硬件事实在本仓库中仍标记为**未核实**。

温度寄存器 `0x00`，16 位、大端、二进制补码、左对齐。**同一芯片家族的分辨率并不统一**：

| 型号 | 位宽 | LSB | 说明 |
|---|---|---|---|
| 原始 LM75 / TI LM75A | 9 | 0.5 ℃ | 低 7 位未定义 |
| NXP LM75A / LM75B | 11 | 0.125 ℃ | 低 5 位恒为 0 |

本实现的 `lm75` 按 9 位（`raw >> 7`，×0.5）、`lm75b` 按 11 位（`raw >> 5`，×0.125）。
注意 NXP 的 LM75A 是 11 位而 TI 的 LM75A 是 9 位，因此**不能只凭 "LM75A" 判断分辨率**。
`T_HYST`/`T_OS` 在所有型号上都只有 9 位精度。

已知陷阱（未在代码中自动规避）：
- LM75A 若只做单字节读且低字节 bit7 为 0，会把 SDA 永久拉低导致总线挂死（LM75B 已修复）。
  本驱动使用 `read_word`（一次完整事务）读取两个字节，符合数据手册要求。
- 原始 LM75 在任何访问时都会中断并重启正在进行的转换，轮询间隔不应短于 300 ms。

### EMC2103（转速 + 温度）

核验状态：**未核实**。Rev 0.85 是 preliminary；尚未取得并逐项对照 DS20006705 发布版。
以下常量、位域、故障码与内部温度分辨率是当前实现假设，不应理解为已经确认的硬件保证。

转速 `0x4E`（高字节）/ `0x4F`（低字节），13 位左对齐，低 3 位恒为 0：

```
COUNT = (hi << 5) | (lo >> 3)
RPM   = 3932160 / (COUNT × m)
```

常量 `3932160` 是当前实现的候选值，时钟/边沿推导**未核实**。
此前写出的等式 `3932160 = 60 × 32768 / 5` 不成立，不能作为该常量的依据。
`m` 由 Fan Configuration 1（`0x42`）的 `RANGE[1:0]`（bit6:5）决定，取值 1/2/4/8，
本实现取默认量程（1000 RPM）的 **m = 2**；更换量程需要同步调整，否则转速整体偏移。
高 5 位全 1（寄存器默认值 `FFh`/`F8h`）表示风扇停转或未接，按"无读数"处理。

温度：内部 `0x00/0x01`、外接二极管 1 为 `0x02/0x03`，每通道 16 位有符号、
0.125 ℃ 分辨率（`C = (int16)((hi << 8) | lo) / 256`）。**`0x8000` 表示二极管故障，
不能当作 −128 ℃ 上报**，本实现返回"无读数"。从地址固定为 `0x2E`。

未确认项：数据手册 Rev 0.85 未记录 tach 的影子/锁存寄存器，因此高/低字节分两次读
不保证原子性；本实现按数据手册与内核驱动的做法分两次 `read_byte`。

### ADM1275（电压 + 电流，PMBus DIRECT 格式）

核验状态：**未核实**。Rev.E 手册链接已记录，但命令表、换算公式与量程位的具体章节/表项尚未留存。

`READ_VIN 0x88` / `READ_VOUT 0x8B` / `READ_IOUT 0x8C`，数据右对齐在 bit[11:0]、无符号
（因此代码里 `& 0x0FFF` 是必需而不是可选）。使用数据手册的 LSB 法而非舍入后的 m/b/R 系数：

```
V   = LSB × (code + 0.5)          LSB = 5.208 mV（0–20 V 量程）或 1.488 mV（0–6 V）
IOUT = 12.4 µV × (code − 2048) / R_SENSE
```

当前实现不提供功率与温度；“没有对应命令、完整命令表只有 31 条”这一判断**未核实**，因此暂不提供
`power`/`temp` 测量量——这一点与 ADM1272/1273/1278 等兄弟型号不同。未配置分流电阻时
`iout` 返回"无读数"而不是给出错误的安培值。电压量程由 `PMON_CONFIG`（`0xD4`）的
`VRANGE` 位选择，本实现默认 0–20 V。

### INA219 / INA226（母线电压 / 分流电压 / 电流 / 功率）

核验状态：手册编号 SBOS448G / SBOS547C 已记录，但具体章节/表项尚未留存，以下参数仍属**未核实**。

寄存器指针相同，但 LSB 与位域不同：

| 项目 | INA219 | INA226 |
|---|---|---|
| 分流电压 `0x01` | 16 位有符号，10 µV | 16 位有符号，2.5 µV |
| 母线电压 `0x02` | 13 位无符号，4 mV，**数据在 bit[15:3]** | 15 位无符号，1.25 mV，无需移位 |
| 功率 `0x03` | 16 位无符号，LSB = **20** × Current_LSB | 16 位无符号，LSB = **25** × Current_LSB |
| 校准 `0x05` | 系数 0.04096，**FS0 恒为 0** | 系数 0.00512 |

母线电压寄存器低 3 位是 `bit2`（未定义）、`bit1` CNVR、`bit0` OVF，因此 INA219 必须
右移 3 位后再换算，否则会读出错误电压。两种芯片都用大端（高字节在低地址）。

**电流与功率寄存器在写入校准寄存器之前恒为 0**，因此驱动在构造时就写校准值：
默认写 `4096`（INA219）/ `2048`（INA226），与 Linux `ina2xx` 驱动一致，
此时电流寄存器数值等于分流电压寄存器数值。若需要真实安培/瓦特，应提供分流电阻与
最大电流以按数据手册公式计算校准值（当前构造路径保留了该入口）。

已知限制：INA219 的电流/功率寄存器对负分流电压不产生负值，实际上是单向测量；
INA226 的电流寄存器是正常有符号的。

## 4. 测试覆盖

`tests/core_test.cpp` 中的 `FakeLinuxIo` 脚本化 ioctl 应答，因此上述解码与换算逻辑
不需要真实 `/dev/i2c-*` 即可单测。已覆盖：LM75 9/11 位与负温度、EMC2103 转速常量与
停转哨兵、EMC2103 温度与二极管故障码、ADM1275 12 位掩码与 0.5 偏移、INA219 母线移位
与校准写入、INA226 15 位母线与其功率系数、未知型号与不支持测量量的拒绝。

**未覆盖**：真实电气行为、I2C 时序、LM75A 总线挂死场景、EMC2103 高/低字节原子性、
INA 系列在真实分流电阻下的电流精度。

## 5. 数据手册来源

链接和手册编号仅供后续核验，不代表本仓库已完成硬件确认。当前逐项状态：

| 项目 | 候选来源 | 状态 |
|---|---|---|
| LM75/LM75B 分辨率、字节序及总线限制 | SNOS808P、NXP Rev.3/Rev.04；章节/表号未记录 | 未核实 |
| EMC2103 常数 3932160、0x8000、内部分辨率、固定 m=2/RANGE | preliminary Rev 0.85；DS20006705 发布版未取得 | 未核实 |
| EMC2103 高/低字节原子性 | preliminary Rev 0.85；锁存机制未确认 | 未核实 |
| ADM1275 READ_PIN/温度命令缺失、命令数、量程和换算 | Rev.E；具体命令表及公式章节未记录 | 未核实 |
| INA219/INA226 位域、换算、校准和符号行为 | SBOS448G/SBOS547C；章节/表号未记录 | 未核实 |

在取得手册并记录确切章节/表号前，不移除“未核实”标记，也不依据猜测修改驱动行为。

- LM75（National/TI 谱系，含 LM75A）：[LM75A SNOS808P](https://www.ti.com/lit/ds/symlink/lm75a.pdf)
- NXP LM75A / LM75B（11 位格式）：[LM75B Rev.3](https://media.digikey.com/pdf/Data%20Sheets/NXP%20PDFs/LM75B.pdf)、[LM75A Rev.04](https://media.digikey.com/pdf/Data%20Sheets/NXP%20PDFs/LM75A.pdf)
- EMC2103（Rev 0.85，初步版）：SMSC 84 页数据手册；发布版 DS20006705 未能获取，
  因此常量 `3932160`、故障码 `0x8000` 与内部分辨率尚未与发布版核对
- ADM1275 Rev.E：[ADM1275.pdf](https://www.analog.com/media/en/technical-documentation/data-sheets/ADM1275.pdf)
- INA219 SBOS448G：[ina219.pdf](https://www.ti.com/lit/ds/symlink/ina219.pdf)
- INA226 SBOS547C：[ina226.pdf](https://www.ti.com/lit/ds/symlink/ina226.pdf)
- 交叉核对：[Linux emc2103.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/hwmon/emc2103.c)、[Linux ina2xx.c](https://github.com/analogdevicesinc/linux/blob/main/drivers/hwmon/ina2xx.c)、[Linux adm1275.c](https://github.com/analogdevicesinc/linux/blob/main/drivers/hwmon/pmbus/adm1275.c)
