# 导出产物消费指南（下游契约）

时间：2026-11-08（2026-11-09 更新：仓库内 `dist/` 改为明文 JSON）
适用版本：`moenotes_slidepuzzle 0.1.0`，`export` 子命令，schema 1

这份文档写给**读导出文件的下游程序**，不是给生成器维护者。它只描述盘上格式
和保证，不描述求解算法。

## 零、仓库内的 `dist/`

本仓库把全量 504 盘（84 首 × 3–8）的解法直接提交在 `dist/`，**明文 JSON，不压缩**。
下游可以直接按路径取，也可以 `curl` 单个文件，不必自己跑求解器：

```
https://raw.githubusercontent.com/<owner>/<repo>/master/dist/100050_5.jsonl
```

单文件最大约 157 KB（8×8 最难盘）。GitHub 的 HTTP 与 git 传输都会对文本做
压缩，所以仓库占用的体积远小于工作区的 20 MB。

盘面可以被**单独更新**：某个盘面找到更短解时，只替换那一个文件并更新
`index.jsonl` 的对应行，其余 503 个文件保持逐字节不变。因为导出器是确定性的
（见第五节），文件字节变化必然意味着解法真的变了。下游若要判断自己缓存是否
过期，比对 `index.jsonl` 里的 `solutionLength` / `solver` 即可。

## 一、目录布局

```
dist/
  index.jsonl               所有盘面的一行摘要
  100001_3.jsonl            歌曲 100001、3x3
  100001_4.jsonl            歌曲 100001、4x4
  ...
  100084_8.jsonl            歌曲 100084、8x8
```

文件名固定为 `<musicId>_<grid>.jsonl`。用 `--no-compress` 以外的默认设置
另外生成时后缀是 `.jsonl.gz`，两种形式下游都应能读。

**不要靠扫描目录发现盘面**，用 `index.jsonl`：它是唯一声明了「这一盘确实
完整生成过」的地方。

## 二、index 格式

每行一个 JSON 对象，无外层数组。压缩模式下是单 gzip member：

```json
{"musicId":100001,"grid":3,"file":"100001_3.jsonl","solutionLength":21,"solver":"exact-astar","quality":"optimal"}
```

| 字段 | 含义 |
|---|---|
| `musicId` | 歌曲 ID，与游戏 master 的 LiveMusic ID 一致 |
| `grid` | 盘面边长，3–8 |
| `file` | 相对 `<out>` 的文件名 |
| `solutionLength` | 步数，等于该文件 `step` 行的数量 |
| `solver` | 实际使用的求解器，见下表 |
| `quality` | 该解的保证等级 |

压缩模式下的 `index.jsonl.gz` **一定是单 gzip member**。这一点是刻意的：多数语言的 gzip
读取器默认只读第一个 member，早期版本每行一个 member，Java 的
`GZIPInputStream` 这类读取器只看得到第一行。`gzip -l` 也只报最后一个 member
的尺寸，会显示成远小于真实值。

## 三、每盘文件格式

gzip 的 JSONL：第一行 `meta`，之后每行一步 `step`，行数 = `solutionLength + 1`。

### meta 行

```json
{"type":"meta","schema":1,"musicId":100050,"grid":3,"jacket":"jkt_003_100050",
 "initial":[[0,3,6],[8,4,7],[1,-1,5]],
 "goal":[[6,7,8],[3,4,5],[0,1,-1]],
 "solutionLength":15,"solver":"exact-astar","quality":"optimal","elapsedMs":0}
```

| 字段 | 含义 |
|---|---|
| `type` | 恒为 `"meta"`；用来区分两类行 |
| `schema` | 格式版本，当前为 `1`。字段语义变化时会递增 |
| `musicId` / `grid` | 同 index，可用来校验文件没有被改名 |
| `jacket` | 封面资源名，如 `jkt_003_100050`；下游用它找切图 |
| `initial` | 起始盘面，屏幕序二维数组 |
| `goal` | 完成态，屏幕序二维数组 |
| `solutionLength` | 步数 |
| `solver` / `quality` | 这一盘实际走的路径，见第四节 |
| `elapsedMs` | 求解耗时。`--deterministic` 时恒为 `0` |

### step 行

```json
{"type":"step","index":1,"move":"U","tile":4,"from":[1,1],"to":[2,1],
 "board":[[0,3,6],[8,-1,7],[1,4,5]]}
```

| 字段 | 含义 |
|---|---|
| `index` | 1 起，连续 |
| `move` | **空格**移动的方向：`U`/`D`/`L`/`R` |
| `tile` | 被滑动的 tile 值 |
| `from` / `to` | 被滑动 tile 的 `[row, col]`，屏幕序 |
| `board` | 这一步之后的完整盘面 |

注意 `move` 描述空格、`from`/`to` 描述 tile。两者方向相反，做动画时用
`from`/`to` 驱动 tile，用 `board` 直接重绘则不需要理解任何一方。

### 盘面数值约定

- 值是 **tile 的正确位置索引**，不是 tile 编号。
- `index = y * grid + x`，原点在**左下**，`y` 向上（与游戏一致）。
- `-1` 是空格。
- 导出的二维数组是**屏幕序**：行 `0` 是画面上最上面一行。

