# nice_backtrace

Instead of this:
```sh
$ ./a.out
a.out: example.cpp:10: <some loooong symbol>: Assertion `n > 0' failed.
[1]    1953 abort (core dumped)  ./a.out

$ coredumpctl debug 1953
(gdb) where
<stacktrace>
```
Do this:

![screenshot](./screenshot.png)

## How

Just `#include "backtrace.hpp"` before all other headers in the root source
file:

```c++
#include "backtrace.hpp"

// now include all other stuff you need here
#include <vector>
// ...

size_t factorial(size_t const n) {
  assert(n > 0); // will fail
  return n * factorial(n - 1);
}

int main() {
  factorial(3);
}
```

Then when compiling, be sure to link the GNU `dl` library:
```shell
$ g++ -g -ldl example.cpp
```

This requires **glibc** (>= 2.1) and GNU binutils on a POSIX system like linux,
BSD, or MacOS. It will not work on Windows.

## How it works

Glibc comes with a builtin `backtrace()` function that reads the return
addresses from the stack. Together with `dladdr`, these are converted to
physical offsets in the ELF text segment of the respective shared library or
executable. `addr2line` converts these into demangled symbols and source file
positions, if available.

The remaining threehundredsomething lines make it look pretty.
