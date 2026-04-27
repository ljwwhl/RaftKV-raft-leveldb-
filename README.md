# RaftKV

RaftKV 是一个基于 Raft 共识算法实现的分布式 Key-Value 存储系统。项目使用 C++ 实现，支持 Leader 选举、日志复制、日志提交、状态机应用、快照、成员变更、LevelDB 持久化存储以及基于 epoll/Reactor 的网络通信模型。

当前版本主要用于学习和理解 Raft 协议的核心机制，以及分布式 KV 存储系统的基本实现流程。

---

## 一、项目功能

### 1. Raft 核心功能

- Leader 选举
- RequestVote RPC
- AppendEntries RPC
- 心跳机制
- 日志复制
- 多数派提交
- 状态机 apply
- Leader 崩溃后重新选举
- Follower 日志追赶
- `nextIndex[]` / `matchIndex[]` 日志同步优化

### 2. KV 存储功能

- `put <key> <value>`：写入 key-value
- `get <key>`：读取 key-value
- `del <key>`：删除 key
- 状态机底层使用 LevelDB 保存数据

### 3. 持久化功能

- 持久化当前任期 `currentTerm`
- 持久化投票节点 `votedFor`
- 持久化 Raft 日志
- 持久化快照边界：
  - `lastIncludedIndex`
  - `lastIncludedTerm`
- LevelDB 持久化状态机数据

### 4. 快照功能

- 日志达到一定数量后可以创建快照元数据
- 快照后裁剪旧日志
- 使用 LevelDB 保存状态机数据
- `raft_x_state.txt` 保存 Raft 元数据和剩余日志

### 5. 成员变更功能

支持基于 Raft Joint Consensus 的成员变更：

- 添加节点
- 删除节点
- `CONFIG_JOINT`
- `CONFIG_NEW`
- learner 节点
- leaving 节点
- removed 节点

成员变更流程：

```text
旧配置 oldConfig
        ↓
提交 CONFIG_JOINT
        ↓
进入联合配置 JOINT
        ↓
提交 CONFIG_NEW
        ↓
进入新稳定配置 STABLE
```

### 6. 网络功能

当前网络层使用 epoll + Reactor 模型：

- `EventLoop`
- `Channel`
- `TcpServer`
- `TcpConnection`
- `Buffer`
- 非阻塞 socket
- 长度前缀协议，解决 TCP 粘包和拆包问题

---

## 二、项目目录结构

```text
RAFT
├── cluster.conf                 # 集群配置文件
├── db/                          # LevelDB 数据目录
├── src
│   ├── client
│   │   ├── clerk.cpp            # 客户端请求封装
│   │   └── clerk.h
│   ├── common
│   │   └── types.h              # 公共类型定义
│   ├── persistence
│   │   ├── storage.cpp          # Raft 元数据持久化
│   │   └── storage.h
│   ├── raftCore
│   │   ├── raft_node.cpp        # Raft 核心逻辑
│   │   └── raft_node.h
│   ├── rpc
│   │   ├── Buffer.h             # 网络缓冲区
│   │   ├── Channel.cpp          # fd 事件封装
│   │   ├── Channel.h
│   │   ├── EventLoop.h          # epoll 事件循环
│   │   ├── TcpConnection.cpp    # TCP 连接对象
│   │   ├── TcpConnection.h
│   │   ├── TcpServer.cpp        # Reactor TCP 服务端
│   │   ├── TcpServer.h
│   │   ├── tcp_client.cpp       # TCP 客户端
│   │   ├── tcp_client.h
│   │   └── util.h
│   ├── stateMachine
│   │   ├── kv_store.cpp         # LevelDB 状态机
│   │   └── kv_store.h
│   ├── client_main.cpp          # 客户端入口
│   └── node_main.cpp            # Raft 节点入口
├── state        # Raft 元数据与日志文件
└── README.md
```

---

## 三、依赖环境

### 1. 编译环境

- Linux
- g++
- C++11
- pthread
- LevelDB

### 2. 安装 LevelDB

Ubuntu / Debian：

```bash
sudo apt update
sudo apt install libleveldb-dev
```

---

## 四、配置文件

项目使用 `cluster.conf` 作为集群地址簿。

示例：

```text
1 127.0.0.1 9001
2 127.0.0.1 9002
3 127.0.0.1 9003
4 127.0.0.1 9004
```

