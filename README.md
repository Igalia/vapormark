`vapormark` is a benchmark framework developed for measuring various performance metrics (e.g., throughput, latency, and tail latency) and the process states (e.g., backend stall, energy consumption) while running a program on Linux. It especially targets `SteamOS` -- a Linux-based gaming device --but most features are genetically useful in regular Linux environments.


Three Phases
------------
`vapormark` consists of three phases:
  1. running a benchmark (i.e., collecting performance data)
  2. analyzing the collected data
  3. generating a report. 

Only the first step should run on a target device, such as `SteamDeck`. The others can be run on almost any Linux box.

External Dependencies
---------------------
`vapormark` uses the following in each phase:
  - running a benchmark (i.e., collecting performance data)
      - [schbench](https://kernel.googlesource.com/pub/scm/linux/kernel/git/mason/schbench/) for micro-benchmarking of scheduler performance
      - [MangoHud](https://github.com/flightlessmango/MangoHud) for measuring FPS (frame per second) during a running game
      - `strace`, `trace-cmd`, `cpupower`, and `perf` for collecting processor states
  - analyzing the collected data
      - `matplotlib` python library for generating graphs
  - generating a report
      - `pandoc` for generating a report in HTML format
  - for all phases
      - `python3`

Installation
------------
Just clone this repository and hit `make` on the top of the directory. The build procedure will clone and build `schbench`. All the binaries will be put under the `vapormark/bin` directory.

If you want to measure FPS, install `MangoHud`. For `SteamDeck`, please refer the following steps:
  1. Press `STEAM` button then choose `Power -> Switch to Desktop`
  2. On Plasma Desktop, launch `Discover Center`. Then find and install `MangoHud`
  3. Copy the ManguHud configuration file, `vapormark/config/MangoHud.conf` to `/home/deck/mangologs-vapormark`. This is the minimal MangoHud configuration that `vapormark` expects.

Running a benchmark and collecting performance data
---------------------------------------------------

#### `scmon`: collecting system usage of a process tree

`scmon` collects a system call usage of a process tree. It generates per-task system call trace file under `OUTDIR` with prefix `LOG` and suffix `-scmon.*`. It imposes noticeable performance overhead so it should not be used when collecting performance numbers. `scmon` is useful to understand the high-level behavioral traits of an application.

```
usage: scmon [-h] -o OUTDIR -l LOG [-p PID] [-r ROOT] [-n NAME] [-c CMD [CMD ...]]

Collect system call usage statistics of a program

options:
  -h, --help            show this help message and exit
  -o OUTDIR, --outdir OUTDIR
                        output directory
  -l LOG, --log LOG     log file prefix
  -p PID, --pid PID     process id to monitor
  -r ROOT, --root ROOT  root process id to monitor (all decendents will be monitored)
  -n NAME, --name NAME  name of a process to monitor
  -c CMD [CMD ...], --cmd CMD [CMD ...]
                        command to execute

For example, 'scmon -o log -l steam -n steam' to log the system call usage of 'steam' 
and all its decendents under log/steam*-scmon*.
```

#### `procmon`: collecting processor and scheduling statistics

`procmon` collects four types of information: 1) scheduler's wakeup events, 2) CPU's c-state, 3) CPU's energy consumption, and 4) processor's performance monitoring data (e.g., instruction per cycle). Similar to `scmon`, it generates logs under `OUTDIR` with prefix `LOG` and suffix `-procmon.*`. It collects information while it runs. The runtime overhead is not marginal so it can be run with an application level benchmark (like game). However, it is not recommended with a micro-benchmark (`schdbench`), which is much more sensitive to any noises.

```
usage: procmon [-h] -o OUTDIR -l LOG [-s] [-c] [-e] [-p] [-a]

Collect CPU statistics and system-wide scheduling statistics

options:
  -h, --help            show this help message and exit
  -o OUTDIR, --outdir OUTDIR
                        output directory
  -l LOG, --log LOG     log file prefix
  -s, --sched           trace wake-up events of process scheduler
  -c, --cstate          trace c-state of all CPUs
  -e, --energy          trace energy consumption of all CPUs
  -p, --perf            trace performance statistics of all CPUs
  -a, --all             trace all statistics

procmon internally uses 'trace-cmd', 'cpupower', and 'perf'.
```

#### `mbench`: running a micro-benchmark
`mbench` is a wrapper which runs `schbench` with a pre-configured settings. For convenience, it launches `procmon` if necessary. However, to get accurate performance results, it is recommended `mbench` with and without `procmon`. Also, make sure there is no heavy background tasks: for example, in `SteamOS`, `steam`, `mangoapp`, `gamemoded`, `gamescope`, and `steamwebhelper`.

```
usage: mbench [-h] -o OUTDIR -l LOG [-b BG] [-f FG] [-c CONFIG] [-r RUNTIME] [-p]

Run a micro-benchmark with a pre-configured setting

options:
  -h, --help            show this help message and exit
  -o OUTDIR, --outdir OUTDIR
                        output directory
  -l LOG, --log LOG     log file prefix
  -b BG, --bg BG        command line of a background task
  -f FG, --fg FG        command line of a foreground task for benchmarking
  -c CONFIG, --config CONFIG
                        run a benchmark with preconfigured setting: `schbench50`, 
                        `schbench100`, and `schbench200`, each of which runs `schbench` with 
                        50%, 100%, and 200% CPU utilization, respectively
  -r RUNTIME, --runtime RUNTIME
                        benchmark running time in seconds (default = 90sec)
  -p, --procmon         run with profiling on

Performance monitoring (-p) WILL interfere the results of micro-benchmark. Do NOT use -p when 
you collect performance results. Instead, run the same benchmark twice: one without profiling 
for performance comparison and another with profiling for analysis. Also, make sure there is 
no heavy background task running. 
```

