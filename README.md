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




---

## 4. Engineering Analysis

(To be added)

---

## 5. Design Decisions and Tradeoffs

(To be added)

---

## 6. Scheduler Experiment Results

(To be added)