也就是说，`board[r][c] == v` 表示屏幕上第 `r` 行第 `c` 列显示的是属于第 `v`
个正确位置的封面切片。渲染每一格 = 把封面按 `grid × grid` 切开，取第 `v`
片贴上去，`-1` 画空格底色。

## 四、`solver` / `quality` 的含义

| solver | quality | 含义 |
|---|---|---|
| `exact-astar` | `optimal` | 3x3/4x4，**证明最优** |
| `pdb-exact` | `optimal` | 5x5，PDB 精确搜索，**证明最优** |
| `pdb-w1.5` | `near-optimal` | 5x5，加权搜索，约最优的 1.1 倍 |
| `ladder` | `heuristic` | 5x5 及以上，加权 A\*，无最优保证 |

下游若只接受最优解，按 `quality == "optimal"` 过滤即可。3x3/4x4 永远是
`optimal`，5x5 在 `--strategy auto` 下通常是 `near-optimal`。

## 五、`--deterministic`

默认情况下 `elapsedMs` 记录真实耗时，同样的输入两次运行会产生**不同字节**。
解法本身是确定的（同一盘面每次得到同一条路径），只有这个时间字段在变。

需要内容寻址、去重、或「重新生成后校验一致」时加 `--deterministic`：该字段
恒为 `0`，整个目录逐字节确定。

```bash
moenotes_slidepuzzle export --out puzzles --deterministic --index
```

## 六、生成与校验

```bash
# 全量：84 首 × 3..8 = 504 盘
moenotes_slidepuzzle export --out dist --index --deterministic --no-compress

# 只做一部分
moenotes_slidepuzzle export --out puzzles --grid 5 --music 100001 --index

# 中断后继续（已完整的文件不重算）
moenotes_slidepuzzle export --out puzzles --index --resume

# 端到端校验：逐步重放、检查每步 board、检查终局、交叉核对 index
python3 tools/verify_export.py dist
```

`verify_export.py` 同时接受 `.jsonl` 和 `.jsonl.gz`，并会：

- 逐步重放，确认每一步的 `tile`/`from`/`to` 与 `board` 自洽，终局等于 `goal`；
- 交叉核对 index：index 列的必须存在、存在的必须被列、`solutionLength` 必须与
  文件实际步数一致；
- 检查后缀与真实编码是否匹配（`.jsonl` 里装 gzip 字节会报明确的错误，而不是
  一个 UTF-8 解码失败）。

### 增量更新单个盘面

某个盘面找到更短解时，不要手动 `cp` 覆盖——那样会让 index 与磁盘脱节。

```bash
python3 tools/update_board.py dist 100050 5 --strategy pdb
```

它会把该盘重新求解到临时目录，**只有当新解不更长时**才替换文件并更新
`index.jsonl` 的对应行，其余行与文件一律不动。重复执行是幂等的
（解相同时直接退出）。`--dry-run` 只报告不写入。

`--resume` 的完整性判定：文件存在、能解压到 EOF（gzip trailer 校验通过）、
meta 行的 `schema`/`musicId`/`grid`/`solutionLength` 与本次任务匹配。任一条
不满足就重建。截断的文件不会被误认为完整。

`--resume` 会保留 index 中本次不重新生成的行的 provenance（`solver`/`quality`），
不会把它们改写成 `cached`。损坏或字段非法的 index 行会被丢弃并在 stderr 报告
条数——丢弃是安全的，对应盘面下次运行时会被重建。

## 七、下游常见错误

1. **只读第一个 gzip member**。用 `zlib.decompressobj(wbits=31)` 循环、
   Python 的 `gzip` 模块、Node 的 `zlib.gunzipSync` 都没问题；自己写的
   inflate 循环要处理多 member。索引文件已保证单 member，盘面文件也是单 member。
2. **把 `move` 当成 tile 的方向**。它是空格的方向。
3. **把 `board` 当成 tile 编号**。它是「正确位置索引」，渲染时是切片下标。
4. **假设盘面是行主序、原点左上**。导出已经是屏幕序，直接按二维数组用即可；
   只有在和库的 `tilePositions`（原点左下、y 向上）互相转换时才需要翻转。
5. **用 `index` 之外的方式判断盘面是否齐全**。缺文件、旧 schema、截断文件都
   可能存在于目录里；以 index 为准。

## 八、容量参考（实测）

| 形式 | 全量 504 盘 | 最大单文件 |
|---|---|---|
| 明文 `.jsonl` | 18.6 MiB | 157 KB（8×8 最难盘） |
| gzip `.jsonl.gz` | 1.4 MiB | 54 KB |

按尺寸分布（明文）：3×3 + 4×4 + 5×5 合计约 15%，6×6 约 16%，7×7 约 28%，
8×8 约 41%。

每步都带完整盘面，比只存方向大约 30 倍，换来的是下游渲染无需自己重放状态。
仓库里存明文而不是 gzip：单文件可以直接被 HTTP 取用而无需客户端解压，
git 与 GitHub 的传输层压缩已能抵消大部分体积差。