每一行格式：

```text
nodeId ip port
```

表示：

```text
Node 2 运行在 127.0.0.1:9002
```

注意：

`cluster.conf` 只是地址簿，不是真正的 Raft 集群配置来源。真正的成员变更必须通过 Raft 日志中的 `CONFIG_JOINT` 和 `CONFIG_NEW` 完成。

---

## 五、编译项目

### 1. 编译 Raft 节点

```bash
g++ -std=c++11 -pthread \
src/node_main.cpp \
src/raftCore/raft_node.cpp \
src/rpc/Channel.cpp \
src/rpc/TcpConnection.cpp \
src/rpc/TcpServer.cpp \
src/rpc/tcp_client.cpp \
src/stateMachine/kv_store.cpp \
src/persistence/storage.cpp \
-o node \
-lleveldb
```

### 2. 编译客户端

```bash
g++ -std=c++11 -pthread \
src/client_main.cpp \
src/client/clerk.cpp \
src/rpc/tcp_client.cpp \
-o client
```

---

## 六、启动集群

### 1. 初始 `cluster.conf`

```text
1 127.0.0.1 9001
2 127.0.0.1 9002
3 127.0.0.1 9003
```

### 2. 启动三个 Raft 节点

终端 1：

```bash
./node 127.0.0.1 9001
```

终端 2：

```bash
./node 127.0.0.1 9002
```

终端 3：

```bash
./node 127.0.0.1 9003
```

启动后，每个节点会从 `cluster.conf` 中查找自己的节点 ID，并构造 peer 列表。

---

## 七、启动客户端

```bash
./client
```

客户端默认读取当前目录下的：

```text
config/cluster.conf
```

客户端支持以下命令：

```text
put <key> <value>
get <key>
del <key>
add <nodeId>
remove <nodeId>
quit
```

---

## 八、基本 KV 操作

### 1. 写入数据

```text
put a 1
```

预期输出：

```text
put success
```

### 2. 查询数据

```text
get a
```

预期输出：

```text
value = 1
```

### 3. 删除数据

```text
del a
```

预期输出：

```text
delete success
```

再次查询：

```text
get a
```

预期输出：

```text
get failed or no key
```

---

## 九、添加节点

假设当前集群是：

```text
{1, 2, 3}
```

现在要添加 Node 4。

### 1. 修改 `cluster.conf`

将 node4 写入配置文件：

```text
1 127.0.0.1 9001
2 127.0.0.1 9002
3 127.0.0.1 9003
4 127.0.0.1 9004
```

### 2. 启动 node4 learner

```bash
./node 127.0.0.1 9004 --learner
```

learner 节点的特点：

- 可以接收日志
- 可以接收快照
- 不参与投票
- 不发起选举
- 不会干扰当前集群 term

### 3. 客户端执行 add

```text
add 4
```

Leader 会执行两阶段成员变更：

```text
CONFIG_JOINT old={1,2,3} new={1,2,3,4}
CONFIG_NEW   current={1,2,3,4}
```

成功后，node4 成为正式投票成员。

---

## 十、删除节点

假设当前集群是：

```text
{1, 2, 3, 4}
```

现在删除 Node 3。

客户端执行：

```text
remove 3
```

删除过程：

```text
CONFIG_JOINT old={1,2,3,4} new={1,2,4}
CONFIG_NEW   current={1,2,4}
```

Node 3 在 `CONFIG_JOINT` 阶段会进入 leaving 状态：

```text
leaving_ = true
votingMember_ = false
```

进入 leaving 后，Node 3 不再主动发起选举。

当 Node 3 应用 `CONFIG_NEW` 后，会进入 removed 状态：

```text
removed_ = true
votingMember_ = false
```

删除成功后，可以再从 `cluster.conf` 中删除 Node 3。

---

## 十一、节点本地命令

每个 Raft 节点终端支持：

```text
status
logs
```

### 1. 查看节点状态

```text
status
```

输出示例：

```text
Node 1 | state=LEADER | term=3 | votedFor=1 | leaderId=1 | commitIndex=5 | lastApplied=5 | lastIncludedIndex=0 | lastIncludedTerm=0 | logs=5
configState: STABLE
currentConfig: {1,2,3}
```

### 2. 查看日志

```text
logs
```

输出示例：

