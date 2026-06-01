#pragma once

#include <windows.h>
#include <climits>
#include <map>
#include <mutex>
#include <utility>

class FrameProcessGuard {
public:
    static constexpr LONGLONG kUnknownSampleTime = LLONG_MIN;

    bool HasSeenFrame(void* pData, int sourceId, LONGLONG sampleTime, DWORD now) {
        bool seenViaPointer = false;

        std::lock_guard<std::mutex> lock(m_mutex);
        PruneOldEntries(now);

        if (pData) {
            auto it = m_pointerEntries.find(pData);
            seenViaPointer = it != m_pointerEntries.end() &&
                it->second.sourceId != sourceId &&
                now - it->second.timestamp < kDuplicateWindowMs;
            m_pointerEntries[pData] = { now, sourceId };
        }

        if (sampleTime != kUnknownSampleTime) {
            auto key = std::make_pair(sourceId, sampleTime);
            auto it = m_timestampEntries.find(key);
            if (it != m_timestampEntries.end() && now - it->second < kTimestampWindowMs) {
                it->second = now;
                return true;
            }
            m_timestampEntries[key] = now;
        }

        return seenViaPointer;
    }

private:
    struct BufferTag {
        DWORD timestamp;
        int sourceId;
    };

    void PruneOldEntries(DWORD now) {
        for (auto it = m_pointerEntries.begin(); it != m_pointerEntries.end();) {
            if (now - it->second.timestamp > kPruneAfterMs) {
                it = m_pointerEntries.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_timestampEntries.begin(); it != m_timestampEntries.end();) {
            if (now - it->second > kPruneAfterMs) {
                it = m_timestampEntries.erase(it);
            } else {
                ++it;
            }
        }
    }

    static constexpr DWORD kDuplicateWindowMs = 15;
    static constexpr DWORD kTimestampWindowMs = 1000;
    static constexpr DWORD kPruneAfterMs = 1000;

    std::map<void*, BufferTag> m_pointerEntries;
    std::map<std::pair<int, LONGLONG>, DWORD> m_timestampEntries;
    std::mutex m_mutex;
};
