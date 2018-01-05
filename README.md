What is this?  This is:

1. Mono versions used by various Unity releases, modified to expose the debugging interface.
This was originally done by <https://github.com/0xd4d/dnSpy-Unity-mono/>. The interface is the standard Mono SDB protocol, nothing dnSpy specific (although dnSpy uses it), so any Mono debugger can be attached.
Also see [README-dnSpy.md](README-dnSpy.md) which is the original README for that part (building, how to update etc.)
2. A quickly hacked profiler, "hackprof".
Hackprof writes timing reports for the managed code. The rest of this README describes this.

## Motivation / WARNING

This project aims to provide a profiler for Mono code that runs embedded in a Release build of a Unity game. Usually this is of interest because you are writing a mod for it. In that case, neither the Unity editor profiler, nor standard C# profilers are available to you (Release builds do not have the profiler, you do not have the editor files, and so far nobody executed Unity games with native .net runtimes instead of the embedded Mono)

This works by swapping out the mono.dll used by the game (see [How to setup a Unity game for profiling section](#how-to-setup-a-unity-game-for-profiling)). **The modified mono.dll is not suitable for general gaming, expect crashes/hangs at any point. Some games may corrupt savegames (or other things) in these cases, backup them if they mean anything to you.**

My personal version was Cities:Skylines modding. As such, this repository currently only contains the modifications for the Unity version used by it (which was 5.6.3 around the beginning of 2018).

Regarding code quality (security/stability): **suck it**. I tried to match the coding quality of the average Unity game, which usually means: if you're lucky, the GC cleans up after you, but don't expect anything to deinitialize properly. I wrote it for myself to be good enough to do the job, and if it crashes half of the time or gets horribly slow, I'm fine with that and you should be too. About exposing debug interfaces to at least all processes on localhost (only localhost?): if you can place arbitrary code in C:/Program Files and that happens to be a problem for you, then the debug socket is not your biggest problem.

With that said, have fun experimenting around with this profiler :)

## Features (actual and planned)

Planned features = ~~crossed out~~

* logging profile mode: logging all/most method invocations + run time + who called who
* ~~sampling profile mode~~
* ~~configurable~~ filter (exclude/~~include~~) for method calls, to improve profiling performance and limit results to areas of interest
* generate report in callgrind format (view with KCacheGrind/QCachegrind/...) ~~and reduce its size enough so that qcachegrind doesn't crash (might get irrelevant with start/stop control and better filter lists)~~
* ~~select/deselect threads to be profiled~~
* ~~start/stop control of profiling (e.g. do not profile loading screen)~~
* ~~extract filename/line information for method calls~~
* ~~line-level profiling of selected functions~~

## Logging profile mode

It's heavy. This is to some degree unavoidable (games, especially when they're so highly modular as anything based on Unity (which isn't a bad thing), call tons of functions in a very short time).

However, many functions do not need to be traced, because their cost is negligible, adding lots of overhead while in reality execution cost is near zero. In a C# runtime, much more things involve method calls (usually optimized away) than you think:

* simple property getter, return private member unmodified, probably JITted away
* cheap but frequent syscalls (e.g. GetTickCount or equivalent, very common in games)
* accessing a primitive array is formally a method call
* some IO stream wrappers make use of a ReadByte method, which causes many method calls (even if there is additional buffering between stream and filesystem)
* floating point math is cheap unless you try to trace it

All of these are not relevant for most profiling tasks because tracing them makes them look heavier than they really are. Often they're also buried deep in the framework, so if you're just profiling your own mod to improve its performance, you have no control over these parts anyway. You also don't care about what implementation detail of the engine's LoadAsset() method causes it to be slow, because you can't fix it anyway, all you care about is that LoadAsset() takes long. Time spent in ignored methods is instead attributed directly to the next caller that wasn't ignored (that'd be LoadAsset()).

Useless function calls take time to intercept and record. At some point they must also be written to disk, and later they must also be processed by the reporting tool. Not ignoring basic/primitive methods causes gigabytes of data to be generated within seconds, and you won't even get to the main menu. Ignoring them gets you gigabytes of data and a main menu after a few minutes. Also ignoring most of the low-level game framework gets you only a small (2-10x) slowdown, good enough to play the game. You can further restrict to whatever is the bare minimum for your task.

Ignore rules can be arbitrarily complex, because they're only evaluated when a previously unseen method is recorded. The result is stored in a hash table and any further invocation will quickly find out that the method is to be ignored.

## How to setup a Unity game for profiling

As said, this works by replacing the embedded Mono runtime with a version that supports remote debugging. Thankfully 0xd4d has made the work of providing ready-to-compile code + compiled versions of mono.dll for Unity players of most/all versions (see <https://github.com/0xd4d/dnSpy/wiki/Debugging-Unity-Games>). This repository extends on that work, adding a profiler.

Replace the game's mono.dll (in `GAMENAME_Data/Mono` - there may be other copies around, but replacing this one should suffice) with a debug version. It is a good idea to keep the old version around for regular gaming, because the debug/profile version runs a bit slower even without a debugger attached and without profiling running. Even though you might be able to recover the original easily (e.g. Steam "Check file integrity") this still takes longer than just renaming the backup version.

Other than a slight slowdown (which intensifies under profiling/debugging), when launched, the game should now still run mostly normally and additionally expose 1. a Mono soft debugger interface on localhost:55555 where debuggers can attach and 2. a profiling control interface.

