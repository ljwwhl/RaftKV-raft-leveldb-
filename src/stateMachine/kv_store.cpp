#include "kv_store.h"
#include <leveldb/write_batch.h>
#include <iostream>

KVStore::KVStore()
    : db_(nullptr) {
}

KVStore::~KVStore() {
    delete db_;
    db_ = nullptr;
}

bool KVStore::open(const std::string& dbPath) {
    dbPath_ = dbPath;

    leveldb::Options options;
    options.create_if_missing = true;

    leveldb::Status status = leveldb::DB::Open(options, dbPath, &db_);
    if (!status.ok()) {
        std::cerr << "[KVStore] open LevelDB failed: "
                  << status.ToString() << std::endl;
        return false;
    }

    std::cout << "[KVStore] open LevelDB success: "
              << dbPath << std::endl;

    return true;
}

void KVStore::apply(const LogEntry& entry) {
    if (db_ == nullptr) {
        std::cerr << "[KVStore] db not open" << std::endl;
        return;
    }

    leveldb::WriteOptions writeOptions;

    switch (entry.op) {
        case CommandType::PUT: {
            leveldb::Status status =
                db_->Put(writeOptions, entry.key, entry.value);

            if (!status.ok()) {
                std::cerr << "[KVStore] PUT failed: "
                          << status.ToString() << std::endl;
                return;
            }

            std::cout << "[APPLY] PUT key=" << entry.key
                      << ", value=" << entry.value
                      << ", term=" << entry.term
                      << ", index=" << entry.index << std::endl;
            break;
        }

        case CommandType::DEL: {
            leveldb::Status status =
                db_->Delete(writeOptions, entry.key);

            if (!status.ok()) {
                std::cerr << "[KVStore] DEL failed: "
                          << status.ToString() << std::endl;
                return;
            }

            std::cout << "[APPLY] DEL key=" << entry.key
                      << ", term=" << entry.term
                      << ", index=" << entry.index << std::endl;
            break;
        }

        case CommandType::NONE: {
            std::cout << "[APPLY] NOOP, term=" << entry.term
                      << ", index=" << entry.index << std::endl;
            break;
        }

        default:
            // CONFIG_JOINT / CONFIG_NEW 不应该由 KVStore apply
            break;
    }
}

bool KVStore::get(const std::string& key, std::string& value) const {
    if (db_ == nullptr) {
        return false;
    }

    leveldb::ReadOptions readOptions;
    leveldb::Status status = db_->Get(readOptions, key, &value);

    if (status.IsNotFound()) {
        return false;
    }

    if (!status.ok()) {
        std::cerr << "[KVStore] GET failed: "
                  << status.ToString() << std::endl;
        return false;
    }

    return true;
}

void KVStore::printAll() const {
    if (db_ == nullptr) {
        std::cout << "[KVStore] db not open" << std::endl;
        return;
    }

    std::cout << "===== LevelDB KVStore =====" << std::endl;

    leveldb::ReadOptions readOptions;
    leveldb::Iterator* it = db_->NewIterator(readOptions);

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::cout << it->key().ToString()
                  << " => "
                  << it->value().ToString()
                  << std::endl;
    }

    if (!it->status().ok()) {
        std::cerr << "[KVStore] iterator error: "
                  << it->status().ToString() << std::endl;
    }

    delete it;
}

std::map<std::string, std::string> KVStore::dump() const {
    std::map<std::string, std::string> data;

    if (db_ == nullptr) {
        return data;
    }

    leveldb::ReadOptions readOptions;
    leveldb::Iterator* it = db_->NewIterator(readOptions);

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        data[it->key().ToString()] = it->value().ToString();
    }

    if (!it->status().ok()) {
        std::cerr << "[KVStore] dump iterator error: "
                  << it->status().ToString() << std::endl;
    }

    delete it;
    return data;
}

void KVStore::loadFromSnapshot(const std::map<std::string, std::string>& data) {
    if (db_ == nullptr) {
        std::cerr << "[KVStore] db not open" << std::endl;
        return;
    }

    leveldb::WriteOptions writeOptions;

    // 简化做法：先删除当前所有 key，再写入 snapshot 数据
    std::map<std::string, std::string> oldData = dump();

    leveldb::WriteBatch batch;

    for (const auto& p : oldData) {
        batch.Delete(p.first);
    }

    for (const auto& p : data) {
        batch.Put(p.first, p.second);
    }

    leveldb::Status status = db_->Write(writeOptions, &batch);
    if (!status.ok()) {
        std::cerr << "[KVStore] load snapshot failed: "
                  << status.ToString() << std::endl;
        return;
    }

    std::cout << "[KVStore] load snapshot success, kvCount="
              << data.size() << std::endl;
}