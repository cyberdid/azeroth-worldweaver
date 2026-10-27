#include "world_state_writer.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace
{
    std::mutex chatFileMutex;

    std::string Flatten(std::string value)
    {
        for (char& c : value)
            if (c == '\n' || c == '\r' || c == '\0')
                c = ' ';
        return value;
    }

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::string SafeGroupKey(std::string value)
    {
        for (char& c : value)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                c = '_';
        return value.empty() ? "unknown" : value;
    }
}

WorldStateWriter& GetWorldStateWriter()
{
    static WorldStateWriter writer;
    return writer;
}

std::uint64_t WorldStateWriter::GetDroppedEventCount(std::string const& groupKey) const
{
    std::lock_guard<std::mutex> lock(_dropCounterMutex);
    auto found = _droppedEventsByGroup.find(groupKey);
    return found == _droppedEventsByGroup.end() ? 0 : found->second;
}

void WorldStateWriter::IncrementDroppedEvent(std::string const& groupKey)
{
    _droppedEvents.fetch_add(1);
    std::lock_guard<std::mutex> lock(_dropCounterMutex);
    ++_droppedEventsByGroup[groupKey];
}

bool WorldStateWriter::Start(std::string logDirectory, std::string telemetryDirectory,
    std::size_t eventQueueLimit, std::size_t eventLogBytes)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_worker.joinable())
        return true;

    _logDirectory = std::move(logDirectory);
    _telemetryDirectory = telemetryDirectory.empty() ? _logDirectory : std::move(telemetryDirectory);
    _eventQueueLimit = std::min<std::size_t>(std::max<std::size_t>(eventQueueLimit, 8), 8192);
    _eventLogBytes = std::min<std::size_t>(std::max<std::size_t>(eventLogBytes, 64 * 1024), 128 * 1024 * 1024);
    _stopping = false;
    _worker = std::thread(&WorldStateWriter::Run, this);
    return true;
}

void WorldStateWriter::Stop()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_worker.joinable())
            return;
        _stopping = true;
    }
    _condition.notify_one();
    _worker.join();
}

void WorldStateWriter::PublishSnapshot(std::string groupKey, std::string json)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping || !_worker.joinable())
        {
            _droppedSnapshots.fetch_add(1);
            return;
        }
        _pendingSnapshots[SafeGroupKey(std::move(groupKey))] = std::move(json);
    }
    _condition.notify_one();
}

void WorldStateWriter::PublishEvent(std::string groupKey, std::string eventJson)
{
    std::string safeGroupKey = SafeGroupKey(std::move(groupKey));
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping || !_worker.joinable())
        {
            IncrementDroppedEvent(safeGroupKey);
            _droppedRecords.fetch_add(1);
            return;
        }
        if (_pendingRecords.size() >= _eventQueueLimit)
        {
            IncrementDroppedEvent(safeGroupKey);
            _droppedRecords.fetch_add(1);
            return;
        }

        PendingRecord record;
        record.groupKey = std::move(safeGroupKey);
        record.json = std::move(eventJson);
        _pendingRecords.push_back(std::move(record));
    }
    _condition.notify_one();
}

void WorldStateWriter::PublishChatLine(std::string botName, std::string channel,
    std::string sender, std::string text)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping || !_worker.joinable())
        {
            _droppedRecords.fetch_add(1);
            return;
        }

        if (_pendingRecords.size() >= _eventQueueLimit)
        {
            auto event = std::find_if(_pendingRecords.begin(), _pendingRecords.end(),
                [](PendingRecord const& record) { return !record.isChatLine; });
            if (event != _pendingRecords.end())
            {
                std::string const droppedGroup = event->groupKey;
                _pendingRecords.erase(event);
                IncrementDroppedEvent(droppedGroup);
                _droppedRecords.fetch_add(1);
            }
            else
            {
                _droppedRecords.fetch_add(1);
                return;
            }
        }

        PendingRecord record;
        record.botName = std::move(botName);
        record.channel = std::move(channel);
        record.sender = std::move(sender);
        record.text = std::move(text);
        record.isChatLine = true;
        _pendingRecords.push_back(std::move(record));
    }
    _condition.notify_one();
}

