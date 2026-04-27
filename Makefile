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

g++ -std=c++11 -pthread \
src/client_main.cpp \
src/client/clerk.cpp \
src/rpc/tcp_client.cpp \
-o client

mkdir db && mkdir state