```text
---- Node 1 logs ----
snapshot: lastIncludedIndex=0, lastIncludedTerm=0
[index=1, term=3, op=0, key=a, value=1]
[index=2, term=3, op=0, key=b, value=2]
```

---

## 十二、持久化文件说明

### 1. Raft 元数据文件

文件位于：

```text
state/raft_1_state.txt
```

内容示例：

```text
currentTerm 3
votedFor -1
lastIncludedIndex 0
lastIncludedTerm 0
logCount 2
1 3 0 z 1 0 0
2 3 0 a 2 0 0
```

含义：

```text
currentTerm          当前任期
votedFor             当前任期投票给谁
lastIncludedIndex    快照覆盖的最后日志 index
lastIncludedTerm     快照覆盖的最后日志 term
logCount             剩余日志数量
```

日志格式：

```text
index term op key value oldConfig newConfig
```

其中：

```text
op=0 PUT
op=1 GET
op=2 DEL
op=3 NONE
op=4 CONFIG_JOINT
op=5 CONFIG_NEW
```

### 2. LevelDB 状态机数据

每个节点都有自己的 LevelDB 数据目录，例如：

```text
db/leveldb_node_1
db/leveldb_node_2
db/leveldb_node_3
```

Raft 日志提交后，状态机会将结果写入 LevelDB。

注意：

LevelDB 保存的是状态机结果，Raft 日志仍然是系统一致性的来源。

---

## 十三、快照说明

当前版本使用 LevelDB 保存状态机数据。


快照相关元数据保存在：

```text
raft_1_state.txt
```

包括：

```text
lastIncludedIndex
lastIncludedTerm
```

创建快照时，系统会：

```text
1. 保留 LevelDB 中的状态机数据
2. 更新 lastIncludedIndex
3. 更新 lastIncludedTerm
4. 裁剪已经被快照覆盖的日志
5. 持久化剩余日志
```

在联合配置 `JOINT` 阶段不创建快照，避免配置状态恢复错误。

---

## 十四、网络模型

项目网络层使用 epoll + Reactor 模式。

核心组件：

```text
EventLoop      epoll 事件循环
Channel        fd 事件封装
TcpServer      监听连接
TcpConnection  处理单个连接
Buffer         输入/输出缓冲区
```

网络协议使用长度前缀：

```text
4 字节长度 + 请求体
```

这样可以解决 TCP 粘包和拆包问题。

请求体仍然使用字符串协议，例如：

```text
CPUT a 1
CGET a
CDEL a
CADD 4 127.0.0.1 9004
CREMOVE 3
```

---

## 十五、常见问题

### 1. 节点无法启动，提示找不到自己

错误示例：

```text
self node not found in cluster.conf
```

说明当前节点的 `ip:port` 没有写入 `cluster.conf`。

检查配置文件中是否存在：

```text
nodeId ip port
```

例如：

```text
1 127.0.0.1 9001
```

### 2. learner 节点导致 term 增长

启动 learner 时必须带：

```bash
--learner
```

例如：

```bash
./node 127.0.0.1 9004 --learner
```

否则 node4 可能在正式加入前发起选举，导致旧集群 term 被拉高。

### 3. remove 后被删除节点一直发起选举

被删除节点在 `CONFIG_JOINT` 阶段应该进入 leaving 状态。

如果仍然出现：

```text
election timeout
becomes CANDIDATE
```

说明 `leaving_` 或 `votingMember_` 判断没有生效。

### 4. 修改 cluster.conf 后节点没有自动加入

这是正常的。

`cluster.conf` 只是地址簿。真正加入集群必须执行：

```text
add <nodeId>
```

真正删除节点必须执行：

```text
remove <nodeId>
```

---

## 十六、清理测试数据

如果需要重新测试，可以清理：

```bash
rm -rf db/leveldb_node_*
rm -f state/raft_*_state.txt
```

如果项目根目录下还有旧文件，也可以清理：

```bash
rm -f raft_*_state.txt raft_*_snapshot.txt
rm -rf leveldb_node_*
```

---

## 十七、后续可优化方向

- 使用 protobuf 替代字符串协议
- 实现异步日志复制
- 每个 peer 维护长连接
- 业务线程池处理 `handleRpcText`
- 多 Reactor 网络模型
- ReadIndex 批量读优化
- Snapshot 分块传输
- 更完善的成员变更持久化
- 更严格的崩溃恢复流程
- 使用配置版本号管理 `cluster.conf`