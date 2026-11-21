# Coding

这是一个面向 Windows OIer 的**编译+运行**工具，支持 `interactive` 模式和 `signle` 模式，使用前请确保机器上装有 `g++`。

它与 `CPH` 有部分重合，舍弃了 Special Judge、批量测试和文件对比，有以下优点：
1. 按照 SHA-256 增量编译，`probe.h` 预编译为 `.gch` 文件节省编译时间
2. interactive 式的运行
3. 严格保证输出顺序的 stdout-stderr 分离
4. 实时的带颜色的 stdout-stderr 区分
5. linedit 行编辑器能较好地处理瞬间的大量输入
6. 实时的 status_line 便于判断 interactive 下程序状态

> 它不判题。

## 环境搭建

`g++` 请自行解决。

你可以将 `config.toml` 文件放在 `cg_*` 文件的同目录下，或在`cg_*` 同目录下使用 `config.link` 链接 `config.toml`。

搭建环境时需要指定 `probe.h` 路径，可以将其放在 `.inject/probe.h`，若不想使用 `probe.h` 分离 stdout 和 stderr，请将 `[inject].enabled` 设为 `false`。



## 可执行文件一览

| 工具   | 用途                                         |
| ------ | -------------------------------------------- |
| `cg_i` | 编译一次后直接再控制台交互运行               |
| `cg_s` | 编译一次，用 `[io].single_input` 作为输入    |
| `cg_t` | 后台常驻，按全局热键触发交互 / 单文件 / 批量 |

## 交互模式的编辑器

`cg_i` 的行编辑器在 [`linedit/`](linedit/) 里。

它往下只依赖 `base/text.hpp`（UTF-8 解码与显示宽度）和 `base/win_error.hpp`，
细节见 [`linedit/README.md`](linedit/README.md)。

## 构建

```sh
mkdir build
cmake -S . -B build
cmake --build build --config Release
```

默认把 C/C++ 运行库**静态链接**进 exe，所以 `bin\*.exe` 拷到没装 MinGW 的机器上也能直接跑。想要动态链接：`cmake -S . -B build -DMINIMALCODING_STATIC_RUNTIME=OFF`。

## 行编辑操作

因为 `cg_i` 接管了控制台的输入，控制台自己那套行编辑（退格、Ctrl+V 粘贴、历史）就不再生效了，
所以这些由我们自己实现：

| 按键               | 行为                                            |
| ------------------ | ----------------------------------------------- |
| `Ctrl+V`           | 粘贴，回车才传给程序                            |
| `Ctrl+Z`           | 发送 `EOF` 信号结束输入，当前缓冲区不为空时无效 |
| `Ctrl+C` / `Esc`   | 杀掉子进程，回到结果输出                        |
| `←` / `→`          | 按**字符**移动                                  |
| `Ctrl`+`←` / `→`   | 按**单词**移动                                  |
| `Enter`            | 提交当前行，并把这一行留在屏幕的转录里          |
| `Shift`+`Enter`    | 换行，不提交                                    |
| `Backspace`        | 按**字符**退格                                  |
| `Delete`           | 按**字符**删除                                  |
| `Ctrl`+`Backspace` | 按**单词**退格                                  |
| `Ctrl`+`Delete`    | 按**单词**删除                                  |

## 批量模式

批量模式挂在 `cg_t` 上：按 `Ctrl+Alt+B` 后 `cg_t` 用 `--job b` 启动一次批量任务，
先弹出选择文件夹的窗口；窗口会从上一轮用过的测试目录开始，那个目录不存在时回到 `base`；
选中测试用例目录后：

- 每个点优先用 `<name>.ans` 作为期望答案，没有则用 `<name>.out`；程序输出与它比较，
  不同记 `WA`，没有期望答案记 `NA`；
- 前面的信息输出和 `cg_s` 一致：`[source]`、`[build]`、`[io] input <- <目录>`；
- 用例名和结果按同一判定染色：`AC` 绿、`WA` 红、`RE` 粉，`TLE` / `MLE` / `NA` / `SE` /
  `KE` / `SK` 默认色；条、时间、内存不着色；
- 运行中的表按时间限制比例画条；未开始的行只有空条和空时间；`time_limit_ms = 0` 时条内
  的 `#` 段循环向右滚动，时间列显示 CPU 耗时（不超过 9999ms 用 `ms`，超过换成一位小数的
  `s`，数字和单位都右对齐）；跑完后按非 `KE`/`SK` 用例的最长 CPU 用时相对填充（最长的
  顶满），被 `Ctrl+C` 杀掉的条保持空；
- 运行中按 `Ctrl+C` 会杀掉当前正在跑的用例（记 `KE`），队列里没跑的继续跑完；没开始就
  结束的记 `SK`；
- 非控制台（管道、重定向）下不原地刷新，跑完按同样格式打印最终表；
- 跑完询问是否记录输出；选 `y` 会在 `[batch].output` 下创建 `<YYYYMMDD_HHMMSS>/`，
  每个点写一个 `<name>.out`。

