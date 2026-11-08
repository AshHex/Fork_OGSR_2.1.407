////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_loot_queue.h
//	Created 	: 19.09.2026
//  Modified 	: 19.09.2026
//	Author		: StrawFlux
//	Description : ALife loot queue
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_runtime_switch.h"
#include "alife_space.h"

#include <deque>
#include <mutex>
#include <utility>

namespace ALifeLootQueue
{
struct Event
{
    ALife::_OBJECT_ID npc_id = ALife::_OBJECT_ID(-1);
    xr_string npc_section;
    ALife::_OBJECT_ID item_id = ALife::_OBJECT_ID(-1);
    xr_string item_section;
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

inline void push(ALife::_OBJECT_ID npc_id, LPCSTR npc_section, ALife::_OBJECT_ID item_id, LPCSTR item_section)
{
    if (!ALIFE_ON)
        return;

    Event event;
    event.npc_id = npc_id;
    if (npc_section)
        event.npc_section = npc_section;
    event.item_id = item_id;
    if (item_section)
        event.item_section = item_section;

    std::lock_guard<std::mutex> lock(event_mutex());

    constexpr u32 MAX_QUEUE_SIZE = 256;
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
