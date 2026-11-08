#include "stdafx.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "xrServer_Objects.h"
#include "xrServer.h"
#include "xrmessages.h"
#include "ai_space.h"

void xrServer::Perform_destroy(CSE_Abstract* object, u32 mode)
{
    R_ASSERT(object);
    R_ASSERT(object->ID_Parent == 0xffff);

#ifdef DEBUG
#ifdef SLOW_VERIFY_ENTITIES
    verify_entities();
#endif
#endif

    while (!object->children.empty())
    {
        const u16 child_id = object->children.back();
        CSE_Abstract* child = game->get_entity_from_eid(child_id);
        R_ASSERT2(child, make_string("child registered but not found [%d]", child_id));

        if (child->ID_Parent != object->ID)
        {
#ifdef DEBUG
            Msg("[LSS][SLS_CLEAR] repairing parent for child [%d][%s]: [%d] -> [%d]",
                child_id, child->name_replace(), child->ID_Parent, object->ID);
#endif

            CSE_Abstract* previous_parent = game->get_entity_from_eid(child->ID_Parent);
            if (previous_parent && previous_parent != object)
            {
                auto previous_it = std::find(
                    previous_parent->children.begin(),
                    previous_parent->children.end(),
                    child_id);
                if (previous_it != previous_parent->children.end())
                    previous_parent->children.erase(previous_it);
            }

            child->ID_Parent = object->ID;
        }

        Perform_reject(child, object, 2 * NET_Latency);
#ifdef DEBUG
#ifdef SLOW_VERIFY_ENTITIES
        verify_entities();
#endif
#endif
        Perform_destroy(child, mode);
    }

    u16 object_id = object->ID;
    entity_Destroy(object);

#ifdef DEBUG
#ifdef SLOW_VERIFY_ENTITIES
    verify_entities();
#endif
#endif

    NET_Packet P;
    P.w_begin(M_EVENT);
    P.w_u32(Device.dwTimeGlobal - 2 * NET_Latency);
    P.w_u16(GE_DESTROY);
    P.w_u16(object_id);
    SendBroadcast(BroadcastCID, P, mode);
}

void xrServer::SLS_Clear()
{
#if 0
    Msg("SLS-CLEAR : %d objects");
    xrS_entities::const_iterator I = entities.begin();
    xrS_entities::const_iterator E = entities.end();
    for (; I != E; ++I)
        Msg("entity to destroy : [%d][%s][%s]", (*I).second->ID, (*I).second->name(), (*I).second->name_replace());
#endif

    u32 mode = net_flags(TRUE, TRUE);
    while (!entities.empty())
    {
        bool found = false;
        xrS_entities::const_iterator I = entities.begin();
        xrS_entities::const_iterator E = entities.end();
        for (; I != E; ++I)
        {
            if ((*I).second->ID_Parent != 0xffff)
                continue;
            found = true;
            Perform_destroy((*I).second, mode);
            break;
        }
        R_ASSERT(found);
    }
}
