# 一些细节

## 编译时

### 什么时候重新编译

三层判定：

1. `config.toml` 或注入头比 exe 新 → 重编译；
2. 否则 exe 比源文件新 → **不重编译**（连哈希都不算）；
3. 否则算源文件的 SHA-256，和 `.cache` 里记的比：一样 → 不重编译（只是 `touch` 了文件、内容没变），不一样或者没有记录 → 重编译。

`[build]` 那一行会带上原因，路径同样相对 `base`，比如 `[build] up-to-date (.coding\build\src\a.probe.exe, exe is newer)`、`[build] up-to-date (.coding\build\src\a.probe.exe, sha256 matches)`、`[build] built -> .coding\build\src\a.probe.exe (no exe yet)`。

### 缓存表 `.cache`

纯文本，一行一条，最近的排在最上面。键是**源文件相对 `base` 的路径**，不是文件名 —— 否则两个同名源文件会互相顶掉缓存记录：

```
# coding build cache: <sha256> <path relative to base>, most recently built first
9f2c... src\one\a.cpp
0b71... src\two\a.cpp
```

重编译成功后会先把旧的那条删掉、再把新的一条插到第一行，所以最近编译的文件永远在最好找、最好改的位置。缓存命中时不重写这个文件（免得每次运行都动磁盘），也没有条数上限。

> 热度处理说明：文件只有几十行时，查表本来就是线性的，排在最前面换不来多少速度；它真正的价值是「第一行就是上次编译的文件」以及手改方便。如果只想要一个映射，顺序其实无所谓。

### 旧 exe 删除失败时

重新编译要先把旧的 exe 删掉。Windows 上**正在运行的程序**和**没人回收的编译器进程**都会占住这个文件，这时删除会以**拒绝访问**失败，而重命名仍然可以。所以：

1. 先打印**失败原因**，然后问你要不要把旧 exe 挪进 **`.coding/build/.trash/`**（产物按目录分层，但 `.trash` 固定在 build 根下，不会在每个子目录里散落），文件名就是它自己的 **SHA-256**。这是重命名，不受占用影响。
2. 连重命名都失败就真没招了：打印说明、等你**按任意键**，然后以退出码 `2` 结束，不再继续编译。

## 运行时

1. **选源文件**：`[compiler].source` 可以是**一个目录**，也可以是**一个 `.cpp` 文件**（缺省是 `base`）。

   - 是**目录**时：递归扫描它，只认 `.cpp`（大小写不敏感），不进入 `.` 开头的目录（`.git`、`.coding` 之类），按**最后修改时间**挑出最新的一个；时间并列时取路径字典序小的。
   - 是**文件**时：直接用它。**只认 `.cpp`**，不是 `.cpp` 就报错。

   无论哪种，都会把选中的文件打印出来（路径**相对 `base`**；只有源文件在 `base` 之外时才显示绝对路径）：

   ```
   [source] src\one\a.cpp (modified 2026-06-01 12:00:00)
   ```

   路径不存在、既不是目录也不是普通文件、文件不是 `.cpp`、或者目录里没有任何 `.cpp` 时，报错后**等你按任意键**再以退出码 `2` 结束。

   注意一个边界：**名字叫 `foo.cpp` 的目录仍然是目录**，会被当目录扫描，而不是当成源文件。
   另外 `source` 会先判断是不是目录、再判断是不是文件 —— 所以指到目录时，目录里挑哪一个是按上面的规则，不受你给这个名字的影响。
2. **按需编译**（见下文「什么时候重新编译」）。
3. **运行**。产物和运行目录全部在 `<base>/.coding/` 下。

### stdout / stderr 的分离

两条独立管道**无法**保证 stdout 和 stderr 的先后顺序，先写的那一路可能后到。所以 `cg_s` / `cg_i` 换了个做法：

1. **编译期**：把 `inject/probe.h` 强制塞进翻译单元（`g++` /`clang++` 用 `-include`，`cl.exe` 用 `/FI`）。它接管 `cout` /`cerr` / `clog` 以及 `printf` 这一族 C 输出函数，把所有输出写进**同一条**带标记的流：`\x01` 表示 stdout、`\x02` 表示 stderr，且只在流切换时打一次标记。
2. **运行期**：`cg` 解析这条流，把字节还原成两路——顺序天然就是程序产生的顺序。

