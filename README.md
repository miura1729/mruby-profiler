## What's mruby-profiler

mruby-profiler is a VM instruction level profiler for mruby. mruby-profiler
counts execution time and execution count per VM(RITE) instruction
by CODE_FETCH_HOOK.
mruby-profiler can customize format of result by mruby. mruby-profiler provides default formatter in mrblib/profiler.rb. Sample result shows in https://gist.github.com/miura1729/6972107. Document of customizing format don't exist yet sorry.

## How to use

### 1. get mruby-profiler
  git clone https://github.com/miura1729/mruby-profiler.git

### 2. Enable #define ENABLE_DEBUG in include/mrbconf.h

### 3. Add gems in build_config.rb

### 4. make

### 5. Execute your mruby program

### 6. Enjoy or cry

# Licence
 Same mruby's licence

# Author

 Miura Hideki (a plumber)

 @miura1729 (Twitter)

 d.hatena.ne.jp/miura1729
