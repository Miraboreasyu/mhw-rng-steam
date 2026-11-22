# `rng_steam` —— 蒸汽机按键提示与控序插件

- **插件标识**：`steam`（编译产物 `rng_steam.dll`，版本 `1.1.0`）
- **源码位置**：[steam_plugin.cpp](steam_plugin.cpp)
- **宿主依赖**：`mhw_rng.dll`（Plugin SDK `kMhwRngApiVersion == 3`，头文件 [mhw_rng_api.h](include/mhw_rng_api.h)）
- **接管调用点**：`0x066679A` / `0x06667BB`（三键排列生成）、`0x05CA0A2`（隐藏必中保底判定）

`rng_steam` 为官方子插件与 `mhw_rng` Plugin SDK v3.0 参考实现，支持在 `ObserveOnly` 模式下仅观测并输出按键顺序，以及在 `Takeover` 模式下按逆算公式固定目标按键顺序。

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
   - 通过 `RBX` 指向 `cSteamControl + 0x4A4` 的 `pityPct`（`12..100`），当 `(r % 100) < pityPct` 时置位 `cSteamControl + 0x4A8 = 1`，使本轮判定为任意按键全中。
5. **`cSteamControl` 遥测字段**：
   - `+0x440`：`outputMode`（`uint32_t`，`0 = 1x`，`1 = 10x`）
   - `+0x480`：`streak`（`uint8_t`，连胜计数）
   - `+0x4A4`：`pityPct`（`uint32_t`，隐藏必中概率百分比）
   - `+0x4A9`：`isRedRound`（`uint8_t`，`1` 表示红槽高倍率奖励轮）

### 锁序逆算公式
若目标三键排列为 `[b0, b1, b2]`（`{0,1,2}` 的任意全排列）：
- 设 `wantPos1` 为值 `1` 所在下标 → 令第 1 处 RNG 返回 `wantPos1`；
- 设 `wantPos2` 为值 `2` 所在下标，计算 `delta = (wantPos2 + 3 - wantPos1) % 3`（必为 `1` 或 `2`）→ 令第 2 处 RNG 返回 `delta - 1`（`0` 或 `1`）。

---

## 二、聊天指令与通知策略

所有指令通过游戏内聊天框发送（默认前缀 `$rng`）：

| 指令 | 说明 |
| :--- | :--- |
| `$rng steam` / `$rng steam status` | 查看提示开关、详细模式（`verbose`）、当前按键样式、锁序状态、上一轮结果与 6 种排列命中分布 |
| `$rng steam on` / `$rng steam off` | 开启或关闭蒸汽机按键实时提示（默认开启） |
| `$rng steam verbose [on\|off]` | 开关详细播报模式（别名 `v`，默认 `off`） |
| `$rng steam style <kb\|cn\|xbox\|ps\|num>` | 切换提示样式：键盘 `A-W-D`（默认）、中文 `左-中-右`、`xbox`、`ps`、`1-2-3` |
| `$rng steam pity <on\|off>` | 开关“隐藏必中保底触发”播报（默认开启；需同时开启 `verbose on` 才会在聊天栏推送） |
| `$rng steam lock [awd\|adw\|wad\|wda\|daw\|dwa\|r2\|off]` | 锁定固定按键顺序（不带参数默认锁为 `awd`，配合游戏内按住空格/RT/R2 自动输入）；`off` 恢复原版顺序 |
| `$rng steam reset` | 清空锁序并归零本局统计计数 |
| `$rng steam help` | 在聊天栏查看指令帮助 |

### 默认静默策略（`verbose = off`）
为避免长时间运行蒸汽机时聊天栏高频刷屏，`verbose` 默认为 `off`：
- **未锁序轮次（`!lockEnabled`）**：若提示开启（`on`），每轮仍会输出解码后的三键顺序，供手动按键参考。
- **已锁序轮次（`lockEnabled`）**：游戏内键位正常改写，但默认静默；仅当开启 `$rng steam verbose on` 时才会每轮播报 `[锁序]` 提示。
- **隐藏必中保底**：触发次数仍计入 `$rng steam status` 统计，但默认静默；仅当 `verbose on` 且 `pity on` 时才播报。
- **命令回执**：所有手动输入的 `$rng steam ...` 指令回执均正常输出，不受 `verbose` 影响。

> [!TIP]
> 若只需要读取原版随机生成的按键顺序而不改写任何 RNG 结果，可执行 `$rng observe` 将微内核切换为仅观测模式，插件仍会在每轮开始时输出按键顺序。

---

## 三、使用的 SDK v3.0 接口机制

1. **Pre + Post 联合回调与 `ObserveOnly` 穿透**：使用 `RegisterSlotHookEx` 与 `kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly`，在 `ObserveOnly` 模式下依然接收 `ctx->finalValue0` 通知。
2. **MASM 现场非易失寄存器与 SEH 读结构体**：通过 `ctx->callerRegs->rbx` 与 `ctx->callerRegs->rdi` 读取调用现场的 `cSteamControl*` 与中间局部变量 `pos1`，并在 `IsValidUserPtr` + `__try / __except` 保护下提取红槽/保底率/连胜信息。
3. **跨线程异步聊天投递**：RNG 回调可由任意游戏工作线程触发，插件在释放叶子锁 `g_stateMx` 后通过 `g_host->ChatPrintfAsync(...)` 投递至内核队列，再由 `sChat::move` 在主线程渲染到聊天窗口。
4. **生命周期钩子联动**：通过 `RegisterLifecycleHook` 监听 `$rng clearall`，在框架清空规则时同步解除 `steam` 插件自身的锁序状态。

---

## 四、独立构建与热重载

```bat
:: 1. 从核心仓库同步 include\mhw_rng_api.h（不传参默认查找 ..\..\include 或 %MHW_RNG_SDK_DIR%）
sync_sdk.bat <path-to-mhw-rng>

:: 2. 编译生成 build\rng_steam.dll（或带 --deploy 部署到游戏目录）
build.bat --deploy
```

部署后若游戏正在运行，在聊天框输入 `$rng reload steam` 或 `$rng load steam` 即可热重载生效。