于是 `cg_s` / `cg_i` 里 stdout 绿色、stderr 红色，像终端里那样正确交织。

接管是靠宏做的，所以这些细节值得知道：

| 情况                                                           | 行为                                     |
| -------------------------------------------------------------- | ---------------------------------------- |
| `printf(...)` / `::printf(...)`                                | 接管                                     |
| `std::printf(...)` 这类**限定名**写法                          | 接管，定义 `PROBE_NO_STD_ALIASES` 可关掉 |
| `std::cout` / `std::cerr` / `std::clog`                        | 接管                                     |
| `sprintf` / `snprintf` / `vsnprintf`                           | **不接管**                               |
| `fprintf(f, …)` / `fputs(…, f)` 里的 `f` 是 `fopen` 出来的文件 | **原样转发**，不受影响                   |
| 多线程同时输出                                                 | 合并流内部有锁                           |
| `std::wcout` / `wprintf` 等宽字符输出                          | **不接管**，直接写原句柄                 |
| 先 `freopen` 重定向过 stdout                                   | 仍写原来的句柄                           |

缓冲遇到换行立刻 flush，所以程序中途崩溃也不会吞掉已有输出；`ExitProcess` 这类硬退出除外。

### 交互模式下的粘贴

粘贴**只回显，不自动提交**：剪贴板内容会自动去掉最末尾的换行（`\r\n` 统一成 `\n`），整段显示在输入区上（多行就占多行，状态行跟着下移到输入区下面），**按下回车才整块写给程序**，然后程序的输出才出现。

## 数据维护方式

### `.coding/` 结构

| 路径                                          | 内容                               |
| --------------------------------------------- | ---------------------------------- |
| `.coding/build/<相对目录>/<文件名>.exe`       | 原版可执行文件                     |
| `.coding/build/<相对目录>/<文件名>.probe.exe` | 注入版可执行文件（默认跑的就是它） |
| `.coding/build/.cache`                        | 构建缓存表                         |
| `.coding/build/.trash/`                       | 删不掉的旧 exe 挪到这里            |
| `.coding/work/`                               | 运行目录                           |

**产物按源文件相对 `base` 的目录结构摆放**，`<文件名>` 是源文件去掉 `.cpp` 之后的名字。所以两个同名源文件各有各的产物，不会再互相覆盖：

```
src/one/a.cpp   ->  .coding/build/src/one/a.probe.exe
src/two/a.cpp   ->  .coding/build/src/two/a.probe.exe
```

源文件在 `base` 之外时退到 `.coding/build/_external/<盘符>/<其余路径>/`，同样稳定、不撞车，也不依赖当前工作目录。

`[compiler].output`、`[compiler].output_probe`、`[runner].work_dir` 已经从配置里删掉，写了也会被忽略（stderr 上会提示一行）；`[io].single_input` / `[io].single_output` 不变，仍按 `base` 解析。

## 关于配置

### 路径基准与配置文件的链接

- **`base`**：文件里所有相对路径都以它为起点，可以写绝对路径，也可以写相对路径（相对的 `base` 以配置文件所在目录为起点）。不写 `base` 时，相对路径就按**配置文件所在目录**解析。`base` 不受自身影响，配置文件本身的路径也不受它影响。
- **`config.link`**：想在多处复用同一份配置，就把配置放在任意位置，然后在 `.exe` 旁边放一个 `config.link` 文本文件。它**不是 TOML**，内容规则是：

```
C:/path/to/config.toml
```

第一个非空、不以 `#` 开头的行就是配置文件路径：可以写绝对路径，也可以写相对路径（相对路径以
**这个链接文件自己所在目录**为起点）。路径两旁多余的空格、成对的引号和 UTF-8 BOM 都会被忽略，所以直接粘贴就能用；写多行时只有第一行生效。文件为空或只有注释会**直接报错**，不会静默地当作没有链接。

