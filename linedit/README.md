# linedit

从 `cg_i` 里抽出来的行缓冲编辑器。

## 内容

| 头文件                                               | 作用                                                  |
| ---------------------------------------------------- | ----------------------------------------------------- |
| [`key.hpp`](include/linedit/key.hpp)                 | 语义按键 `Key`、`Size`、`Screen`                      |
| [`session.hpp`](include/linedit/session.hpp)         | 控制台会话：切原始模式、读按键、拿尺寸、写字节        |
| [`editor.hpp`](include/linedit/editor.hpp)           | 行缓冲区：光标、UTF-8 编辑、词级移动/删除、按行排版   |
| [`bar.hpp`](include/linedit/bar.hpp)                 | 屏幕编排：程序输出往上滚，输入行 + 状态行钉在下面两行 |
| [`status_line.hpp`](include/linedit/status_line.hpp) | 状态行里的旋转木棍                                    |

依赖方向是单向的：`Editor` 不认识 `Bar`，`Bar` 不认识 `Editor`（它只接受
「要显示的文本」），`session` 不认识上面任何一层。只有 `session.cpp` 和
`bar.cpp` 碰 `HANDLE`。

## 功能

- 插入 / 退格 / 删除，都按 UTF-8 字符边界走
- 左右按字符移动，Ctrl+左右按词移动，Ctrl+退格/Delete 按词删除
- Enter 提交（返回 `Outcome::Submitted`，补不补换行由宿主决定）
- Shift+Enter 插入换行；`Esc` / `Ctrl+C` 报中断
- Ctrl+Z 在空行上报 `Outcome::EndOfInput`，非空行忽略
- 粘贴只插进缓冲区、不自动提交（`clean_paste` 统一换行、去掉末尾换行）
- Shift+Enter 插入的换行会让输入区占多行；**超宽的行是截断，不是折行**
- 输入多行时状态行跟着下移；终端过小或光标不可定位时 `open` 失败

## 对宿主的要求

只有两个叶子工具，都是向下依赖（`linedit` → `base`）：

- `base/text.hpp` 的 `decode_utf8` / `measure` / `display_width`（UTF-8 解码与东亚宽度）
- `base/win_error.hpp` 的 `last_error_string`

要搬到别的工程，替换这两个头即可；`linedit` 自己的头文件都不包含它们。
