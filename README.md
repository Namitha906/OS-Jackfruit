# Multi-Container Runtime

## 1. Team Information

- Member 1: Namitha N Reddy  
- SRN: PES2UG24AM098
- Member 3: Nanditha A
- SRN: PES2UG24AM100

---

## 2. Build, Load, and Run Instructions

### Build

make


### Start Supervisor

sudo ./engine supervisor ./rootfs-base


### Run Containers

sudo ./engine start c1 ./rootfs-alpha ls
sudo ./engine start c2 ./rootfs-beta ls


### List Containers

sudo ./engine ps


### Stop Container

sudo ./engine stop c1


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

![Alpha vs Beta](add-image-link-1)

Two CPU-bound containers are executed concurrently using the `yes` command. Different scheduling priorities are assigned using `nice` values (0 and 10). The container with lower nice value (higher priority) receives a greater share of CPU time, as observed in the `top` output.

---

#### Experiment 2: CPU-bound vs I/O-bound workload

![CPU vs IO](![WhatsApp Image 2026-04-13 at 11 35 53 AM (2)](https://github.com/user-attachments/assets/1884832a-8412-4213-82b7-c39eae1c7c9f)
![WhatsApp Image 2026-04-13 at 11 35 53 AM (1)](https://github.com/user-attachments/assets/3581f310-d319-422b-860e-4eccf1cb8a8b)
![WhatsApp Image 2026-04-13 at 11 35 53 AM](https://github.com/user-attachments/assets/0d52a118-d6ad-4fd8-9674-cc41c548d70b)
)

A CPU-bound container (`yes > /dev/null`) is executed alongside an I/O-bound container (`sleep 1`). The CPU-bound process consumes significantly more CPU resources, while the I/O-bound process remains mostly idle. This demonstrates how the Linux scheduler prioritizes CPU-intensive workloads differently from I/O-bound ones.




---

## 4. Engineering Analysis

(To be added)

---

## 5. Design Decisions and Tradeoffs

(To be added)

---

## 6. Scheduler Experiment Results

(To be added)
