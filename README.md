<div style="display:flex;align-items:center;justify-content:space-between;">
<h1 style="display:inline-block;">KeepON-rpi</h1>
<img src="https://i.postimg.cc/2STCXFnM/temp-Imageu-Rzj-Bl.avif" width="40" alt="Logo"/>
</div>

A prototype implementation of KeepON on the bcmgenet driver of RPi 4B

```bibtex
@misc{xue2025keepon,
      title={Supporting Deterministic Traffic on Standard NICs}, 
      author={Chuanyu Xue and Tianyu Zhang and Andrew Loveless and Song Han},
      year={2025},
      eprint={2506.17877},
      archivePrefix={arXiv},
      primaryClass={cs.NI},
      url={https://arxiv.org/abs/2506.17877}, 
}
```

Paper: https://arxiv.org/abs/2506.17877



# Build

Compile as module:

```bash
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules EXTRA_CFLAGS="-DENABLE_DUAL_CLOCK_MODE=0"
```

> [!NOTE]
> You must copy `/drivers/net/ethernet/broadcom/unimac.h` to an upper-level path when compiling as a module.


After successful installation, you should see the emulated PTP hardware clock (EPHC):

```bash
ethtool -T eth0

# Time stamping parameters for eth0:
# Capabilities:
# 	hardware-transmit
# 	software-transmit
# 	hardware-receive
# 	software-receive
# 	software-system-clock
# 	hardware-raw-clock
# PTP Hardware Clock: 0
# Hardware Transmit Timestamp Modes:
# 	off
# 	on
# Hardware Receive Filter Modes:
# 	none
# 	all
```

# Examples

**Example 1: Real-Time Traffic Only**

```bash
# Allocate all slots to tc-0
sudo insmod genet.ko slot_masks=0xffffffff,0,0,0,0 pkt_size=1230

# Configure pre-buffering heap for tc-0
sudo tc qdisc add dev eth0 root handle 1: mqprio num_tc 2 \
    map 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 \
    queues 1@0 1@1 \
    hw 0
sudo tc qdisc replace dev eth0 parent 1:1 etf clockid CLOCK_TAI delta 150000

# Synchronize system clock
sudo systemctl stop systemd-timesyncd
sudo phc2sys -s /dev/ptp0 -O 0 -m

# Send strictly periodic traffic
sudo ./test/cyclic 100000 200000 100

# Receiver should see strictly 100μs period
sudo tcpdump -Q in -ttt -ni eth0 --time-stamp-precision=nano -j adapter_unsynced -s 8

#  00:00:00.000100000  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000099992  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000099992  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000100000  [|ether]
```

**Example 2: Heterogeneous Traffic Management**

```bash
# Allocate for isolation
# tc-0: [■-------------------------------] (slot 0)
# tc-1: [□■---------------■--------------] (slot 1, slot 17)
# tc-2: [□□○○○○○○○○○○○○○○○□○○○○○○○○○○○○○○] (slot 2-16, 18-31)
# Repeats every 3.2ms cycle (32 slots × 100μs)

sudo insmod genet.ko slot_masks=0x01,0x20002,0,0,0 pkt_size=1230

# Configure pre-buffering for tc-0, tc-1
sudo tc qdisc del dev eth0 root
sudo tc qdisc add dev eth0 root handle 1: mqprio num_tc 3 \
    map 2 2 2 2 2 2 2 2 0 0 0 0 1 1 1 1 \
    queues 1@0 1@1 1@2 \
    hw 0
sudo tc qdisc replace dev eth0 parent 1:1 etf clockid CLOCK_TAI delta 150000
sudo tc qdisc replace dev eth0 parent 1:2 etf clockid CLOCK_TAI delta 150000

# Synchronize system clock
sudo systemctl stop systemd-timesyncd
sudo phc2sys -s /dev/ptp0 -O 0 -m

sudo ./test/cyclic 100000 200000 100 0
# Send strictly periodic traffic (tc-2: best-effort)
# Expect: no guarantee
#  00:00:00.000140000  [|ether]
#  00:00:00.000099992  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000100000  [|ether]
#  00:00:00.000120000  [|ether]
#  00:00:00.000080000  [|ether]

sudo ./test/cyclic 100000 200000 100 8
# Send strictly periodic traffic (tc-0: real-time)
# Expect: strictly 32us period (1-slot per-cycle)
#  00:00:00.000320000  [|ether]
#  00:00:00.000319992  [|ether]
#  00:00:00.000320000  [|ether]
#  00:00:00.000320000  [|ether]
#  00:00:00.000320000  [|ether]
#  00:00:00.000320000  [|ether]

sudo ./test/cyclic 100000 200000 100 12
# Send strictly periodic traffic (tc-1: real-time)
# Expect: strictly 16us period (2-slot per-cycle)
#  00:00:00.000160000  [|ether]
#  00:00:00.000160000  [|ether]
#  00:00:00.000159992  [|ether]
#  00:00:00.000160000  [|ether]
#  00:00:00.000160000  [|ether]
#  00:00:00.000160000  [|ether]
```

