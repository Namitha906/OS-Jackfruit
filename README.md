# Multi-Container Runtime

## 1. Team Information

- Member 1: Namitha N Reddy  
- SRN: PES2UG24AM098
- Member 2: Nanditha A
- SRN: PES2UG24AM100

---

# OS-Jackfruit Project – Task Execution Guide

##  Setup

```bash
cd OS-Jackfruit
make
sudo ./engine supervisor ./rootfs-alpha
```

---

#Task 1: Start a Container

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-alpha /bin/sh
```

Check:

```bash
sudo ./engine ps
```

Output:

```
ID   PID    STATUS
c1   <pid>  running
```

---

# Task 2: Stop Container

```bash
sudo ./engine stop alpha
sudo ./engine ps
```

Output:

```
c1   <pid>  stopped
```

---

# Task 3: Logging (2 Terminals)

### Terminal 1 (Supervisor)

```bash
sudo ./engine supervisor ./rootfs-alpha
```

### Terminal 2 (Run container)

```bash
sudo ./engine start c1 ./rootfs-alpha ls
cat logs/c1.log
```

Output:

```
bin
dev
etc
home
...
```

---

# Task 4: CPU Scheduling (CPU Hog)

```bash
sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog
```

Check CPU usage:

```bash
top
```

Expected:

```
cpu_hog  → high CPU usage (~90%+)
```

---

# Task 5: I/O Simulation (IO Pulse)

```bash
sudo ./engine start io1 ./rootfs-alpha /io_pulse
```

Check zombie state:

```bash
ps aux | grep Z
```

Expected:

```
Z (zombie process visible)
```

---

# Task 6: Memory Monitoring (Soft & Hard Limits)

## Step 1: Load Kernel Module

```bash
sudo insmod monitor.ko
```

---

## Step 2: Run Container with Limits

```bash
sudo ./engine run alpha ./rootfs-alpha /memory_hog --soft-mib 5 --hard-mib 15
```

---

## Step 3: Check Kernel Logs

```bash
sudo dmesg | tail
```

---

##  Output

```
[container_monitor] Registered container=alpha pid=<pid> soft=5242880 hard=10485760
[container_monitor] SOFT LIMIT container=alpha pid=<pid> rss=5316608 limit=5242880
[container_monitor] HARD LIMIT container=alpha pid=<pid> rss=10559488 limit=10485760
```

---

# Cleanup

```bash
sudo ./engine stop alpha 2>/dev/null
sudo rmmod monitor 2>/dev/null
rm -rf logs/*
make clean
```

---

#  Summary

* Containers are created and managed using `engine`
* CPU hog simulates heavy CPU load
* IO pulse demonstrates process states
* Kernel module monitors memory usage
* Soft limit → warning
* Hard limit → enforcement

---


## 3. Demo with Screenshots
### 1. Multi-container supervision and metadata tracking

![Multi-container](![WhatsApp Image 2026-04-11 at 7 19 53 PM](https://github.com/user-attachments/assets/6ad4f597-e813-4df8-9bd7-370282571bb2)
)

Two containers (c1 and c2) are launched under a single supervisor process. The output shows multiple container instances with unique PIDs. The `ps` command displays container metadata including ID, PID, and status (running/stopped), confirming correct tracking and lifecycle management by the supervisor.

### 2. CLI and IPC communication

![CLI Terminal 1](![WhatsApp Image 2026-04-11 at 7 50 42 PM](https://github.com/user-attachments/assets/c4995f40-6c82-48d0-ba47-b1b783870bdf)
)
![CLI Terminal 2](![WhatsApp Image 2026-04-11 at 7 51 33 PM](https://github.com/user-attachments/assets/fe3e7649-f758-470a-8f0b-e6c8c15ce7fc)
)

The supervisor runs as a long-lived process and listens on a UNIX domain socket. CLI commands such as `start` are issued from a separate terminal and sent to the supervisor through the control channel. The supervisor processes the request and responds appropriately, demonstrating successful inter-process communication (IPC) between the CLI and the supervisor.

### 3. Bounded-buffer logging and IPC

![Logging Terminal 1](![WhatsApp Image 2026-04-12 at 7 01 01 PM](https://github.com/user-attachments/assets/ee60db53-ed1b-4206-98ef-1b1f15866f5b)
)
![Logging Terminal 2](![WhatsApp Image 2026-04-12 at 7 01 01 PM (1)](https://github.com/user-attachments/assets/a8071768-871c-47cf-bad5-2f357f18b18c)
)

Container output (stdout/stderr) is captured using pipes and processed through a producer-consumer logging pipeline. A producer thread reads data from the container’s output stream and inserts it into a bounded buffer, while a consumer thread writes the data to persistent per-container log files.

The screenshot shows the output of a container (`ls` command) successfully captured in `logs/c1.log`, demonstrating correct logging, synchronization, and IPC between container processes and the supervisor.

### 4. Scheduling experiments


#### Experiment 1: CPU-bound workloads with different priorities

![Priority Scheduling](![WhatsApp Image 2026-04-13 at 12 02 16 PM](https://github.com/user-attachments/assets/9ee5e3da-1038-4bf0-9be2-f2ad48de572a)
)

Two CPU-bound processes (`yes`) were executed with different nice values (0 and 10). The `top` output shows that processes with lower nice value (higher priority) receive a larger share of CPU time, while higher nice value processes receive less CPU.

This demonstrates how the Linux Completely Fair Scheduler (CFS) distributes CPU time based on process priority.

---

#### Experiment 2: CPU-bound vs I/O-bound workload

![CPU vs IO]
(![WhatsApp Image 2026-04-13 at 11 35 53 AM (2)](https://github.com/user-attachments/assets/1884832a-8412-4213-82b7-c39eae1c7c9f)
![WhatsApp Image 2026-04-13 at 11 35 53 AM (1)](https://github.com/user-attachments/assets/3581f310-d319-422b-860e-4eccf1cb8a8b)
![WhatsApp Image 2026-04-13 at 11 35 53 AM](https://github.com/user-attachments/assets/0d52a118-d6ad-4fd8-9674-cc41c548d70b)
)

A CPU-bound container (`yes > /dev/null`) is executed alongside an I/O-bound container (`sleep 1`). The CPU-bound process consumes significantly more CPU resources, while the I/O-bound process remains mostly idle. This demonstrates how the Linux scheduler prioritizes CPU-intensive workloads differently from I/O-bound ones.

### 5. Clean Teardown

The system ensures proper cleanup of containers and resources during shutdown.

* Containers are started and stopped using the CLI, and their state is correctly reflected using the `ps` command.
* After stopping a container, no running processes remain (`ps aux | grep c1` shows no active processes).
* Zombie processes were checked using `ps aux | grep Z`, and none were found.
* Logging and supervisor threads terminate cleanly when containers exit.

### Conclusion

The runtime successfully performs clean teardown with:

* Proper container termination
* Accurate metadata updates
* No zombie or leftover processes
  ![WhatsApp Image 2026-04-14 at 11 11 47 AM](https://github.com/user-attachments/assets/34653923-87c0-40df-8838-b99bd57f6991)

### 6.Soft and Hard Limit

To evaluate memory control, a memory-intensive program (memory_hog) was executed inside a container with defined soft and hard limits.

<img width="818" height="576" alt="WhatsApp Image 2026-04-20 at 8 48 45 PM" src="https://github.com/user-attachments/assets/755cb5ca-4edd-40b1-95de-af8396153fe9" />




---

## 4. Engineering Analysis

This project demonstrates several core operating system concepts including process isolation, inter-process communication (IPC), kernel-user interaction, and scheduling behavior.

### Process Isolation (Namespaces)
Each container is created using Linux namespaces (PID, UTS, and mount namespaces). This ensures that processes inside a container have their own isolated environment, including separate process IDs and filesystem views. This mimics lightweight containerization similar to Docker.

### Supervisor Architecture
A central supervisor process manages all containers. It is responsible for:
- Creating containers using `clone()`
- Tracking container metadata (ID, PID, state)
- Handling user commands via a control interface

This reflects real-world container runtimes where a daemon manages multiple containers.

### Inter-Process Communication (IPC)
Two IPC mechanisms are used:
1. **Pipes** – to capture container stdout/stderr for logging
2. **Unix domain sockets** – for communication between CLI and supervisor

This separation ensures modularity and efficient communication.

### Logging System (Producer-Consumer Model)
Container outputs are not written directly to the terminal. Instead:
- Producer threads read output from pipes
- Data is inserted into a bounded buffer
- Consumer threads write logs to files

This prevents blocking and ensures no data loss under high load.

### Kernel-Level Monitoring
A kernel module monitors container memory usage. The supervisor registers containers using ioctl calls. The kernel periodically checks memory usage and enforces limits:
- Soft limit → warning
- Hard limit → process termination

This demonstrates interaction between user-space and kernel-space components.

### Scheduling Behavior
The project is used as a platform to observe Linux scheduling. By running CPU-bound and I/O-bound workloads with different priorities, we analyze how the scheduler allocates CPU time fairly.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** Use Linux namespaces instead of full virtualization  
**Tradeoff:** Lightweight but less secure than full VMs  
**Justification:** Efficient and aligns with modern container systems like Docker

---

### Supervisor Design
**Choice:** Single supervisor managing all containers  
**Tradeoff:** Centralized control can be a bottleneck  
**Justification:** Simplifies container tracking and control logic

---

### Logging System (Bounded Buffer)
**Choice:** Producer-consumer model with bounded buffer  
**Tradeoff:** Requires synchronization (mutex + condition variables)  
**Justification:** Prevents data loss and avoids blocking when handling high log volume

---

### IPC Mechanisms
**Choice:** Pipes + Unix domain sockets  
**Tradeoff:** More complex than a single IPC method  
**Justification:** Separates concerns (logging vs control communication)

---

### Kernel Monitoring (LKM)
**Choice:** Implement monitoring in kernel space  
**Tradeoff:** More complex and harder to debug  
**Justification:** Direct access to process memory usage and realistic OS-level enforcement

---

### Scheduling Experiments
**Choice:**  Use real Linux scheduler instead of simulating one  
**Tradeoff:** Less control over internal scheduling logic  
**Justification:** Provides real-world insights into Linux CFS behavior
---

## 6. Scheduler Experiment Results

### Experiment 1: CPU-bound workloads with different priorities

Two CPU-bound processes (`yes`) were executed with different nice values:
- Process 1: nice = 0 (higher priority)
- Process 2: nice = 10 (lower priority)

**Observation:**
The process with lower nice value consistently received more CPU time, as shown in the `top` output.

| Process | Nice Value | CPU Usage |
|--------|-----------|----------|
| yes (high priority) | 0 | Higher |
| yes (low priority)  | 10 | Lower |

**Conclusion:**
Linux Completely Fair Scheduler (CFS) allocates CPU time based on priority, favoring processes with lower nice values.

---

### Experiment 2: CPU-bound vs I/O-bound workloads

Two workloads were run simultaneously:
- CPU-bound: `yes > /dev/null`
- I/O-bound: `sleep 1`

**Observation:**
The CPU-bound process consumed most of the CPU, while the I/O-bound process used negligible CPU.

| Process Type | CPU Usage |
|-------------|----------|
| CPU-bound   | High     |
| I/O-bound   | Very Low |

**Conclusion:**
The scheduler prioritizes active CPU-demanding processes, while I/O-bound processes yield CPU time when waiting, improving system responsiveness.

---

### Overall Insight

The experiments demonstrate that:
- Linux scheduler balances fairness and efficiency
- Priority (nice value) directly affects CPU allocation
- CPU-bound processes dominate CPU usage
- I/O-bound processes allow better multitasking

These results align with the behavior of the Linux Completely Fair Scheduler (CFS).