#### `MangoHud`: measuring FPS, CPU/GPU utilization, etc.
Launching, starting, and stopping `MangoHud` is not integrated with `vapormark`. Hence `vapormark` just follows the standard `MangoHud` usage. Especially in `SteamDeck`, please refer to the following procedure:

- For a game to FPS logging, go to `Properties -> General -> Launch Options` and add `mangohud %command%`. *The game must be launched in **Desktop Mode (not in Gaming Mode)** to log FPS and other system stats.*

- Now, you will see the overlay window showing FPS when launching the game. You can start and stop FPS logging by hitting `Shift_L+F2`. The log will be stored at `/home/deck/mangologs-vapormark`. Some games hang when MangoHud is enabled. Other useful MangoHud shortcuts are as follows:

    ```
    Shift_L+F2 : Toggle Logging
    Shift_L+F4 : Reload Config
    Shift_R+F12 : Toggle Hud        
     ```

- Once you finish FPS logging by hitting `Shift_L+F2`, `MangoHud` will generate a `csv` log file under  `/home/deck/mangologs-vapormark`. Please copy and rename it ending with `-mangohud.csv` for analysis and report generation.


- Following games provide in-game benchmarks:

  | Game                   | How to start an in-game benchmark |
  | :--------------------- | :-------------------------------- |
  | Far Cry: New Dawn      | `Options -> Benchmark` |
  | A Total War Saga: Troy | `Options -> Graphics -> Advanced -> Benchmark` |
  | Cyber Punk 2077        | `Settings -> Graphics -> Quick Preset, Run Benchmark` |
  | Factorio               | On terminal: `factorio --benchmark` [map.zip](https://factoriobox.1au.us/map/download/91c009e61f44c3c532f7152b0501ea0fc920723148dd1c38c4da129eb9d399f9.zip) `--benchmark-ticks 1000 --disable-audio` |



Analyzing the collected data
----------------------------
Once the performance data is collected, it is time to analyze the results. In this phase, `vapormark` transforms various log files into the standard CSV format and produces the latency distribution graphs. Specifically, it provides the following commands. The generated files have a suffix of its program, `*-scinsight.*`, `*-procinsight.*`, and `*-ginsight.*`

#### `scinsight`: analyzing `scmon` logs
```
usage: scinsight [-h] -o OUTDIR -l LOG [-q]

Report system call usage statistics of a program

options:
  -h, --help            show this help message and exit
  -o OUTDIR, --outdir OUTDIR
                        output directory
  -l LOG, --log LOG     log file prefix
  -q, --quiet           do not print result to stdout
```



#### `procinsight`: analyzing `procmon` logs
```
usage: procinsight [-h] -o OUTDIR -l LOG [-q]

Report CPU statistics and system-wide scheduling statistics

options:
  -h, --help            show this help message and exit
  -o OUTDIR, --outdir OUTDIR
                        output directory
  -l LOG, --log LOG     log file prefix
  -q, --quiet           do not print result to stdout
```



#### `ginsight`: analyzing a `MangoHud` log
```
usage: ginsight [-h] -l LOG -o OUTDIR -p PREFIX [-q]

Generarte a report from MangoHud log

options:
  -h, --help            show this help message and exit
  -l LOG, --log LOG     MangoHud log file in a CSV format
  -o OUTDIR, --outdir OUTDIR
                        output directory
  -p PREFIX, --prefix PREFIX
                        output file prefix
  -q, --quiet           do not print result to stdout
```

Generating a (comparison) report
--------------------------------

`vapormark` provides a reporting feature that compares the results of multiple configurations. This is especially useful when checking the impact of a certain optimization. When more than one log directories are given (with multiple -l options), `report` uses the logs in the first directory as a baseline and shows the relative delta in percent. 

```
usage: report [-h] -l LOGDIR -p PREFIX -o OUTPUT [-f] [-g]

Generate a report of given log directories

options:
  -h, --help            show this help message and exit
  -l LOGDIR, --logdir LOGDIR
                        a log directory. When mulltiple `-l` options are given, comparison 
                        will be reported using the first one as a baseline.
  -p PREFIX, --prefix PREFIX
                        log file prefix for report generation
  -o OUTPUT, --output OUTPUT
                        target report file name in markdown format
  -f, --force           force to regenerate all CSV files
  -g, --debug           print out debug messages

For example, `report -l base_dir -l cmp_dir -p game1 -o report.md` compares `game1` logs 
in two directoreis -- `base_dir` and `cmp_dir` -- and generates `report.md`. `base_dir` 
is used in calculating the relative difference. When only one log directory is given, 
only the summary of results without comparison is provided. It expects certain file 
extensions: `*.factorio_out` for factorio benchmark and `*.schbench_out` for schbench 
benchmark.
```

Misc tools
----------

#### `sched-config`: save and restore key scheduler parameters from debugfs

```
usage: sched-config [-h] [-g GET] [-s SET]

Set or get the scheduler config parameters

options:
  -h, --help         show this help message and exit
  -g GET, --get GET  Get the scheculer parameters
  -s SET, --set SET  Set the scheculer parameters
sched-config: error: either '-g' or '-s' should be specified
```