链接还可以**串联**：如果它指向的文件名也以 `.link` 结尾，就继续跟着那个文件走，直到指向一个不叫 `.link` 的文件，那就是配置文件。所以一层层复用是合法的：

```
bin/config.link  ->  ../shared/team.link  ->  C:/path/config.toml
```

每一跳的相对路径都以**它自己所在目录**为起点。

链上的问题都会指名道姓地报出来，绝不静默退回别的配置：**成环**（`link loop: a.link -> b.link -> a.link`；同一个文件的不同写法——绝对/相对、含 `..`、大小写不同——都会被认成同一个）、指向**不存在的文件**、链接里**没有路径**、以及超过 **16 跳**。工具查找配置的顺序是：

1. **`.exe` 旁边的 `config.toml`**；
2. **`.exe` 旁边的 `config.link`** 以及它串起来的整条链。

用真 symlink 把 `config.toml` 链到别处也可以，那种情况下 Windows 自己就解析了，不需要 `config.link`（symlink 需要管理员或开发者模式，`config.link` 不需要）。跟着链接找到配置时会在 stderr 打一行 `[config] using <real config.toml> (followed <first> -> <second> -> ...)`。

支持将链接到包含配置文件的目录，能自动查找目录下的 `config.toml`。

## 关于守护进程

### 已知限制：一个会话只能有一个守护进程

### 热键冲突

`Ctrl+Alt+字母` 是很抢手的组合，输入法、显卡驱动、厂商工具都可能已经占着。注册失败不会静默：

```
[daemon] cannot register Ctrl+Alt+I for interactive: already taken by another program
```

改 `config.toml` 的 `[daemon]` 段即可，留空字符串表示关掉那个热键。**改完跑一次 `cg_t --status`**，它会逐个列出注册结果（包括失败原因）；启动就失败的情况下这份状态也会保留，`--status` 会以 `last run:` 标出。

键名写法：`Ctrl`/`Control`、`Alt`、`Shift`、`Win`/`Super`，键可以写 `A`-`Z`、`0`-`9`、`F1`-`F24` 以及 `Space`/`Tab`/`Left`/`Home`/`PageUp` 这类具名键。**必须带至少一个修饰键**，裸键会被拒绝（那等于全局劫持打字）。

### 进程日志

守护进程把「某次任务起了 / 它编译出来的程序起了 / 任务结束了」按日志式追加到
**stdout** 和 **`<base>/.coding/daemon.log`** 两处。
每行带本地时间戳：

```
[2026-10-24 15:25:41] daemon: started (pid 15736)
[2026-10-24 15:25:42] single: job started (pid 13952)
[2026-10-24 15:25:44] single: program started (pid 11996)
[2026-10-24 15:25:46] single: job finished (exit 0) output saved: no
[2026-10-24 15:25:58] interactive: job started (pid 13012)
[2026-10-24 15:25:58] interactive: program started (pid 16172)
[2026-10-24 15:26:03] interactive: already running (pid 13012), focused
[2026-10-24 15:26:17] interactive: job finished (exit 1) input saved: no, output saved: no
```

- `job` 是任务进程（`cg_t.exe --job i|s`），`program` 是它编译出来的那个程序 ——
  后者是孙进程，守护进程看不到，由任务进程通过一块共享内存报回来。
- 结束行报告**是否真的写了输入/输出**（交互模式两者都报，单文件模式只报输出）。
  如果任务是被强杀或没来得及报告，会写 `no report`。
- 日志超过 512 KB 会在下次启动时清空。

单实例互斥体、停止事件、槽位共享内存用的都是 `Local\minimalCoding.cg_t.*`，
**不区分项目目录**。所以：

- 同一会话里第二个 `cg_t` 会说 `already running` 然后退出，即使它指向另一份
  `config.toml`；
- 如果已经在跑的那个是**提权**启动的，非提权的 `cg_t --stop` 会失败
  （`OpenEvent` 被拒绝），只能从任务管理器结束。

要同时守护多个项目，需要把命名空间改成按配置路径派生（比如拼上 `config.toml`
的哈希）。