void WorldStateWriter::Run()
{
    std::error_code error;
    std::filesystem::create_directories(_logDirectory, error);
    error.clear();
    std::filesystem::create_directories(_telemetryDirectory, error);

    for (;;)
    {
        std::map<std::string, std::string> snapshots;
        std::deque<PendingRecord> records;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _condition.wait(lock, [this]
            {
                return _stopping || !_pendingSnapshots.empty() || !_pendingRecords.empty();
            });
            if (_stopping && _pendingSnapshots.empty() && _pendingRecords.empty())
                return;
            snapshots.swap(_pendingSnapshots);
            std::size_t const batchSize = std::min<std::size_t>(_pendingRecords.size(), 64);
            for (std::size_t i = 0; i < batchSize; ++i)
            {
                records.push_back(std::move(_pendingRecords.front()));
                _pendingRecords.pop_front();
            }
        }

        for (auto const& snapshot : snapshots)
            WriteSnapshot(snapshot.first, snapshot.second);
        for (PendingRecord const& record : records)
        {
            if (record.isChatLine)
                WriteChatLine(record.botName, record.channel, record.sender, record.text);
            else
                WriteEvent(record);
        }
    }
}

void WorldStateWriter::WriteSnapshot(std::string const& groupKey, std::string const& json)
{
    std::filesystem::path const target = std::filesystem::path(_telemetryDirectory) / ("world-state-" + groupKey + ".json");
    std::filesystem::path const temporary = target.string() + ".tmp";
    std::error_code error;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            _droppedSnapshots.fetch_add(1);
            return;
        }
        out << json << '\n';
        out.flush();
        if (!out)
        {
            _droppedSnapshots.fetch_add(1);
            out.close();
            std::filesystem::remove(temporary, error);
            return;
        }
        out.close();
        if (!out)
        {
            _droppedSnapshots.fetch_add(1);
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error)
    {
        _droppedSnapshots.fetch_add(1);
        std::filesystem::remove(temporary, error);
    }
}

void WorldStateWriter::WriteEvent(PendingRecord const& record)
{
    if (record.json.size() + 1 > _eventLogBytes)
    {
        IncrementDroppedEvent(record.groupKey);
        _droppedRecords.fetch_add(1);
        return;
    }
    std::filesystem::path const path = std::filesystem::path(_telemetryDirectory) / ("world-events-" + record.groupKey + ".jsonl");
    if (!RotateEventLog(path.string(), record.json.size() + 1))
    {
        IncrementDroppedEvent(record.groupKey);
        _droppedRecords.fetch_add(1);
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
    {
        IncrementDroppedEvent(record.groupKey);
        _droppedRecords.fetch_add(1);
        return;
    }
    out << record.json << '\n';
    out.flush();
    if (!out)
    {
        IncrementDroppedEvent(record.groupKey);
        _droppedRecords.fetch_add(1);
        return;
    }
    out.close();
    if (!out)
    {
        IncrementDroppedEvent(record.groupKey);
        _droppedRecords.fetch_add(1);
    }
}

void WorldStateWriter::WriteChatLine(std::string const& botName, std::string const& channel,
    std::string const& sender, std::string const& text)
{
    std::string filename = Lower(SafeGroupKey(botName)) + "-chat.log";
    std::filesystem::path const path = std::filesystem::path(_logDirectory) / filename;
    std::lock_guard<std::mutex> lock(chatFileMutex);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out)
    {
        _droppedRecords.fetch_add(1);
        return;
    }
    out << std::time(nullptr) << '|' << channel << '|' << sender << '|' << Flatten(text) << '\n';
    out.flush();
    if (!out)
    {
        _droppedRecords.fetch_add(1);
        return;
    }
    out.close();
    if (!out)
    {
        _droppedRecords.fetch_add(1);
    }
}

bool WorldStateWriter::RotateEventLog(std::string const& path, std::size_t incomingBytes)
{
    std::error_code error;
    std::uintmax_t currentBytes = std::filesystem::file_size(path, error);
    if (error || currentBytes + incomingBytes <= _eventLogBytes)
        return true;

    std::filesystem::path previous = path + ".1";
    std::filesystem::remove(previous, error);
    if (error)
        return false;
    error.clear();
    std::filesystem::rename(path, previous, error);
    return !error;
}
