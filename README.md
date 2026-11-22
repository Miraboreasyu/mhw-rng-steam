# `rng_steam` —— MHW:I 15.23 蒸汽机按键提示与控序插件

官方热重载子插件（`nativePC\plugins\mhw_rng\rng_steam.dll`），也是 `mhw_rng` Plugin SDK v3.0 的教学示例插件：同时支持在 **`ObserveOnly` 仅观测模式下零侵入透视按键顺序**，以及在 **`Takeover` 接管模式下按逆推公式锁定目标按键顺序**。

---

## 一、15.23 逆向机制与精确调用点

对《怪物猎人：世界 冰原》Steam 15.23（镜像基址 `0x140000000`）蒸汽机模块的分析表明，每轮三键顺序只在 `cSteamControl::start`（`0x140666640`）中通过两次 `MtRandom::nrand` 洗牌生成：

1. **`RVA 0x066679A` (`CALL 0x142174E30` @ `0x140666795`)**：
   - 将数组 `order[3]` 初始化为 `{0, 0, 0}` 后，抽取 `r1` 计算 `pos1 = r1 % 3`，写入 `order[pos1] = 1`。
   - 返回点入口保存有非易失寄存器 `RBX = cSteamControl*`。
2. **`RVA 0x06667BB` (`CALL 0x142174E30` @ `0x1406667B6`)**：
   - 抽取 `r2` 计算 `pos2 = (pos1 + (r2 & 1) + 1) % 3`，写入 `order[pos2] = 2`，余下槽位保持 `0`。
   - 返回点入口保存有 `RBX = cSteamControl*`、`RDI = pos1`（`0..2`）。
3. **按钮编码与按序校验**（由 `cSteamControl::move` @ `0x1406676C3..0x140667785` 与键位文本表 `FUN_141aea760` 交叉验证）：
   - `0` = 左键（键盘 `A` / Xbox `X` / PS `口(方)`）
   - `1` = 中键（键盘 `W` / Xbox `Y` / PS `△(三)`）
   - `2` = 右键（键盘 `D` / Xbox `B` / PS `○(圆)`）
   - 第 `step` 次按压直接校验 `order[step] == pressedButton`。
4. **隐藏必中保底判定 `RVA 0x05CA0A2`**（`FUN_1405ca060` @ `0x1405CA09D`，仅由 `cSteamControl::start` 调用）：
   - 通过 `RBX` 指向 `cSteamControl + 0x4A4` 的 `pityPct`（`12..100`），当 `(r % 100) < pityPct` 时置位 `cSteamControl + 0x4A8 = 1`，使本轮**任意按键均判为全中**（俗称猫饭/艾露猫隐藏必中保底）。
5. **`cSteamControl` 遥测字段**：
   - `+0x440`：`outputMode`（`uint32_t`，`0 = 1x`，`1 = 10x`）
   - `+0x480`：`streak`（`uint8_t`，连胜计数）
   - `+0x4A4`：`pityPct`（`uint32_t`，隐藏必中概率百分比）
   - `+0x4A9`：`isRedRound`（`uint8_t`，`1` 表示红槽高倍率奖励轮）

### 锁序逆算公式
若目标三键排列为 `[b0, b1, b2]`（`{0,1,2}` 的任意全排列）：
- 设 `wantPos1` 为值 `1` 所在下标 $\Rightarrow$ 令第 1 处 RNG 返回 `wantPos1`；
- 设 `wantPos2` 为值 `2` 所在下标，计算 `delta = (wantPos2 + 3 - wantPos1) % 3`（必为 `1` 或 `2`）$\Rightarrow$ 令第 2 处 RNG 返回 `delta - 1`（`0` 或 `1`）。

---

## 二、聊天指令列表

所有指令通过游戏内聊天框发送（默认前缀 `$rng`）：

| 指令 | 说明 |
| :--- | :--- |
| `$rng steam` / `$rng steam status` | 查看提示开关、当前按键样式、锁序状态、上一轮结果与 6 种排列命中分布 |
| `$rng steam on` / `$rng steam off` | 开启或关闭蒸汽机每轮按键实时提示（默认开启） |
| `$rng steam style <kb\|cn\|xbox\|ps\|num>` | 切换提示样式：键盘 `A-W-D`（默认）、纯中文 `左-中-右`、`xbox`、`ps`、`1-2-3` |
| `$rng steam pity <on\|off>` | 开关“隐藏任意键必中保底触发”播报（默认开启） |
| `$rng steam lock [awd\|adw\|wad\|wda\|daw\|dwa\|r2\|off]` | 锁定固定按键顺序（不带参数默认锁 `awd`，按住空格/RT/R2即自动全中）；`off` 恢复原版顺序 |
| `$rng steam reset` | 清空锁序并归零本局统计计数 |
| `$rng steam help` | 在聊天栏查看简洁指令帮助 |

> [!TIP]
> 如果你只想**纯看原版随机按键提示、完全不接管游戏任何 RNG**，只需执行 `$rng observe` 开启内核的仅观测模式即可，`rng_steam.dll` 仍会在每轮开始时自动把正确答案发到游戏聊天栏。

---

## 三、展示的 SDK v3.0 开发范式

1. **Pre + Post 联合回调与 `ObserveOnly` 穿透**：使用 `RegisterSlotHookEx` 与 `kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly`，在 `ObserveOnly` 模式下依然接收 `ctx->finalValue0` 通知。
2. **MASM 现场非易失寄存器与 SEH 读结构体**：通过 `ctx->callerRegs->rbx` 与 `ctx->callerRegs->rdi` 读取调用现场的 `cSteamControl*` 与中间局部变量 `pos1`，并在 `IsValidUserPtr` + `__try / __except` 保护下提取红槽/保底率/连胜信息。
3. **跨线程异步聊天投递**：RNG 回调可由任意游戏工作线程触发，因此插件通过 `g_host->ChatPrintfAsync(...)` 投递至内核队列，再由 `sChat::move` 在主线程安全渲染到聊天窗口，且投递前已释放叶子锁 `g_stateMx`。
4. **生命周期钩子联动**：通过 `RegisterLifecycleHook` 监听 `$rng clearall`，在框架清规则时自动解除 `steam` 插件自身的锁序状态。

---

## 四、独立构建与热重载

```bat
:: 1. 从核心仓库同步 include\mhw_rng_api.h
sync_sdk.bat

:: 2. 编译生成 build\rng_steam.dll（或带 --deploy 直接部署到游戏目录）
build.bat --deploy
```

部署后若游戏正在运行，在聊天框输入 `$rng reload steam` 或 `$rng load steam` 即可热重载生效。
