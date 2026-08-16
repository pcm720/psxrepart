# PSX repartitioning utility

Reimplements the original XMB repartitioning behavior while allowing custom partition sizes between 2 GB and 137 GB.

## Usage

Run the utility, select your desired partition size via the remote or gamepad, and press X or O to begin.

The utility also supports a command-line argument to specify the partition size:

```
psxrepart.elf -size=<GB>
```

Where `<GB>` is the target size in gigabytes:
- Values ≤ 2GB will use the minimum LBA (0x400000)
- Values ≥ 137GB will use the maximum LBA28 limit (0xFFFFFFF)
- Intermediate values will be calculated proportionally

Example:
```
psxrepart.elf -size=80
```

When the argument is provided, the process will start automatically.
