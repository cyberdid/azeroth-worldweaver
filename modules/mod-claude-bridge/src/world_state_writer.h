#ifndef MOD_CLAUDE_BRIDGE_WORLD_STATE_WRITER_H
#define MOD_CLAUDE_BRIDGE_WORLD_STATE_WRITER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class WorldStateWriter
{
public:
    bool Start(std::string logDirectory, std::string telemetryDirectory,
        std::size_t eventQueueLimit, std::size_t eventLogBytes);
    void Stop();

    void PublishSnapshot(std::string groupKey, std::string json);
    void PublishEvent(std::string groupKey, std::string eventJson);
    void PublishChatLine(std::string botName, std::string channel,
        std::string sender, std::string text);

    std::uint64_t GetDroppedEventCount() const { return _droppedEvents.load(); }
    std::uint64_t GetDroppedEventCount(std::string const& groupKey) const;
    std::uint64_t GetDroppedRecordCount() const { return _droppedRecords.load(); }
    std::uint64_t GetDroppedSnapshotCount() const { return _droppedSnapshots.load(); }

private:
    struct PendingRecord
    {
        std::string groupKey;
        std::string json;
        std::string botName;
        std::string channel;
        std::string sender;
        std::string text;
        bool isChatLine = false;
    };

    void Run();
    void WriteSnapshot(std::string const& groupKey, std::string const& json);
    void WriteEvent(PendingRecord const& record);
    void IncrementDroppedEvent(std::string const& groupKey);
    void WriteChatLine(std::string const& botName, std::string const& channel,
        std::string const& sender, std::string const& text);
    bool RotateEventLog(std::string const& path, std::size_t incomingBytes);

    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::map<std::string, std::string> _pendingSnapshots;
    std::deque<PendingRecord> _pendingRecords;
    std::thread _worker;
    std::string _logDirectory;
    std::string _telemetryDirectory;
    std::size_t _eventQueueLimit = 256;
    std::size_t _eventLogBytes = 4 * 1024 * 1024;
    bool _stopping = false;
    std::atomic<std::uint64_t> _droppedEvents{0};
    std::atomic<std::uint64_t> _droppedRecords{0};
    std::atomic<std::uint64_t> _droppedSnapshots{0};
    mutable std::mutex _dropCounterMutex;
    std::unordered_map<std::string, std::uint64_t> _droppedEventsByGroup;
};

WorldStateWriter& GetWorldStateWriter();

#endif
