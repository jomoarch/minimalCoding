# Coding

这是一个面向 Windows OIer 的**编译+运行**工具，支持 `interactive` 模式和 `signle` 模式。
> 它不判题。

## 可执行文件一览

| 工具   | 用途                                      |
| ------ | ----------------------------------------- |
| `cg_i` | 编译一次后直接再控制台交互运行            |
| `cg_s` | 编译一次，用 `[io].single_input` 作为输入 |
| `cg_t` | 后台常驻，按全局热键触发上面两者          |

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

## 热键守护进程：cg_t

`cg_t` 常驻后台，按全局热键触发一次 `cg_i` / `cg_s` 的运行。

### 用法

```sh
cg_t
cg_t --status
cg_t --stop
```

默认热键：`Ctrl+Alt+I` 交互模式、`Ctrl+Alt+S` 单文件模式、`Ctrl+Alt+Q` 退出。

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
| `[runner].time_limit_ms`   | CPU 时间上限；不写就永远不杀超时程序                                                                                             |
| `[runner].memory_limit_mb` | 内存上限；`0` 表示不限                                                                                                           |
| `[io].single_input`        | `cg_s` 的输入文件                                                                                                                |
| `[io].single_output`       | 程序输出落盘的位置                                                                                                               |
| `[io].colorize_output`     | 是否给程序自身输出染色（默认 `true`）                                                                                            |
| `[io].merge_stderr`        | 是否把 stderr 也写进输出文件；不写就各自维持现状（见上文「stderr 要不要进输出文件」）                                            |
| `[io].print_input`         | 回显时是否先把输入打出来；默认 `false`                                                                                           |
| `[daemon].interactive`     | `cg_t` 触发交互模式的热键，默认 `Ctrl+Alt+I`；留空关掉                                                                           |
| `[daemon].single`          | `cg_t` 触发单文件模式的热键，默认 `Ctrl+Alt+S`；留空关掉                                                                         |
| `[daemon].quit`            | `cg_t` 退出热键，默认 `Ctrl+Alt+Q`；留空关掉                                                                                     |

可执行文件、运行目录与缓存表都由程序在 `<base>/.coding/` 下生成，**不能**再用配置指定；旧配置里的 `[compiler].output`、`[compiler].output_probe`、`[runner].work_dir` 会被忽略。