不想弹窗时可以直接指定目录：

```sh
cg_t --job b --dir path\to\cases
```

`[batch].thread_max` 控制并发测试点数，同时受 CPU 核心数限制。

## 热键守护进程：cg_t

`cg_t` 常驻后台，按全局热键触发一次 `cg_i` / `cg_s` 的运行。

### 用法

```sh
cg_t
cg_t --status
cg_t --stop
```

默认热键：`Ctrl+Alt+I` 交互模式、`Ctrl+Alt+S` 单文件模式、`Ctrl+Alt+B` 批量模式、`Ctrl+Alt+Q` 退出。

### 退出

三条独立的路，任意一条失效都还有别的：

| 方式            | 说明                       |
| --------------- | -------------------------- |
| `Ctrl+Alt+Q`    | 退出热键，默认值，可改可关 |
| `cg_t --stop`   | 给命名事件发信号，可脚本化 |
| 控制台 `Ctrl+C` | 前台手动启动时有效         |

退出时会**注销热键、清理状态文件**，但**不会杀**正在跑的任务——用户按热键拉起来的程序不该因为守护进程退出就消失。

## 配置

复制仓库里的 `config.toml` 改一份自己的，**所有键都是可选的**。

可使用 `config.link` 链接与可执行文件不同目录的配置文件，该文件内部只需要填写目标配置文件或其他链接文件的绝对路径或相对路径。

### 全部键

| 键                         | 说明                                                                                                                             |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `base`                     | 文件里所有相对路径的起点；缺省为配置文件所在目录                                                                                 |
| `[compiler].source`        | 源**目录**或单个源**文件**；目录则递归挑最新的 `.cpp`，文件则只认 `.cpp`。缺省为 `base`                                          |
| `[compiler].args`          | 完整编译命令；源码与 `-o <output>` 会自动追加。默认 `g++ -std=c++17`                                                             |
| `[compiler].force_rebuild` | `true` 时每次运行都重新编译，默认 `false`                                                                                        |
| `[inject].enabled`         | 是否注入 `probe.h`（默认 `true`）；关掉就不需要头文件                                                                            |
| `[inject].header`          | 要注入的头文件；缺省按 `base`、配置文件目录、`.exe` 目录依次找 `include/inject/probe.h`，找不到再找同名目录下的 `inject/probe.h` |
| `[runner].time_limit_ms`   | CPU 时间上限；单文件/批量另有 `3×上限 + 2s` 的墙钟兜底                                                                           |
| `[runner].memory_limit_mb` | 内存上限；`0` 表示不限                                                                                                           |
| `[io].single_input`        | `cg_s` 的输入文件                                                                                                                |
| `[io].single_output`       | 程序输出落盘的位置                                                                                                               |
| `[io].colorize_output`     | 是否给程序自身输出染色（默认 `true`）                                                                                            |
| `[io].merge_stderr`        | 是否把 stderr 也写进输出文件；不写就各自维持现状（见上文「stderr 要不要进输出文件」）                                            |
| `[io].print_input`         | 回显时是否先把输入打出来；默认 `false`                                                                                           |
| `[io].echo_limit_kb`       | 输出超过这么多 KB 就不打印，跑完再问要不要看；`0` 表示不限（默认 `256`）                                                         |
| `[io].ask_save_input`      | 跑完是否询问保存输入；关掉不问（默认`true`）                                                                                     |
| `[io].ask_save_output`     | 跑完是否询问保存输出；关掉不问（默认`true`）                                                                                     |
| `[io].ask_copy_output`     | 跑完是否询问把输出复制到粘贴板（默认 `false`）；复制的内容与写文件那份一致，stderr 是否包含同样看 `merge_stderr`                 |
| `[batch].output`           | 批量输出的根目录；保存时在它下面建 `<YYYYMMDD_HHMMSS>/`。不写则跑完不保存（会提示未配置）                                        |
| `[batch].thread_max`       | 批量并发测试点数，默认 `4`，同时受 CPU 核心数限制                                                                                |
| `[daemon].interactive`     | `cg_t` 触发交互模式的热键，默认 `Ctrl+Alt+I`；留空关掉                                                                           |
| `[daemon].single`          | `cg_t` 触发单文件模式的热键，默认 `Ctrl+Alt+S`；留空关掉                                                                         |
| `[daemon].batch`           | `cg_t` 触发批量模式的热键，默认 `Ctrl+Alt+B`；留空关掉                                                                           |
| `[daemon].quit`            | `cg_t` 退出热键，默认 `Ctrl+Alt+Q`；留空关掉                                                                                     |

可执行文件、运行目录与缓存表都由程序在 `<base>/.coding/` 下生成，**不能**再用配置指定；旧配置里的 `[compiler].output`、`[compiler].output_probe`、`[runner].work_dir` 会被忽略。
