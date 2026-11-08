# Kernel exports that don't do the Xbox kernel's work

Some exports are accepted here without doing what the Xbox kernel does. Each
row says what happens instead, and why that is enough or what is missing.

Every function export has an implementation. One where the Xbox kernel's
behaviour isn't fully known or isn't needed here is marked with a TODO line in
the code, logs its first call, and has a row below. (Before every export had
one, a call to a missing export stopped the guest, "kunimpl"; the XBE loader
still names such imports, `hakuX-native-imports`, and `tools/native/imports.py`
lists them over a census, `tools/native/census.sh`.)

| Export | Here | Why |
|---|---|---|
| `KeStallExecutionProcessor` | returns at once | The emulated devices answer at once; a busy wait would only slow the guest. |
| `HalRegisterShutdownNotification`, `HalIsResetOrShutdownPending` | nothing, 0 | No shutdown in the guest: the app ends the process. |
| `HalWriteSMBusValue` | ignored, but for the SMC's scratch register (0x1B), which reads back | The SMBus devices' settings (the SMC's LED and fan, the video encoder) have no effect here. |
| `FscSetCacheSize`, `FscInvalidateIdleBlocks` | nothing | No file-system cache of its own: the host's page cache serves the disc and the drives. |
| `NtProtectVirtualMemory`, `MmSetAddressProtect` | recorded, not enforced | Guest memory has no per-page protection; `NtQueryVirtualMemory` reports what was set. |
| `MmUnmapIoSpace` | nothing | I/O space is never mapped. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion` | nothing | APCs are delivered only in alertable waits, never into a region. |
| `KeSaveFloatingPointState`, `KeRestoreFloatingPointState` | success | A thread's FPU state stays as it is across kernel calls. |
| `XcCryptService`, `XcUpdateCrypto` | 0, logged | No replaceable crypto vector. |
| `HalReadWritePCISpace` | reads zeros, writes ignored, logged | The PCI devices are emulated by their registers, not their config space. |
| `NtDeviceIoControlFile`, `NtFsControlFile` | known codes answered; any other fails (invalid device request), logged once | The known ones are what titles send: the disk's geometry and partition info, the DVD drive's SCSI pass-through (the Xbox security page), and a format's volume lock, unlock and dismount. |
| `KeReleaseSemaphore` past its limit | not released, logged once | The Xbox kernel raises STATUS_SEMAPHORE_LIMIT_EXCEEDED. |
| `MmDbgAllocateMemory`, `MmDbgFreeMemory`, `MmDbgQueryAvailablePages` | NULL, 0 pages freed, 0 pages; logged once | No debugger memory, as on a retail console; only a development kit's debug monitor uses it. |
| `HalRequestSoftwareInterrupt`, `HalClearSoftwareInterrupt` | a DISPATCH_LEVEL request runs the queued DPCs and the scheduler at the next kernel exit below DISPATCH_LEVEL; an APC_LEVEL request is ignored, logged once; clearing does nothing | No software interrupt controller: DPCs and thread switches come from the scheduler, APCs only in alertable waits; a request cleared too late costs only a scheduler pass that keeps the same thread. |
| `RtlExtendedLargeIntegerDivide` by 0 | 0 (remainder 0), logged once | The Xbox kernel raises the divide error; a title that divides by 0 would fault there anyway. |
| `KeRemoveQueue`, `KeInsertQueue`, `KeInsertHeadQueue` | a waiter gets an entry whenever there is one; the first time that goes past the queue's MaximumCount is logged | NT holds entries back while MaximumCount of the queue's threads run, a thread blocking elsewhere giving its slot back; that needs the queue's threads tracked through every wait. Here more of a queue's threads can be working on entries at once, taking turns on the one CPU. |
| `IofCompleteRequest` | after the completion routines, the IoStatusBlock, event, APC or completion port and the IRP's free happen in the call | The Xbox kernel does that in a kernel APC to the IRP's thread; here APCs run only in alertable waits, and this kernel's own I/O completes the same way. |
| `IofCallDriver`, `IoSynchronousDeviceIoControlRequest` to the kernel's own devices | the disc's sectors and the raw sectors of partitions 0 and 3-5 read and written as through a file; partitions 1 and 2 refuse raw access (invalid device request); the IOCTLs `NtDeviceIoControlFile` knows and the disc's drive geometry; any other code fails (invalid device request), logged once | The data and system partitions are host folders with no FATX image behind them. |
| `ObCreateObject`, `ObInsertObject` | the object gets a handle but its name (if any) isn't entered in the namespace; logged once | The namespace here holds directories, devices and symbolic links only, as `NtCreateEvent` and the other creates ignore names too. |
| `NtQueryDirectoryObject` | lists the kernel's symbolic links and devices, and the directories titles create | Objects created with a name aren't in the namespace (above). |
| `IoCreateFile` with IO_OPEN_TARGET_DIRECTORY | opens the named file, not its directory; logged once | The Xbox kernel's own rename opens the target's directory so; `NtSetInformationFile` renames by path here. |
