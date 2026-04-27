#ifndef STORAGE_H
#define STORAGE_H

#include <string>
#include <vector>
#include "../common/types.h"

class PersistentStorage {
public:
    explicit PersistentStorage(const std::string& baseName);

    bool save(int currentTerm,
              int votedFor,
              int lastIncludedIndex,
              int lastIncludedTerm,
              const std::vector<LogEntry>& logs);

    bool load(int& currentTerm,
              int& votedFor,
              int& lastIncludedIndex,
              int& lastIncludedTerm,
              std::vector<LogEntry>& logs);

private:
    std::string stateFileName_;
};

#endif