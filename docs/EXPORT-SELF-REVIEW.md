# 分支自查发现的问题（待与审查结果对照）

时间：2026-11-07
分支：`slide-puzzle-offline-export`

## 我自己测出来的

### 1. 写入失败完全没有检测（严重）

`sink_printf` 返回 `void`，内部调用 `gzvprintf` / `vfprintf` 后丢弃返回值。
`json_board`、`write_steps`、meta 行全部经由它写出。

后果：磁盘写满或配额超限时，`gzclose` 之前不会有任何错误信号，最终
可能产出一个**语法上存在但内容截断**的文件。更糟的是 `--resume` 的
`cache_is_valid` 只看 gzip 魔数和文件大小 > 64 字节，会把这种文件当成完整。

验证方式：项目没测过。可以用 `ulimit -f` 限制文件大小来复现。

修复方向：`sink_printf` 返回状态；`write_steps` 在任一写失败时返回错误；
`export_board` 已有失败清理路径（`remove(temp)`），接到这条链上即可。

### 2. 未知 musicId 不被拒绝（中等）

`export --music 999999 --grid 3` 会正常生成一个盘面并导出：

```
999999 3x3  11 moves  exact-astar (optimal, 0 ms)
{"type":"meta",...,"musicId":999999,"jacket":"","initial":...}
```

`mnsp_generate` 对任意 ID 都能算出盘面（ID 只当随机种子），所以这不是崩溃，
但对下游是误导：这个 ID 不对应任何真实歌曲，`jacket` 为空。

修复方向：用 `mnsp_jacket_asset_name(id)` 判断，未知 ID 拒绝并给出可用范围提示。

### 3. 重复的 --music 会重复导出（轻微）

```
--music 100001 --music 100001  ->  2 planned, 2 written
```

结果正确（第二次覆盖第一次），但浪费工作。5×5 用 PDB 时每个重复项都要重新求解。

修复方向：解析时去重。

### 4. 并发锁的 stale lock（轻微，已在文档提示）

锁文件用 `fopen(path, "wx")`。进程被 `kill -9` 后锁会残留，
下次运行会被拒绝，需要手工删除。错误消息已经提示了这一点，可以接受，
但更好的做法是把 pid 写进锁并在发现锁时检查该 pid 是否还活着。

## 已验证没问题的

| 项 | 方法 | 结果 |
|---|---|---|
| gzip 完整性 | `gzip -t` 全部 421 个文件 | 0 损坏 |
| JSON 严格性 | Python `json.loads` 抽查 20 文件 | 0 非法 |
| **完整重放** | 420 个盘面逐步重放 | **0 问题** |
| 每步语义 | from 位置是声明的 tile，to 是空格 | 全通过 |
| 中间盘面 | 每步 board 字段与实际一致 | 全通过 |
| 终局 | 最终 board == goal | 全通过 |
| `--grid` 越界 | grid=2 / grid=9 | 正确拒绝 |
| 超长路径 | 300 字符目录名 | 正确报错 |
| 损坏文件 + resume | 写入 garbage 后重跑 | 正确重建 |
| 默认输出目录 | 无 `--out` | 创建 `./puzzles` |
| 锁释放 | 正常运行后 | 锁文件已删除 |
| 并发拒绝 | 预置锁文件后运行 | 拒绝并提示 |

## 重放校验脚本

`tools/verify_export.py`，可对任意导出目录做端到端验证：

```
python3 tools/verify_export.py /tmp/export_full --quiet
421 files checked, 0 with problems
```

这个脚本值得保留在仓库里 —— 它是唯一能验证「下游实际体验」的检查。
