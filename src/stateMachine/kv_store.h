#ifndef KV_STORE_H
#define KV_STORE_H

#include <string>
#include <map>
#include <leveldb/db.h>
#include "../common/types.h"

class KVStore {
public:
    KVStore();
    ~KVStore();

    bool open(const std::string& dbPath);

    void apply(const LogEntry& entry);

    bool get(const std::string& key, std::string& value) const;

    void printAll() const;

    // snapshot 需要用到：导出当前状态机
    std::map<std::string, std::string> dump() const;

    // snapshot 安装时用到：从快照恢复状态机
    void loadFromSnapshot(const std::map<std::string, std::string>& data);

private:
    leveldb::DB* db_;
    std::string dbPath_;
};

#endif