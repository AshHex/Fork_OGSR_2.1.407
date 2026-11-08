////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_news_queue.h
//	Created 	: 18.09.2026
//  Modified 	: 23.09.2026
//	Author		: StrawFlux
//	Description : ALife news queue
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_runtime_switch.h"

#include <deque>
#include <mutex>
#include <utility>

namespace ALifeNewsQueue
{
struct Event
{
    xr_string type;
    xr_string name1;
    xr_string name2;
    xr_string info;
};

inline std::mutex& event_mutex()
{
    static std::mutex value;
    return value;
}

inline std::deque<Event>& event_queue()
{
    static std::deque<Event> value;
    return value;
}

inline void clear()
{
    std::lock_guard<std::mutex> lock(event_mutex());
    event_queue().clear();
}

inline void push(LPCSTR type, LPCSTR name1, LPCSTR name2 = nullptr, LPCSTR info = nullptr)
{
    if (!ALIFE_ON)
        return;

    Event event;

    if (type)
        event.type = type;

    if (name1)
        event.name1 = name1;

    if (name2)
        event.name2 = name2;

    if (info)
        event.info = info;

    std::lock_guard<std::mutex> lock(event_mutex());

    constexpr u32 MAX_QUEUE_SIZE = 128;

    if (event_queue().size() >= MAX_QUEUE_SIZE)
        event_queue().pop_front();

    event_queue().push_back(std::move(event));
}

inline bool pop(Event& event)
{
    std::lock_guard<std::mutex> lock(event_mutex());

    if (event_queue().empty())
        return false;

    event = std::move(event_queue().front());
    event_queue().pop_front();

    return true;
}

inline u32 size()
{
    std::lock_guard<std::mutex> lock(event_mutex());
    return static_cast<u32>(event_queue().size());
}
}