**Example 3: TSN Testbed Integration**

``` bash
# Talker (KeepON) <---> Switch (TTTech) <---> Listener (igb)
# Default set-up (Rt: 0-30 slots, BE: 31-32 slots)
sudo insmod genet.ko pkt_size=1230

sudo tc qdisc del dev eth0 root
sudo tc qdisc add dev eth0 root handle 1: mqprio num_tc 2 \
    map 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 \
    queues 1@0 1@1 \
    hw 0
sudo tc qdisc replace dev eth0 parent 1:1 etf clockid CLOCK_TAI delta 150000

# Synchronize with TSN GM
sudo systemctl stop systemd-timesyncd
sudo phc2sys -s /dev/ptp0 -O 0 -m
sudo ptp4l -i eth0 -f Tool/bcmgenet/test/gptp.cfg -m -P
# Results: around 5us sync offset
# ptp4l[13250.091]: rms 3626 max 4716 freq +92365 +/- 5054 delay 79386 +/-   0
# ptp4l[13251.092]: rms 5936 max 14700 freq +95810 +/- 7737 delay 80152 +/-   0
# ptp4l[13252.092]: rms 5135 max 9518 freq +93921 +/- 7060 delay 80152 +/-   0
# ptp4l[13253.092]: rms 3042 max 5178 freq +92349 +/- 4050 delay 80152 +/-   0
# ptp4l[13254.092]: rms 6516 max 11084 freq +90774 +/- 8833 delay 79386 +/-   0
# ptp4l[13255.092]: rms 6580 max 12376 freq +87635 +/- 8622 delay 79386 +/-   0

# Send strictly periodic traffic (1ms) (tc-0: real-time)
# PCP = 5, VLAN = 0
sudo ./test/cyclic 1000000 200000 100 8 0 5 0

# [1] Without TSN schedule
# Receiver: 
# 1520607476.439987235  [|ether]
# 1520607476.440987549  [|ether]
# 1520607476.441987511  [|ether]
# 1520607476.442987472  [|ether]
# 1520607476.443987426  [|ether]
# 1520607476.444987380  [|ether]


# [2] Set TSN schedule (TTTech Evaluation Board)
# Cycle:  1ms (1/1000)
# queue1: [<--close--><--40us---><-----920us------>]
# queue2: [<--close--><--40us---><---------------->]
# queue5: [<--40us---><--close-------------------->]

# test.cfg:
sgs 40000 0x20 # queue 5   (for real-time)
sgs 40000 0x03 # queue 1-2 (for BE and PTP)

tsntool st wrcl sw0p2 test.cfg
tsntool st configure 0.0 1/1000 0 sw0p2

# [2] Enable TSN schedule (≤ 10ns jitter)
# Receiver:
# 1520607628.694000238  [|ether]
# 1520607628.695000240  [|ether]
# 1520607628.696000241  [|ether]
# 1520607628.697000243  [|ether]
# 1520607628.698000236  [|ether]
# 1520607628.699000238  [|ether]
```
