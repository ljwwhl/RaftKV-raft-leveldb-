#include "storage.h"
#include "../raftCore/raft_node.h"
#include <fstream>
#include <iostream>
#include <sstream>

PersistentStorage::PersistentStorage(const std::string& baseName)
    : stateFileName_("./state/"+baseName + "_state.txt") {
}

bool PersistentStorage::save(int currentTerm,
                             int votedFor,
                             int lastIncludedIndex,
                             int lastIncludedTerm,
                             const std::vector<LogEntry>& logs) {
    std::ofstream ofs(stateFileName_.c_str(), std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "[Storage] open file for write failed: "
                  << stateFileName_ << std::endl;
        return false;
    }

    ofs << "currentTerm " << currentTerm << "\n";
    ofs << "votedFor " << votedFor << "\n";
    ofs << "lastIncludedIndex " << lastIncludedIndex << "\n";
    ofs << "lastIncludedTerm " << lastIncludedTerm << "\n";
    ofs << "logCount " << logs.size() << "\n";

    for (const auto& e : logs) {
        ofs << e.index << " "
            << e.term << " "
            << static_cast<int>(e.op) << " "
            << e.key << " "
            << e.value << " "
            << encodeConfig(e.oldConfig) << " "
            << encodeConfig(e.newConfig)
            << "\n";
    }

    return true;
}

bool PersistentStorage::load(int& currentTerm,
                             int& votedFor,
                             int& lastIncludedIndex,
                             int& lastIncludedTerm,
                             std::vector<LogEntry>& logs) {
    std::ifstream ifs(stateFileName_.c_str());
    if (!ifs.is_open()) {
        return false;
    }

    std::string label;
    size_t logCount = 0;

    ifs >> label >> currentTerm;
    if (ifs.fail() || label != "currentTerm") return false;

    ifs >> label >> votedFor;
    if (ifs.fail() || label != "votedFor") return false;

    ifs >> label >> lastIncludedIndex;
    if (ifs.fail() || label != "lastIncludedIndex") return false;

    ifs >> label >> lastIncludedTerm;
    if (ifs.fail() || label != "lastIncludedTerm") return false;

    ifs >> label >> logCount;
    if (ifs.fail() || label != "logCount") return false;

    logs.clear();

    std::string line;
    std::getline(ifs, line);

    for (size_t i = 0; i < logCount; i++) {
        if (!std::getline(ifs, line)) {
            return false;
        }

        if (line.empty()) {
            i--;
            continue;
        }

        std::istringstream iss(line);

        LogEntry e;
        int opInt = 0;

        iss >> e.index >> e.term >> opInt >> e.key >> e.value;
        if (iss.fail()) {
            return false;
        }

        e.op = static_cast<CommandType>(opInt);
        e.oldConfig = decodeConfig(iss);
        e.newConfig = decodeConfig(iss);

        logs.push_back(e);
    }

    return true;
}