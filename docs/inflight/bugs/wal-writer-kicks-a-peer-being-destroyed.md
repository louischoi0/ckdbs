# The WAL writer can kick a peer whose scheduler is being destroyed

**What is wrong.** The WAL writer thread kicks every core with a durable
waiter after each sync (`WalWriter::KickDurableWaiters`,
`src/wal/writer.cpp`), through the instance's `WakerTable`. That thread
lives until `wal_` is destroyed. Peers are destroyed before that:
`Expeditor::StopStartedCores` clears `cores_`, and `TwoCoreRig` resets its
cores before `wal_`. A peer's durable waiter is a `DurableWait` in a
coroutine frame that stays parked after the reactor stops, and the frame is
destroyed with the scheduler's members, after `~Scheduler`'s body. The
writer also calls `done_.notify_all()` before it kicks, so a peer's
`Sync()` in the shutdown tail can return while the writer is still in the
middle of kicking. Two interleavings follow:

- the writer reads `entries_[core]` while `~Scheduler` writes it through
  `WakerTable::Unregister` (a data race on the entry), or
- the writer reads the entry just before the unregister and calls `Wake()`
  on a `Waker` that is being destroyed (a use-after-free).

**Verified at** `fix-waker-table-unregister` on top of `4f175ba9`, by
reading the code (the critics-developer review of the waker-table
unregister fix). No cell reproduces it: the window is one kick wide and
only opens when a peer is torn down with a parked durable waiter.

**Where it came from.** It existed before the unregister fix. Before it,
the entry dangled for the rest of teardown, and every later kick read freed
memory (the heap-use-after-free BS-S7's history cells found under ASan).
The fix closes that path for kicks issued on the teardown thread. It
narrows the writer's window to the destructor itself, and does not close
it.

**Cost.** A crash or a sanitizer report at shutdown. No answer is wrong and
no data is written: by then the instance has synced and stopped serving.

**The fix, known and unscheduled.** The writer must stop kicking before
any peer runtime is destroyed. Clearing its registry
(`WalManager::SetWakeRegistry(nullptr)`) is not enough on its own: a kick
in flight has already loaded the pointer. The writer has to be quiesced
first, for example by kicking before `done_.notify_all()` and then
waiting for one more writer round after the clear. That changes the
shutdown order of `Expeditor` and the test rigs, so it is left for a work
order of its own and was not folded into BS.
